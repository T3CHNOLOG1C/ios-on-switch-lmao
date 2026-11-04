// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch (OLED, T210B01) battery for iPadOS: MAX17050 fuel gauge (0x36),
// BQ24193 charger (0x6B) and BM92T36 USB-PD controller (0x18), all on I2C1, published as an
// IOPMPowerSource. Re-read when one of them interrupts (the charger's and gauge's TegraGPIO pins, see kAlerts; the
// BM92T36's ALERT through SwitchPD, which owns that pin and its clear-on-read ALERT_STATUS and tells us about every
// alert), plus a slow re-read that bounds the staleness of what has no interrupt (voltage, current, temperature drift).
// There is no polled mode: alerts that cannot be attached, keep firing, or never reach the CPU panic.
//
// powerd (iOS 17) matches IOServiceMatching("IOPMPowerSource") with a first-match
// notification, then re-reads all properties whenever the source sends
// kIOPMMessageBatteryStatusHasChanged (IOPMPowerSource::updateStatus does). It requires
// BatteryInstalled == true plus CurrentCapacity and MaxCapacity, and shows
// round(100 * CurrentCapacity / MaxCapacity), so CurrentCapacity is a percentage and
// MaxCapacity is 100, as on Apple's own gauges; raw mAh go in AppleRaw*Capacity.
// The service registers only after the chips identify and a first reading is published.
//
// Conversions (hekate bdk/power/max17050.c, effective Rsense 10 mOhm: 5 mOhm, CGAIN 2):
// capacity LSB 0.5 mAh, current LSB 156.25 uA (positive = charging), VCELL LSB 0.625 mV
// (bits 15:3), TEMP s16 1/256 C, RepSOC 1/256 %, Cycles 1 % of a cycle (datasheet).
// The charger and PD controller are only read; charge current, input limit and the BQ24193
// I2C watchdog stay as firmware left them. The one exception is switch_battery_set_otg_boost() (USB host mode,
// drivers/SwitchUSB): REG01's CHG_CONFIG and BOOST_LIM, saved and put back as they were. The gauge's alert is armed as Linux max17042_battery.c
// does (max17042_set_soc_threshold, max17042_thread_handler): CONFIG.Aen, an SOC window of +-1 %
// around the current RepSOC, re-centred on every reading, STATUS alert bits cleared (POR, BST kept).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include "../TegraI2C/TegraI2C.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../SwitchUSB/SwitchPD.h"
#include "SwitchBattery.h"

