// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPD: the BM92T36 USB-PD controller's one owner; see SwitchPD.h.
//
// Registers, bits and the source bring-up: CTCaer switch-l4t-kernel-4.9 drivers/misc/bm92txx.c (bm92t_probe,
// bm92t_extcon_cable_set_init_state, bm92t_enable_ocp, bm92t_set_source_mode, bm92t_event_handler's OTG branch).
// Board (OLED DTB bm92t@18, tegra210-nx-power-fixed.dtsi): ALERT PK4 level low; VCONN enable PK5 active high;
// v_vdd5v3 (the controller's vbus-source-supply, 5.3 V, 10 ms ramp) enable PX3 active high.
#include "SwitchPD.h"
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"

namespace {
constexpr unsigned kBus = 1;
constexpr uint8_t kPD = 0x18;
constexpr unsigned kVconnGpio = TEGRA_GPIO(K, 5), kSourceRailGpio = TEGRA_GPIO(X, 3);
constexpr unsigned kMaxListeners = 4;
constexpr unsigned kAlertBurst = 50;  // ALERTs in one second that mean the line is stuck low

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
    Listener listeners[kMaxListeners] = {};
    unsigned listenerCount = 0;
    SwitchPDState state = {};
    bool sourceReady = false, reportedReadFailure = false;
    unsigned alertCount = 0;
    uint64_t alertWindow = 0;

    void handle(bool synthetic);
    static void alertFired(OSObject *owner, IOInterruptEventSource *, int);

public:
    IOWorkLoop *getWorkLoop() const override { return loop; }
    bool begin(uint16_t &type, uint16_t &revision);
    IOReturn addListener(Listener listener);
    IOReturn enableSource();
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

bool SwitchPD::begin(uint16_t &type, uint16_t &revision)
{
    int err = tegra_i2c_init(kBus);
    if (!err) err = read16(FW_TYPE, type);
    if (!err) err = read16(FW_REVISION, revision);
    if (err || revision <= 0x644) {  // bm92t_probe: older firmware is not supported
        IOLog("SwitchPD: BM92T36 not usable (I2C %d, firmware type %#x revision %#x)\n", err, type, revision);
        return false;
    }
    loop = IOWorkLoop::workLoop();
    IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_PD_ALERT, TEGRA_GPIO_LEVEL_LOW);
    alert = nub && loop ? IOInterruptEventSource::interruptEventSource(this, &SwitchPD::alertFired, nub, 0) : nullptr;
    if (!alert || loop->addEventSource(alert) != kIOReturnSuccess) {
        panic("SwitchPD: cannot attach the BM92T36 ALERT (PK4, GPIO %u, level low)", TEGRA_GPIO_PD_ALERT);
    }
    // First state with the line released (an alert latched before we came read here), then interrupts on.
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<SwitchPD *>(owner)->handle(true);
            return kIOReturnSuccess;
        },
        this);
    alert->enable();
    return true;
}

// Work loop. The pin is masked while this runs and unmasked after (TegraGPIO, level pins), so the ALERT_STATUS read
// here is what keeps the interrupt from firing again.
void SwitchPD::handle(bool synthetic)
{
    uint16_t alertBits = 0, status1 = 0, status2 = 0;
    int err = read16(ALERT_STATUS, alertBits);
    if (!err) err = read16(STATUS1, status1);
    if (!err) err = read16(STATUS2, status2);
    if (err) {
        if (!reportedReadFailure) IOLog("SwitchPD: status read failed (I2C %d)\n", err);
        reportedReadFailure = true;
        return;
    }
    reportedReadFailure = false;
    state.alert = synthetic ? 0 : alertBits;
    state.status1 = status1;
    state.status2 = status2;
    state.plugged = status1 & STATUS1_INSERT;
    state.sourcing = status1 & STATUS1_SRC_MODE;
    state.otgDevice = state.sourcing && (status2 & STATUS2_OTG_INSERT);
    state.dataHost = state.plugged && ((status1 >> STATUS1_DR_SHIFT) & 3) == DATA_ROLE_DFP;
    state.vbusPresent = status1 & STATUS1_VSAFE;
    for (unsigned i = 0; i < listenerCount; ++i) listeners[i].function(listeners[i].context, &state);
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
        char pin[256];
        tegra_gpio_describe(TEGRA_GPIO_PD_ALERT, pin, sizeof(pin));
        panic("SwitchPD: BM92T36 ALERT (PK4) fired %u times in 1 s (stuck low?): %s", self->alertCount, pin);
    }
    self->handle(false);
}

