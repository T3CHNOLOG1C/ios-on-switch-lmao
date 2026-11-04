// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPD: the BM92T36 USB-PD controller's one owner and the port's power role; see SwitchPD.h.
//
// Registers, bits and the source bring-up: CTCaer switch-l4t-kernel-4.9 drivers/misc/bm92txx.c (bm92t_probe,
// bm92t_extcon_cable_set_init_state, bm92t_enable_ocp, bm92t_set_source_mode, bm92t_event_handler's OTG branch).
// Board (OLED DTB bm92t@18, tegra210-nx-power-fixed.dtsi): ALERT PK4 level low; VCONN enable PK5 active high;
// v_vdd5v3 (the controller's vbus-source-supply, 5.3 V, 10 ms ramp) enable PX3 active high; VBUS itself from the
// BQ24193 boost (vbus-supply usb0_vbus).
#include "SwitchPD.h"
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include "../SwitchBattery/SwitchBattery.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"

namespace {
constexpr unsigned kBus = 1;
constexpr uint8_t kPD = 0x18;
constexpr unsigned kVconnGpio = TEGRA_GPIO(K, 5), kSourceRailGpio = TEGRA_GPIO(X, 3);
constexpr unsigned kMaxListeners = 4;
constexpr unsigned kAlertBurst = 50;  // ALERTs in one second that mean the line is stuck low
// Classify window: a dual-role partner that loses our Rp sees the port open (tPDDebounce 20 ms), toggles to source
// (tDRP 100 ms), debounces our Rd (tCCDebounce 200 ms) and raises VBUS (tVBUSON 275 ms): under 600 ms by the Type-C
// spec. Re-attach window: our own toggle and debounce of a sink-only partner, about 300 ms.
constexpr uint32_t kClassifyMS = 1000, kReattachMS = 1000;

enum : uint8_t {
    ALERT_STATUS = 0x02,  // clears on read; reading it releases ALERT
    STATUS1 = 0x03,
    STATUS2 = 0x04,
    CONFIG1 = 0x06,
    VENDOR_CONFIG = 0x1a,
    FW_TYPE = 0x4b,
    FW_REVISION = 0x4c,
};
enum : uint16_t {
    ALERT_SRC_FAULT = 1u << 1,
    ALERT_CONTRACT = 1u << 12,
    STATUS1_FAULT_MASK = 3u << 0,
    STATUS1_INSERT = 1u << 7,
    STATUS1_DR_SHIFT = 8,
    STATUS1_VSAFE = 1u << 10,
    STATUS1_SRC_MODE = 1u << 12,
    STATUS2_OTG_INSERT = 1u << 13,
    CONFIG1_SPDSRC_SHIFT = 14,
    CONFIG1_SPDSRC_MASK = 3u << 14,
    SPDSRC12_ON = 0,
    SPDSRC12_OFF = 3,
    VENDOR_CONFIG_OCP_DISABLE = 1u << 2,
    DATA_ROLE_DFP = 2,
};

enum class Phase : uint8_t {
    Empty,     // nothing on the port; source switches on only with host mode
    Sink,      // a partner we do not source: switches off until it is unplugged
    Classify,  // a partner that came up as our sink, switches off: does it source (dual-role) or go (sink-only)?
    Reattach,  // it went without sourcing: switches on, waiting for it to come back as our sink
    Source,    // our VBUS on a sink-only partner
    Held,      // sourcing ended on an error (a contract, a fault, the boost refused): not again until unplugged
};

struct Listener {
    void (*function)(void *, const SwitchPDState *);
    void *context;
};

int read16(uint8_t reg, uint16_t &value)
{
    uint8_t b[2] = {};
    int err = tegra_i2c_read(kBus, kPD, reg, b, 2);
    value = (uint16_t)(b[0] | b[1] << 8);
    return err;
}

int write16(uint8_t reg, uint16_t value)
{
    const uint8_t b[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return tegra_i2c_write(kBus, kPD, reg, b, 2);
}
}  // namespace

class SwitchPD : public IOService {
    OSDeclareDefaultStructors(SwitchPD);

    IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *alert = nullptr;
    IOTimerEventSource *window = nullptr;
    Listener listeners[kMaxListeners] = {};
    unsigned listenerCount = 0;
    SwitchPDState state = {};
    Phase phase = Phase::Empty;
    bool allowSource = false, switchesOn = false, sawDetach = false, stuck = false;
    bool reportedReadFailure = false, reportedSwitchFailure = false;
    unsigned alertCount = 0;
    uint64_t alertWindow = 0;