namespace {
constexpr unsigned kBus = 1;
constexpr uint32_t kIdentifyRetryMS = 30000;  // until the chips answer on I2C1
constexpr uint32_t kRefreshMS = 120000;       // re-read for the values without an interrupt
constexpr uint32_t kSettleMS = 250;           // after an interrupt: coalesce the chips' alerts, let them settle
constexpr unsigned kAlertBurst = 20;          // interrupts per second from one line that mean it is stuck
constexpr uint8_t kGauge = 0x36, kCharger = 0x6b, kPD = 0x18;

// MAX17050 registers.
enum : uint8_t {
    Status = 0x00,
    VAlrtTh = 0x01,
    TAlrtTh = 0x02,
    SAlrtTh = 0x03,
    Config = 0x1d,
    RepCap = 0x05,
    RepSOC = 0x06,
    Temp = 0x08,
    VCell = 0x09,
    Current = 0x0a,
    AvgCurrent = 0x0b,
    FullCap = 0x10,
    Cycles = 0x17,
    DesignCap = 0x18,
    DevName = 0x21,
};
// BQ24193 registers and REG08 fields.
enum : uint8_t { InputSource = 0x00, PowerOnConfig = 0x01, SystemStatus = 0x08, VendorPart = 0x0a };
constexpr uint8_t kPowerGood = 1u << 2;
constexpr uint16_t kStatusKeep = (1u << 1) | (1u << 3);  // POR, BST; the rest are alert flags
constexpr uint16_t kConfigAen = 1u << 2;
// REG01 (bq2419x-charger.c bq2419x_vbus_enable): CHG_CONFIG 5:4 (01 charge, 10 OTG boost), BOOST_LIM 0 (1 = 1.3 A).
constexpr uint8_t kChgConfigMask = 3u << 4, kChgConfigOtg = 2u << 4, kBoostLim1300 = 1u << 0;
// BM92T36 registers (hekate bm92t36.c); state registers only, ALERT_STATUS is SwitchPD's.
enum : uint8_t { PDStatus1 = 0x03, PDCurrentPDO = 0x28 };

// Interrupt lines, all on TegraGPIO (OLED DTB, decompiled: i2c@7000c000 children):
//   BQ24193 INT   PZ0  interrupts = <0xc8 0x00>; a 256 us active-low pulse per event (datasheet): falling edge.
//   MAX17050 ALRT PY0  interrupts = <0xc0 0x00>; open drain, low while a STATUS alert flag is set: falling edge,
//                      and every reading clears the flags, so the next alert is a new edge.
//   BM92T36 ALERT PK4  SwitchPD's (level low, released by its ALERT_STATUS read); its alerts reach pdChanged().
struct Alert {
    unsigned gpio, trigger;
    const char *name;
};
constexpr Alert kAlerts[] = {
    {TEGRA_GPIO_PZ0, TEGRA_GPIO_EDGE_FALLING, "BQ24193 INT (PZ0)"},
    {TEGRA_GPIO_PY0, TEGRA_GPIO_EDGE_FALLING, "MAX17050 ALRT (PY0)"},
};
constexpr unsigned kAlertCount = sizeof(kAlerts) / sizeof(kAlerts[0]);

struct Reading {
    unsigned percent, rawPercent, millivolts, cycles;
    int rawCapacity, fullCapacity, designCapacity, milliamps, avgMilliamps, centiCelsius;
    bool external, charging, full;
    OSDictionary *adapter;
};

int gaugeWord(uint8_t reg, uint16_t *value)
{
    uint8_t b[2] = {};
    int err = tegra_i2c_read(kBus, kGauge, reg, b, sizeof(b));
    *value = (uint16_t)(b[0] | b[1] << 8);
    return err;
}

int gaugeWrite(uint8_t reg, uint16_t value)
{
    const uint8_t b[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return tegra_i2c_write(kBus, kGauge, reg, b, sizeof(b));
}

// Alert on the next 1 % step of RepSOC either way (voltage and temperature windows wide open, as the DT's
// maxim,volt-alrt-threshold 0xff00 / temp-alrt-threshold 0x7f80), then clear the alert flags so ALRT releases.
int armGauge(unsigned percent)
{
    uint16_t config = 0, status = 0;
    int err = gaugeWrite(SAlrtTh, (uint16_t)((percent + 1) << 8 | (percent > 1 ? percent - 1 : 0)));
    if (!err) err = gaugeWrite(VAlrtTh, 0xff00);
    if (!err) err = gaugeWrite(TAlrtTh, 0x7f80);
    if (!err) err = gaugeWord(Config, &config);
    if (!err && !(config & kConfigAen)) err = gaugeWrite(Config, config | kConfigAen);
    if (!err) err = gaugeWord(Status, &status);
    if (!err && (status & ~kStatusKeep)) err = gaugeWrite(Status, status & kStatusKeep);
    return err;
}

void setNumber(OSDictionary *dict, const char *key, int64_t value)
{
    if (OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32)) {
        dict->setObject(key, n);
        n->release();
    }
}

// Adapter details for powerd/IOPSCopyExternalPowerAdapterDetails. A negotiated USB-PD
// fixed PDO (BM92T36 CURRENT_PDO: 10 mA / 50 mV units, hekate bm92t36.c) gives the real
// contract; otherwise report the BQ24193 input current limit at 5 V.
OSDictionary *adapterDetails(bool external, uint8_t inputSource, uint8_t status)
{
    OSDictionary *dict = OSDictionary::withCapacity(5);
    if (!dict) return nullptr;
    if (!external) {
        setNumber(dict, kIOPMPSAdapterDetailsFamilyKey, kIOPSFamilyCodeDisconnected);
        return dict;
    }
    static const int kInputLimit[8] = {100, 150, 500, 900, 1200, 1500, 2000, 3000};
    int millivolts = 5000, milliamps = kInputLimit[inputSource & 7];
    int family = (status >> 6) == 1 ? kIOPSFamilyCodeUSBHost : kIOPSFamilyCodeUSBChargingPort;
    const char *description = "usb charger";
    uint8_t pd[5] = {};
    uint8_t pdStatus[2] = {};
    if (!tegra_i2c_read(kBus, kPD, PDStatus1, pdStatus, sizeof(pdStatus)) && (pdStatus[0] & 0x80) &&
        !tegra_i2c_read(kBus, kPD, PDCurrentPDO, pd, sizeof(pd))) {
        const uint32_t pdo = pd[1] | (uint32_t)pd[2] << 8 | (uint32_t)pd[3] << 16 | (uint32_t)pd[4] << 24;
        if ((pdo >> 30) == 0 && ((pdo >> 10) & 0x3ff)) {
            millivolts = (int)((pdo >> 10) & 0x3ff) * 50;
            milliamps = (int)(pdo & 0x3ff) * 10;
            family = kIOPSFamilyCodeUSBCPD;
            description = "pd charger";
        }
    }
    setNumber(dict, kIOPMPSAdapterDetailsFamilyKey, family);
    setNumber(dict, kIOPMPSAdapterDetailsWattsKey, (int64_t)millivolts * milliamps / 1000000);
    setNumber(dict, kIOPMPSAdapterDetailsVoltage, millivolts);
    setNumber(dict, kIOPMPSAdapterDetailsAmperageKey, milliamps);
    if (const OSString *text = OSString::withCString(description)) {
        dict->setObject(kIOPMPSAdapterDetailsDescriptionKey, text);
        text->release();
    }
    return dict;
}

int readAll(Reading *r)
{
    uint16_t repCap = 0, repSoc = 0, temp = 0, vcell = 0, current = 0, avgCurrent = 0;
    uint16_t fullCap = 0, cycles = 0, designCap = 0;
    uint8_t input = 0, status = 0;
    int err = gaugeWord(RepCap, &repCap);
    if (!err) err = gaugeWord(RepSOC, &repSoc);
    if (!err) err = gaugeWord(Temp, &temp);
    if (!err) err = gaugeWord(VCell, &vcell);
    if (!err) err = gaugeWord(Current, &current);
    if (!err) err = gaugeWord(AvgCurrent, &avgCurrent);
    if (!err) err = gaugeWord(FullCap, &fullCap);
    if (!err) err = gaugeWord(Cycles, &cycles);
    if (!err) err = gaugeWord(DesignCap, &designCap);
    if (!err) err = tegra_i2c_read(kBus, kCharger, InputSource, &input, 1);
    if (!err) err = tegra_i2c_read(kBus, kCharger, SystemStatus, &status, 1);
    if (err) return err;

    const unsigned vbus = status >> 6, chrg = (status >> 4) & 3;
    r->external = (status & kPowerGood) && vbus != 3;  // VBUS 3 = OTG: we power the dock
    r->charging = r->external && (chrg == 1 || chrg == 2);
    r->full = r->external && chrg == 3;
    r->rawPercent = repSoc >> 8;
    r->percent = r->full ? 100 : (repSoc + 128u) >> 8;
    if (r->percent > 100) r->percent = 100;
    r->rawCapacity = repCap / 2;
    r->fullCapacity = fullCap / 2;
    r->designCapacity = designCap / 2;
    r->cycles = cycles / 100u;
    r->millivolts = (vcell >> 3) * 625u / 1000;
    r->milliamps = (int16_t)current * 15625 / 100000;
    r->avgMilliamps = (int16_t)avgCurrent * 15625 / 100000;
    r->centiCelsius = (int16_t)temp * 100 / 256;
    r->adapter = adapterDetails(r->external, input, status);
    return 0;
}
}

class SwitchBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(SwitchBattery) IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOInterruptEventSource *alerts[kAlertCount] = {};
    bool published = false;
    bool reportedFailure = false;
    unsigned alertCount[kAlertCount] = {};
    uint64_t alertWindowStart[kAlertCount] = {};

    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    static void alertFired(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void pdChanged(void *context, const SwitchPDState *state);
    void attachAlerts();
    bool identify();
    bool refresh();
    void setKey(const char *key, OSObject *value);
    void setKeyNumber(const char *key, int64_t value);

public:
    bool start(IOService *provider) override;
    void free() override;
};

OSDefineMetaClassAndStructors(SwitchBattery, IOPMPowerSource)

    void SwitchBattery::setKey(const char *key, OSObject *value)
{
    if (const OSSymbol *symbol = OSSymbol::withCString(key)) {
        setPSProperty(symbol, value);
        symbol->release();
    }
}

void SwitchBattery::setKeyNumber(const char *key, int64_t value)
{
    if (OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32)) {
        setKey(key, n);
        n->release();
    }
}

bool SwitchBattery::start(IOService *provider)
{
    if (!IOPMPowerSource::start(provider)) return false;
    loop = IOWorkLoop::workLoop();
    timer = loop ? IOTimerEventSource::timerEventSource(this, &SwitchBattery::timerFired) : nullptr;
    if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) {
        IOLog("SwitchBattery: work loop setup failed\n");
        return false;
    }
    setName("SwitchBattery");
    attachAlerts();
    timer->setTimeoutMS(1);
    // The PD controller's alerts (a plug, a contract): settle and re-read, as for the other two lines.
    if (switch_pd_add_listener(&SwitchBattery::pdChanged, this)) IOLog("SwitchBattery: no PD controller events\n");
    return true;
}