IOReturn SwitchPD::addListener(Listener listener)
{
    if (listenerCount == kMaxListeners) return kIOReturnNoResources;
    listeners[listenerCount++] = listener;
    listener.function(listener.context, &state);  // the state as it is now
    return kIOReturnSuccess;
}

// Linux bm92t_probe + bm92t_extcon_cable_set_init_state, minus the system reset: source switches off while the
// rails come up, v_vdd5v3 and VCONN on, over-current protection on, both source switches on ("supporting both OTG
// and Charger"). VBUS itself stays off until a sink is seen (SwitchUSB.cpp, through SwitchBattery's OTG boost).
IOReturn SwitchPD::enableSource()
{
    if (sourceReady) return kIOReturnSuccess;
    uint16_t config = 0, vendor = 0;
    if (read16(CONFIG1, config) || read16(VENDOR_CONFIG, vendor)) return kIOReturnIOError;
    const bool sourceOn = ((config & CONFIG1_SPDSRC_MASK) >> CONFIG1_SPDSRC_SHIFT) == SPDSRC12_ON;
    if (!sourceOn) write16(CONFIG1, (uint16_t)((config & ~CONFIG1_SPDSRC_MASK) | SPDSRC12_OFF << CONFIG1_SPDSRC_SHIFT));
    if (tegra_gpio_output(kSourceRailGpio, 1) || tegra_gpio_output(kVconnGpio, 1)) return kIOReturnIOError;
    IOSleep(10);  // v_vdd5v3 regulator-enable-ramp-delay
    int err = 0;
    if (vendor & VENDOR_CONFIG_OCP_DISABLE) err = write16(VENDOR_CONFIG, vendor & ~VENDOR_CONFIG_OCP_DISABLE);
    if (!err && !sourceOn) {
        if (read16(CONFIG1, config)) return kIOReturnIOError;
        err = write16(CONFIG1, (uint16_t)((config & ~CONFIG1_SPDSRC_MASK) | SPDSRC12_ON << CONFIG1_SPDSRC_SHIFT));
    }
    if (err) return kIOReturnIOError;
    sourceReady = true;
    IOLog("SwitchPD: source ready (v_vdd5v3 and VCONN on%s%s)\n",
          vendor & VENDOR_CONFIG_OCP_DISABLE ? ", over-current protection on" : "",
          sourceOn ? "" : ", source switches on");
    return kIOReturnSuccess;
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
        uint16_t type = 0, revision = 0;
        if (resources && pd && pd->init() && pd->attach(resources)) {
            if (pd->begin(type, revision)) {
                pd->setName("SwitchPD");
                pd->registerService();
                gPD = pd;
                gPDStage = 2;
                IOLog("SwitchPD: started (BM92T36 firmware type %#x revision %#x)\n", type, revision);
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
    return pd->getWorkLoop() && pd->getWorkLoop()->runAction(&listenerAction, pd, reinterpret_cast<void *>(listener),
                                                             context) == kIOReturnSuccess
               ? 0
               : -1;
}

extern "C" void switch_pd_check_delivery(const char *who)
{
    if (__atomic_load_n(&gPDStage, __ATOMIC_ACQUIRE) == 2) tegra_gpio_check_delivery(TEGRA_GPIO_PD_ALERT, who);
}

extern "C" int switch_pd_enable_source(void)
{
    SwitchPD *pd = startPD();
    if (!pd) return -1;
    IOReturn ret = pd->getWorkLoop()->runAction(
        [](OSObject *owner, void *, void *, void *, void *) { return static_cast<SwitchPD *>(owner)->enableSource(); },
        pd);
    if (ret != kIOReturnSuccess) IOLog("SwitchPD: source bring-up failed (%#x)\n", ret);
    return ret == kIOReturnSuccess ? 0 : -1;
}