    bool readStatus(uint16_t &status1, uint16_t &status2);
    void handle(uint16_t alertBits, uint16_t status1, uint16_t status2);
    void decide(uint16_t alertBits);
    void enter(Phase next, const char *why);
    void setSwitches(bool on);
    void startSourcing();
    void endSourcing(Phase next, const char *why);
    void notify();
    void stopEvents();
    static void alertFired(OSObject *owner, IOInterruptEventSource *, int);
    static void windowFired(OSObject *owner, IOTimerEventSource *);

public:
    IOWorkLoop *getWorkLoop() const override { return loop; }
    bool begin(uint16_t &type, uint16_t &revision, uint16_t &config, bool &turnedOff);
    IOReturn addListener(Listener listener);
    void setAllowSource(bool allow);
};
OSDefineMetaClassAndStructors(SwitchPD, IOService);

namespace {
SwitchPD *gPD;
uint32_t gPDStage;  // 0 never asked, 1 starting, 2 running, 3 absent
IOLock *gPDLock;

IOLock *pdLock()
{
    IOLock *lock = __atomic_load_n(&gPDLock, __ATOMIC_ACQUIRE);
    if (lock) return lock;
    IOLock *fresh = IOLockAlloc(), *none = nullptr;
    if (!fresh) return nullptr;
    if (__atomic_compare_exchange_n(&gPDLock, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return fresh;
    IOLockFree(fresh);
    return none;
}
}  // namespace

// Before the interrupt: the source switches off whatever an earlier boot left (r7f left them on; the controller keeps
// CONFIG1 across reboots), the latched ALERT released, the first state decided, then interrupts on.
bool SwitchPD::begin(uint16_t &type, uint16_t &revision, uint16_t &config, bool &turnedOff)
{
    int err = tegra_i2c_init(kBus);
    if (!err) err = read16(FW_TYPE, type);
    if (!err) err = read16(FW_REVISION, revision);
    if (err || revision <= 0x644) {  // bm92t_probe: older firmware is not supported
        IOLog("SwitchPD: BM92T36 not usable (I2C %d, firmware type %#x revision %#x)\n", err, type, revision);
        return false;
    }
    if (!read16(CONFIG1, config)) {
        switchesOn = ((config & CONFIG1_SPDSRC_MASK) >> CONFIG1_SPDSRC_SHIFT) != SPDSRC12_OFF;
        const bool wereOn = switchesOn;
        setSwitches(false);
        turnedOff = wereOn && !switchesOn;
    }
    loop = IOWorkLoop::workLoop();
    window = loop ? IOTimerEventSource::timerEventSource(this, &SwitchPD::windowFired) : nullptr;
    IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_PD_ALERT, TEGRA_GPIO_LEVEL_LOW);
    alert = nub && loop ? IOInterruptEventSource::interruptEventSource(this, &SwitchPD::alertFired, nub, 0) : nullptr;
    if (!window || loop->addEventSource(window) != kIOReturnSuccess || !alert ||
        loop->addEventSource(alert) != kIOReturnSuccess) {
        panic("SwitchPD: cannot attach the BM92T36 ALERT (PK4, GPIO %u, level low)", TEGRA_GPIO_PD_ALERT);
    }
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchPD *>(owner);
            uint16_t alertBits = 0, status1 = 0, status2 = 0;
            (void)read16(ALERT_STATUS, alertBits);  // releases a latched ALERT; its event is the state read next
            if (self->readStatus(status1, status2)) self->handle(0, status1, status2);
            return kIOReturnSuccess;
        },
        this);
    alert->enable();
    return true;
}

bool SwitchPD::readStatus(uint16_t &status1, uint16_t &status2)
{
    int err = read16(STATUS1, status1);
    if (!err) err = read16(STATUS2, status2);
    if (err && !reportedReadFailure) IOLog("SwitchPD: status read failed (I2C %d)\n", err);
    reportedReadFailure = err != 0;
    return !err;
}