void SwitchBattery::free()
{
    for (auto *&alert : alerts) {
        if (!alert) continue;
        alert->disable();
        if (loop) loop->removeEventSource(alert);
        OSSafeReleaseNULL(alert);
    }
    if (timer) {
        timer->cancelTimeout();
        if (loop) loop->removeEventSource(timer);
        timer->release();
    }
    OSSafeReleaseNULL(loop);
    IOPMPowerSource::free();
}

bool SwitchBattery::identify()
{
    uint8_t name[2] = {}, vendor = 0;
    int err = tegra_i2c_init(kBus);
    if (!err) err = tegra_i2c_read(kBus, kGauge, DevName, name, sizeof(name));
    if (!err) err = tegra_i2c_read(kBus, kCharger, VendorPart, &vendor, 1);
    if (!err && (name[0] | name[1] << 8) == 0x00ac && vendor == 0x2f) return true;
    if (!reportedFailure) {
        IOLog("SwitchBattery: MAX17050/BQ24193 not found on I2C1 (err %d, dev 0x%02x%02x, vendor 0x%02x)\n", err,
              name[1], name[0], vendor);
        reportedFailure = true;
    }
    return false;
}

bool SwitchBattery::refresh()
{
    Reading r = {};
    int err = readAll(&r);
    if (err) {
        if (!reportedFailure) IOLog("SwitchBattery: read failed (%d), keeping last values\n", err);
        reportedFailure = true;
        return false;
    }
    reportedFailure = false;
    setBatteryInstalled(true);
    setExternalConnected(r.external);
    setExternalChargeCapable(r.external);
    setIsCharging(r.charging);
    setAtCriticalLevel(false);
    setCurrentCapacity(r.percent);
    setMaxCapacity(100);
    setVoltage(r.millivolts);
    setAmperage(r.avgMilliamps);
    setCycleCount(r.cycles);
    setKey(kIOPMFullyChargedKey, r.full ? kOSBooleanTrue : kOSBooleanFalse);
    setKey("AppleRawExternalConnected", r.external ? kOSBooleanTrue : kOSBooleanFalse);
    setKeyNumber("InstantAmperage", r.milliamps);
    setKeyNumber(kIOPMPSBatteryTemperatureKey, r.centiCelsius);
    setKeyNumber(kIOPMPSDesignCapacityKey, r.designCapacity);
    setKeyNumber("NominalChargeCapacity", r.fullCapacity);
    setKeyNumber("AppleRawCurrentCapacity", r.rawCapacity);
    setKeyNumber("AppleRawMaxCapacity", r.fullCapacity);
    if (r.adapter) {
        setKey(kIOPMPSAdapterDetailsKey, r.adapter);
        r.adapter->release();
    }
    updateStatus();
    // Acknowledge: re-centre the gauge window (releases ALRT).
    if (armGauge(r.rawPercent) && !reportedFailure) IOLog("SwitchBattery: gauge alert setup failed\n");
    return true;
}

// The charger's and gauge's alert lines through TegraGPIO (which may sleep until the GIC is up).
void SwitchBattery::attachAlerts()
{
    for (unsigned i = 0; i < kAlertCount; ++i) {
        IOService *nub = tegra_gpio_interrupt_nub(kAlerts[i].gpio, kAlerts[i].trigger);
        alerts[i] =
            nub ? IOInterruptEventSource::interruptEventSource(this, &SwitchBattery::alertFired, nub, 0) : nullptr;
        if (!alerts[i] || loop->addEventSource(alerts[i]) != kIOReturnSuccess) {
            panic("SwitchBattery: cannot attach %s (GPIO %u)", kAlerts[i].name, kAlerts[i].gpio);
        }
    }
    for (auto *alert : alerts) alert->enable();
}