// Work loop: an ALERT (the pin is masked while this runs and unmasked after, TegraGPIO level pins, so the
// ALERT_STATUS read here is what keeps it from firing again), a classify window's end, or a listener's first call.
void SwitchPD::handle(uint16_t alertBits, uint16_t status1, uint16_t status2)
{
    state.alert = alertBits;
    state.status1 = status1;
    state.status2 = status2;
    state.plugged = status1 & STATUS1_INSERT;
    state.vbusPresent = status1 & STATUS1_VSAFE;
    state.dataHost = state.plugged && ((status1 >> STATUS1_DR_SHIFT) & 3) == DATA_ROLE_DFP;
    decide(alertBits);
    notify();
}

void SwitchPD::notify()
{
    state.role = phase == Phase::Empty    ? SWITCH_PD_ROLE_NONE
                 : phase == Phase::Source ? SWITCH_PD_ROLE_SOURCE
                                          : SWITCH_PD_ROLE_SINK;
    for (unsigned i = 0; i < listenerCount; ++i) listeners[i].function(listeners[i].context, &state);
}

// The power role (SwitchPD.h). Only Classify and Reattach run a window; every way out of Source is endSourcing().
void SwitchPD::decide(uint16_t alertBits)
{
    const bool plugged = state.plugged, vbus = state.vbusPresent;
    const bool partnerIsSink = plugged && (state.status1 & STATUS1_SRC_MODE);  // we came up as the source
    const bool otgSink = partnerIsSink && (state.status2 & STATUS2_OTG_INSERT);
    switch (phase) {
    case Phase::Empty:
        // Our sink without OTG_INSERT (yet, or an accessory): nothing to decide until the controller says more.
        if (!plugged || (partnerIsSink && !otgSink)) break;
        if (!partnerIsSink) {
            enter(Phase::Sink, "sink: the partner is a source (charger, host or dock)");
        } else if (vbus) {
            enter(Phase::Sink, "sink: the partner came up as our sink with VBUS already up");
        } else if (!allowSource) {
            enter(Phase::Sink, "sink: the partner came up as our sink; host mode off");
        } else {
            enter(Phase::Classify, "classifying: the partner came up as our sink; source switches off 1 s");
        }
        break;
    case Phase::Classify:
        if (!plugged) {
            sawDetach = true;  // the switches moved the port: a sink-only partner stays away, a dual-role one sources
        } else if (!partnerIsSink) {
            window->cancelTimeout();
            enter(Phase::Sink, "dual-role partner: it sources with our source switches off (sink until unplugged)");
        }
        break;
    case Phase::Reattach:
        if (!plugged || (partnerIsSink && !otgSink)) break;
        window->cancelTimeout();
        if (otgSink && !vbus) {
            startSourcing();
        } else {
            enter(Phase::Sink, "sink: the partner came back as a source, or with VBUS up");
        }
        break;
    case Phase::Source:
        if (!plugged) {
            endSourcing(Phase::Empty, "detached: VBUS, v_vdd5v3, VCONN off");
        } else if (!partnerIsSink) {
            endSourcing(Phase::Sink, "not sourcing: the partner took the source role (sink until unplugged)");
        } else if (alertBits & ALERT_CONTRACT) {
            endSourcing(Phase::Held, "not sourcing: the partner made a USB-PD contract (no plain OTG device)");
        } else if ((alertBits & ALERT_SRC_FAULT) || (state.status1 & STATUS1_FAULT_MASK)) {
            endSourcing(Phase::Held, "not sourcing: source fault");
        }
        break;
    case Phase::Sink:
    case Phase::Held:
        if (!plugged) enter(Phase::Empty, "detached");
        break;
    }
}

void SwitchPD::enter(Phase next, const char *why)
{
    phase = next;
    sawDetach = false;
    setSwitches(next == Phase::Empty || next == Phase::Held || next == Phase::Reattach ? allowSource : false);
    if (next == Phase::Classify) window->setTimeoutMS(kClassifyMS);
    if (next == Phase::Reattach) window->setTimeoutMS(kReattachMS);
    IOLog("SwitchPD: %s (alert %#x status1 %#x status2 %#x)\n", why, state.alert, state.status1, state.status2);
}