// Work loop: one of the chips reported a change. Read everything a moment later (the charger, PD controller and
// gauge tend to fire together on a plug event). A line that keeps firing is stuck: panic and name it.
void SwitchBattery::alertFired(OSObject *owner, IOInterruptEventSource *sender, int)
{
    auto *self = OSDynamicCast(SwitchBattery, owner);
    if (!self) return;
    unsigned i = 0;
    while (i < kAlertCount && self->alerts[i] != sender) ++i;
    if (i == kAlertCount) return;
    const uint64_t now = mach_absolute_time();
    uint64_t elapsedNS = 0;
    absolutetime_to_nanoseconds(now - self->alertWindowStart[i], &elapsedNS);
    if (elapsedNS > 1000000000ull) {
        self->alertWindowStart[i] = now;
        self->alertCount[i] = 0;
    }
    if (++self->alertCount[i] > kAlertBurst) {
        char state[256];
        tegra_gpio_describe(kAlerts[i].gpio, state, sizeof(state));
        panic("SwitchBattery: %s fired %u times in %llu ms, %s", kAlerts[i].name, self->alertCount[i],
              elapsedNS / 1000000, state);
    }
    self->timer->setTimeoutMS(kSettleMS);
}

// SwitchPD's work loop, after each BM92T36 ALERT (it has read ALERT_STATUS). The first call, at registration, is
// the state as it is (alert 0): nothing happened, so nothing to re-read.
void SwitchBattery::pdChanged(void *context, const SwitchPDState *state)
{
    auto *self = static_cast<SwitchBattery *>(context);
    if (state->alert) self->timer->setTimeoutMS(kSettleMS);
}

void SwitchBattery::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = OSDynamicCast(SwitchBattery, owner);
    if (!self) return;
    if (self->published) {
        // An alert pending at the GPIO controller that never reached us panics here (TegraGPIO.h).
        for (const Alert &alert : kAlerts) tegra_gpio_check_delivery(alert.gpio, "SwitchBattery");
        switch_pd_check_delivery("SwitchBattery");
        self->refresh();
    } else if (self->identify() && self->refresh()) {  // the first reading also arms the gauge window
        self->published = true;
        self->registerService();
        IOLog("SwitchBattery: started\n");
    }
    sender->setTimeoutMS(self->published ? kRefreshMS : kIdentifyRetryMS);
}

extern "C" int switch_battery_start(void)
{
    static SwitchBattery *battery;
    if (battery) return 0;
    IOService *resources = IOService::getResourceService();
    SwitchBattery *candidate = OSTypeAlloc(SwitchBattery);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        IOLog("SwitchBattery: attach failed\n");
        OSSafeReleaseNULL(candidate);
        return -1;
    }
    if (!candidate->start(resources)) {
        candidate->detach(resources);
        candidate->release();
        return -1;
    }
    battery = candidate;
    return 0;
}

// USB host mode's VBUS (drivers/SwitchUSB): the BQ24193 boost on (CHG_CONFIG 10, BOOST_LIM 1.3 A, as Linux
// bq2419x_vbus_enable) or back to what REG01 held before. The one writer of REG01. Thread context.
extern "C" int switch_battery_set_otg_boost(bool on)
{
    static IOLock *lock;
    static bool boosting;
    static uint8_t saved;
    if (!lock) {
        IOLock *fresh = IOLockAlloc(), *none = nullptr;
        if (!fresh) return -1;
        if (!__atomic_compare_exchange_n(&lock, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
    }
    IOLockLock(lock);
    int err = 0;
    if (on != boosting) {
        uint8_t reg = 0;
        err = tegra_i2c_init(kBus);
        if (!err) err = tegra_i2c_read(kBus, kCharger, PowerOnConfig, &reg, 1);
        if (!err) {
            uint8_t next;
            if (on) {
                saved = reg & (kChgConfigMask | kBoostLim1300);
                next = (uint8_t)((reg & ~(kChgConfigMask | kBoostLim1300)) | kChgConfigOtg | kBoostLim1300);
            } else {
                next = (uint8_t)((reg & ~(kChgConfigMask | kBoostLim1300)) | saved);
            }
            err = tegra_i2c_write(kBus, kCharger, PowerOnConfig, &next, 1);
        }
        if (!err) boosting = on;
    }
    IOLockUnlock(lock);
    if (err) IOLog("SwitchBattery: OTG boost %s failed (I2C %d)\n", on ? "on" : "off", err);
    return err ? -1 : 0;
}