// CONFIG1 SPDSRC: both source switches on (SPDSRC12_ON, bm92t_set_source_mode) or off (SPDSRC12_OFF).
void SwitchPD::setSwitches(bool on)
{
    if (on == switchesOn) return;
    uint16_t config = 0;
    int err = read16(CONFIG1, config);
    if (!err) {
        config =
            (uint16_t)((config & ~CONFIG1_SPDSRC_MASK) | (on ? SPDSRC12_ON : SPDSRC12_OFF) << CONFIG1_SPDSRC_SHIFT);
        err = write16(CONFIG1, config);
    }
    if (!err) {
        switchesOn = on;
    } else if (!reportedSwitchFailure) {
        IOLog("SwitchPD: source switches %s failed (I2C %d)\n", on ? "on" : "off", err);
    }
    reportedSwitchFailure = err != 0;
}

// Reattach: the partner proved sink-only and is back as our sink with VBUS down. bm92t_extcon_cable_set_init_state
// and the OTG branch of bm92t_event_handler: over-current protection on, v_vdd5v3 and VCONN on, then VBUS.
void SwitchPD::startSourcing()
{
    uint16_t vendor = 0;
    int err = read16(VENDOR_CONFIG, vendor);
    if (!err && (vendor & VENDOR_CONFIG_OCP_DISABLE)) err = write16(VENDOR_CONFIG, vendor & ~VENDOR_CONFIG_OCP_DISABLE);
    if (!err) err = tegra_gpio_output(kSourceRailGpio, 1);
    if (!err) err = tegra_gpio_output(kVconnGpio, 1);
    if (!err) IOSleep(10);  // v_vdd5v3 regulator-enable-ramp-delay
    const int boost = err ? 0 : switch_battery_set_otg_boost(true);
    phase = Phase::Source;
    if (err) {
        endSourcing(Phase::Held, "not sourcing: over-current protection or the source rails failed");
    } else if (boost == SWITCH_BATTERY_BOOST_VBUS_PRESENT) {
        endSourcing(Phase::Sink, "not sourcing: the charger sees VBUS (sink until unplugged)");
    } else if (boost) {
        endSourcing(Phase::Held, "not sourcing: the BQ24193 boost failed");
    } else {
        IOLog("SwitchPD: sink-only partner: sourcing VBUS (OCP, v_vdd5v3, VCONN, boost on) (alert %#x status1 %#x "
              "status2 %#x)\n",
              state.alert, state.status1, state.status2);
    }
}

// The one way out of Source, for a detach and for every error: VBUS (the boost), v_vdd5v3 and VCONN off, then the
// switches as `next` has them (the empty-port state, or off for a sink session).
void SwitchPD::endSourcing(Phase next, const char *why)
{
    (void)switch_battery_set_otg_boost(false);
    (void)tegra_gpio_output(kVconnGpio, 0);
    (void)tegra_gpio_output(kSourceRailGpio, 0);
    enter(next, why);
}

void SwitchPD::windowFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchPD *>(owner);
    uint16_t status1 = 0, status2 = 0;
    if (!self->readStatus(status1, status2)) return;
    self->state.alert = 0;
    self->state.status1 = status1;
    self->state.status2 = status2;
    const bool plugged = status1 & STATUS1_INSERT;
    if (self->phase == Phase::Classify) {
        if (!self->sawDetach) {
            self->enter(Phase::Sink, "not sourcing: no detach with our source switches off, sink-only not proven");
        } else if (plugged) {
            self->enter(Phase::Sink, "not sourcing: the partner came back with our source switches off");
        } else {
            self->enter(Phase::Reattach, "sink-only partner: it left with our source switches off and sourced "
                                         "nothing; switches on");
        }
    } else if (self->phase == Phase::Reattach) {
        self->enter(Phase::Empty, "empty: the sink-only partner did not come back");
    } else {
        return;
    }
    self->handle(0, status1, status2);  // the state as it is now, through the new phase
}

void SwitchPD::alertFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchPD *>(owner);
    const uint64_t now = mach_absolute_time();
    uint64_t elapsedNS = 0;
    absolutetime_to_nanoseconds(now - self->alertWindow, &elapsedNS);
    if (elapsedNS > 1000000000ull) {
        self->alertWindow = now;
        self->alertCount = 0;
    }
    if (++self->alertCount > kAlertBurst) {
        self->stopEvents();
        return;
    }
    uint16_t alertBits = 0, status1 = 0, status2 = 0;
    int err = read16(ALERT_STATUS, alertBits);
    if (err) {
        if (!self->reportedReadFailure) IOLog("SwitchPD: status read failed (I2C %d)\n", err);
        self->reportedReadFailure = true;
        return;
    }
    if (self->readStatus(status1, status2)) self->handle(alertBits, status1, status2);
}

// ALERT stuck low (re-fires as fast as the work loop reads it): no storm. The pin is masked for the rest of the boot
// with one line, VBUS is never driven again (sourcing ends, switches off) and the port stays sink-only; the charger,
// which does not need the PD controller to charge, and the battery's own slow re-read carry on.
void SwitchPD::stopEvents()
{
    char pin[256];
    tegra_gpio_describe(TEGRA_GPIO_PD_ALERT, pin, sizeof(pin));
    alert->disable();
    stuck = true;
    allowSource = false;
    window->cancelTimeout();
    IOLog("SwitchPD: BM92T36 ALERT (PK4) fired %u times in 1 s, turned off (port sink-only until reboot): %s\n",
          alertCount, pin);
    if (phase == Phase::Source) {
        endSourcing(Phase::Sink, "not sourcing: no PD events");
    } else {
        enter(Phase::Sink, "sink: no PD events");
    }
    notify();
}

IOReturn SwitchPD::addListener(Listener listener)
{
    if (listenerCount == kMaxListeners) return kIOReturnNoResources;
    listeners[listenerCount++] = listener;
    listener.function(listener.context, &state);  // the state as it is now
    return kIOReturnSuccess;
}

void SwitchPD::setAllowSource(bool allow)
{
    if (allow == allowSource || stuck) return;
    allowSource = allow;
    if (!allow && phase == Phase::Source) {
        endSourcing(Phase::Held, "not sourcing: host mode is off");
        notify();
    } else if (!allow && (phase == Phase::Classify || phase == Phase::Reattach)) {
        window->cancelTimeout();
        enter(Phase::Sink, "sink: host mode is off");
        notify();
    } else if (phase == Phase::Empty || phase == Phase::Held) {
        setSwitches(allow);
    }
}

// ---------------------------------------------------------------------------
namespace {
SwitchPD *startPD()
{
    IOLock *lock = pdLock();
    if (!lock) return nullptr;
    IOLockLock(lock);
    if (gPDStage == 0) {
        gPDStage = 3;
        IOService *resources = IOService::getResourceService();
        SwitchPD *pd = OSTypeAlloc(SwitchPD);
        uint16_t type = 0, revision = 0, config = 0;
        bool turnedOff = false;
        if (resources && pd && pd->init() && pd->attach(resources)) {
            if (pd->begin(type, revision, config, turnedOff)) {
                pd->setName("SwitchPD");
                pd->registerService();
                gPD = pd;
                gPDStage = 2;
                IOLog("SwitchPD: started (BM92T36 firmware type %#x revision %#x, CONFIG1 %#x%s)\n", type, revision,
                      config, turnedOff ? ": source switches were on, now off" : "");
            }
            // A failed begin() leaves the service attached: its alert source may already be on the loop.
        } else {
            OSSafeReleaseNULL(pd);
        }
    }
    SwitchPD *pd = gPDStage == 2 ? gPD : nullptr;
    IOLockUnlock(lock);
    return pd;
}

IOReturn listenerAction(OSObject *owner, void *function, void *context, void *, void *)
{
    return static_cast<SwitchPD *>(owner)->addListener(
        Listener{reinterpret_cast<void (*)(void *, const SwitchPDState *)>(function), context});
}
}  // namespace

extern "C" int switch_pd_add_listener(void (*listener)(void *, const SwitchPDState *), void *context)
{
    SwitchPD *pd = startPD();
    if (!pd || !listener) return -1;
    return pd->getWorkLoop()->runAction(&listenerAction, pd, reinterpret_cast<void *>(listener), context) ==
                   kIOReturnSuccess
               ? 0
               : -1;
}

extern "C" void switch_pd_check_delivery(const char *who)
{
    if (__atomic_load_n(&gPDStage, __ATOMIC_ACQUIRE) == 2) tegra_gpio_check_delivery(TEGRA_GPIO_PD_ALERT, who);
}

extern "C" int switch_pd_allow_source(bool allow)
{
    SwitchPD *pd = startPD();
    if (!pd) return -1;
    pd->getWorkLoop()->runAction(
        [](OSObject *owner, void *allow, void *, void *, void *) -> IOReturn {
            static_cast<SwitchPD *>(owner)->setAllowSource(allow != nullptr);
            return kIOReturnSuccess;
        },
        pd, allow ? pd : nullptr);
    return 0;
}
