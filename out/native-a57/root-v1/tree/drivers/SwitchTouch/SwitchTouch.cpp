// SPDX-License-Identifier: BSD-2-Clause-Patent
// Nintendo Switch OLED touch screen: STMicro FingerTip S (FTM4CD60/FTM4CD50, chip id 0x3670) on I2C3 0x49, reported
// to iPadOS as a ten-finger touch screen through SwitchHIDDevice (SwitchHID.h).
//
// Hardware (switchroot tegra210b01-nx-common.dtsi fts@49; hekate bdk/input/touch.c; switchroot Linux ftm4.c):
//   * Power: hekate's hw_deinit runs touch_power_off (reset/VIO low, MAX77620 LDO6 off, I2C3 clock off), so the
//     panel is brought up from scratch as touch_power_on does: PJ7 (stm,vio-gpio) low, LDO6 2.9 V on (PMIC 0x3C on
//     I2C5), I2C3 up, PJ7 high, 10 ms.
//   * Init (touch_init, Linux fts_init): chip id at hardware register 0x0004, system reset (register 0x0028 = 0x80,
//     then the controller-ready event 0x10), finger sense mode (C3 01), sense on (93), clear the event stack (A1),
//     and the interrupt enable Linux sets and hekate (which polls) does not: register 0x002C = 0x48.
//   * Interrupt: INT on GPIO PX1, level low while the event FIFO holds events (DT IRQ_TYPE_LEVEL_LOW, IRQF_ONESHOT).
//     The action reads the event count (register 0x0023: byte 1 >> 1) and the events (86, 8 bytes each), as Linux
//     fts_interrupt_handler, until the FIFO is empty, so INT is released before the pin is unmasked.
//   * Events: byte 0 low nibble = type (3 enter, 4 leave, 5 motion), high nibble = contact 0..9; X = b1 << 4 | b3 >> 4,
//     Y = b2 << 4 | b3 & 0xf, already in the landscape frame hekate's GUI uses (stm,max_coords 1280 x 720); scaled
//     from the panel's active area (stm,max-real-coords 1264 x 704, stm,edge-offset 15) as fts_coordinates_factor.
// That landscape frame is the one the display shows iOS (NativeScanout.h and the rotate-3 console both put landscape
// pixel (x, y) at portrait row 1279 - x, column y, upright in handheld use, as hekate's GUI). Boot-arg
// switch-touch-flip: bit 0 mirrors X, bit 1 mirrors Y, in case the hardware shows otherwise. Portrait-native
// (IOSwitchPortrait.h): iOS's screen is the portrait panel, so contacts are reported at that row and column (position).
//
// Failures panic with a one-line reason; there is no polled mode. A 10 s watchdog only checks that a latched INT
// reached the CPU (tegra_gpio_check_delivery).
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <pexpert/pexpert.h>
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"
#include "SwitchHID.h"
#include "SwitchTouch.h"

// SwitchBrightness (SwitchBacklight.cpp): false while the panel is dark (display power off). Weak, so
// the touch screen links and reports as before without it.
extern "C" __attribute__((weak)) bool switch_display_is_on(void)
{
    return true;
}

// SwitchBrightness (SwitchALS.cpp): LDO6 also feeds the ambient light sensor's I2C side, which waits for it. Weak, as
// above.
extern "C" __attribute__((weak)) void switch_als_supply(bool) {}

namespace {
constexpr unsigned kBus = 3, kPmicBus = 5;
constexpr uint8_t kTouch = 0x49, kPmic = 0x3c;
// MAX77620 LDO6_CFG: output voltage 5:0 in 50 mV steps from 800 mV, power mode 7:6 (3 = normal); hekate
// max7762x_regulator_set_voltage / _enable (LDO6 = touch, 2.9 V).
constexpr uint8_t kLdo6Cfg = 0x2f, kLdoVoltMask = 0x3f, kLdoModeNormal = 0xc0, kLdo6Volt = (2900 - 800) / 50;

enum : uint8_t {  // FTM4 commands (hekate touch.h FTS4_CMD_*, Linux ftm4.h)
    CmdReadOneEvent = 0x85,
    CmdReadAllEvents = 0x86,
    CmdSenseOn = 0x93,
    CmdClearEvents = 0xa1,
    CmdHwReg = 0xb6,
    CmdSenseMode = 0xc3,
};
enum : uint8_t { EvEnter = 0x03, EvLeave = 0x04, EvMotion = 0x05, EvReady = 0x10 };
enum : uint16_t { RegChipId = 0x0004, RegEventCount = 0x0023, RegReset = 0x0028, RegInterrupt = 0x002c };
constexpr uint8_t kFingerMode = 0x01, kResetValue = 0x80, kInterruptEnable = 0x48;
constexpr uint16_t kChipId = 0x3670;
constexpr unsigned kEventSize = 8, kFifoDepth = 32, kSlots = kSwitchTouchSlots;
constexpr unsigned kRealMaxX = 1264, kRealMaxY = 704, kEdgeOffset = 15;
constexpr unsigned kEdgeBand = 24;  // px: ~3 mm on the 155 x 87 mm panel (Apple's edge contacts are ~3 mm)
constexpr uint32_t kWatchdogMS = 10000;
constexpr unsigned kEmptyLimit = 64;  // interrupts in a row that found the FIFO empty: INT is stuck low

int command(const uint8_t *bytes, size_t length)
{
    return tegra_i2c_transfer(kBus, kTouch, bytes, length, nullptr, 0);
}

int readRegister(uint16_t reg, uint8_t *buffer, size_t length)
{
    const uint8_t tx[3] = {CmdHwReg, (uint8_t)(reg >> 8), (uint8_t)reg};
    return tegra_i2c_transfer(kBus, kTouch, tx, sizeof(tx), buffer, length);
}

int writeRegister(uint16_t reg, uint8_t value)
{
    const uint8_t tx[4] = {CmdHwReg, (uint8_t)(reg >> 8), (uint8_t)reg, value};
    return command(tx, sizeof(tx));
}

int eventCount(unsigned *count)
{
    uint8_t b[2] = {};
    const int err = readRegister(RegEventCount, b, sizeof(b));
    *count = err ? 0 : b[1] >> 1;
    return err;
}

// Polls the event stack for one event type during bring-up only (hekate _touch_wait_event): INT is not enabled yet.
bool waitEvent(uint8_t type, unsigned timeoutMS)
{
    for (unsigned waited = 0; waited <= timeoutMS; ++waited) {
        unsigned count = 0;
        if (!eventCount(&count) && count) {
            const uint8_t cmd = CmdReadOneEvent;
            uint8_t event[kEventSize] = {};
            if (!tegra_i2c_transfer(kBus, kTouch, &cmd, 1, event, sizeof(event)) && event[0] == type) return true;
        }
        IOSleep(1);
    }
    return false;
}

uint16_t scale(unsigned raw, unsigned realMax, unsigned size)
{
    if (raw < kEdgeOffset) raw = kEdgeOffset;
    if (raw > realMax) raw = realMax;
    return (uint16_t)((raw - kEdgeOffset) * (size - 1) / (realMax - kEdgeOffset));
}
}  // namespace

class SwitchTouch : public IOService {
    OSDeclareDefaultStructors(SwitchTouch)

        struct Contact {
        bool down;
        uint16_t x, y;
    };

    IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *irq = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOService *hid = nullptr;
    Contact contacts[kSlots] = {};
    uint32_t edge = 0;     // contacts that began within kEdgeBand of a panel edge
    uint32_t changed = 0;  // contacts that went down or up since the last report
    bool dirty = false, running = false, announced = false;
    bool screenOff = false;  // display power off (switch_touch_display_power): stays down until the screen lights
    unsigned flip = 0, empty = 0;

    static void interruptOccurred(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    void powerUp();
    void bringUp();
    void enableSensing();
    void powerDown();
    void handle(const uint8_t *event);
    void position(const uint8_t *event, Contact &contact) const;
    void flush();
    void releaseAll();

public:
    bool start(IOService *provider) override;
    void free() override;
    void suspend();
    void resume();
    void displayPower(bool on);
};

OSDefineMetaClassAndStructors(SwitchTouch, IOService)

    bool SwitchTouch::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    PE_parse_boot_argn("switch-touch-flip", &flip, sizeof(flip));
    hid = switch_hid_create(kSwitchHIDTouch);
    IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_TOUCH_INT, TEGRA_GPIO_LEVEL_LOW);
    if (!nub) {
        char state[256];
        tegra_gpio_describe(TEGRA_GPIO_TOUCH_INT, state, sizeof(state));
        panic("SwitchTouch: no interrupt nub for the touch INT line (PX1): %s", state);
    }
    loop = IOWorkLoop::workLoop();
    irq = IOInterruptEventSource::interruptEventSource(this, &SwitchTouch::interruptOccurred, nub, 0);
    timer = IOTimerEventSource::timerEventSource(this, &SwitchTouch::timerFired);
    if (!loop || !irq || !timer || loop->addEventSource(irq) != kIOReturnSuccess ||
        loop->addEventSource(timer) != kIOReturnSuccess) {
        panic("SwitchTouch: work loop setup failed");
    }
    timer->setTimeoutMS(1);  // power the panel up on the work loop, off the caller's thread
    return true;
}

void SwitchTouch::free()
{
    if (loop) {
        if (irq) loop->removeEventSource(irq);
        if (timer) loop->removeEventSource(timer);
    }
    OSSafeReleaseNULL(irq);
    OSSafeReleaseNULL(timer);
    OSSafeReleaseNULL(loop);
    IOService::free();
}

// hekate touch_power_on.
void SwitchTouch::powerUp()
{
    if (tegra_gpio_output(TEGRA_GPIO_TOUCH_RESET, 0)) panic("SwitchTouch: cannot drive the touch reset line (PJ7)");
    IODelay(20);

    uint8_t ldo = 0;
    int err = tegra_i2c_init(kPmicBus);
    if (!err) err = tegra_i2c_read(kPmicBus, kPmic, kLdo6Cfg, &ldo, 1);
    if (!err) {
        ldo = (uint8_t)((ldo & ~kLdoVoltMask) | kLdo6Volt);
        err = tegra_i2c_write(kPmicBus, kPmic, kLdo6Cfg, &ldo, 1);
    }
    IOSleep(1);
    if (!err) {
        ldo |= kLdoModeNormal;
        err = tegra_i2c_write(kPmicBus, kPmic, kLdo6Cfg, &ldo, 1);
    }
    if (err)
        panic("SwitchTouch: MAX77620 LDO6 (touch 2.9 V, I2C5 0x3c reg 0x2f) could not be enabled: I2C error %d", err);
    IOSleep(1);

    err = tegra_i2c_init(kBus);
    if (err) panic("SwitchTouch: I2C3 init failed: %d", err);
    IOSleep(1);
    tegra_gpio_output(TEGRA_GPIO_TOUCH_RESET, 1);
    IOSleep(10);
    switch_als_supply(true);  // LDO6 on for 12 ms or more: L4T's bh1730 waits 10 ms after its supplies
    waitEvent(EvReady, 20);   // as hekate: the reset below waits for it again
}

// hekate touch_init (chip id, system reset, sensing), three attempts.
void SwitchTouch::bringUp()
{
    int err = 0;
    uint8_t id[7] = {};
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        err = readRegister(RegChipId, id, sizeof(id));
        if (err || (id[1] << 8 | id[2]) != kChipId) continue;
        err = writeRegister(RegReset, kResetValue);
        IOSleep(10);
        if (err || !waitEvent(EvReady, 20)) continue;
        enableSensing();
        if (!announced)
            IOLog("SwitchTouch: started (FTM4 firmware %02x%02x, config %02x/%02x)\n", id[3], id[4], id[5], id[6]);
        announced = true;
        return;
    }
    panic("SwitchTouch: FTM4 on I2C3 0x49 did not come up: I2C error %d, chip id %02x%02x (expected 3670), "
          "no controller-ready event after reset otherwise",
          err, id[1], id[2]);
}

// Finger sense mode, sensing on, empty event stack, interrupt on (Linux fts_init; hekate touch_sense_enable).
void SwitchTouch::enableSensing()
{
    const uint8_t mode[2] = {CmdSenseMode, kFingerMode}, on = CmdSenseOn, clear = CmdClearEvents;
    int err = command(mode, sizeof(mode));
    IOSleep(10);
    if (!err) err = command(&on, 1);
    if (!err) err = command(&clear, 1);
    if (!err) err = writeRegister(RegInterrupt, kInterruptEnable);
    if (err) panic("SwitchTouch: FTM4 sense/interrupt enable failed: I2C error %d", err);
}

void SwitchTouch::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = OSDynamicCast(SwitchTouch, owner);
    if (!self) return;
    if (!self->running) {
        self->powerUp();
        self->bringUp();
        self->running = true;
        self->irq->enable();
    } else {
        tegra_gpio_check_delivery(TEGRA_GPIO_TOUCH_INT, "SwitchTouch");
    }
    sender->setTimeoutMS(kWatchdogMS);
}

// Work loop, PX1 masked until return: drain the FIFO so the controller releases INT. While the panel is dark the events
// are read and dropped (a finger on a dark screen must not reach SpringBoard), and contacts that were down when it went
// dark are lifted once.
void SwitchTouch::interruptOccurred(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = OSDynamicCast(SwitchTouch, owner);
    if (!self) return;
    const bool shown = switch_display_is_on();
    bool any = false;
    for (unsigned round = 0; round < 4; ++round) {
        unsigned count = 0;
        int err = eventCount(&count);
        uint8_t events[kFifoDepth * kEventSize];
        if (!err && count) {
            if (count > kFifoDepth) count = kFifoDepth;
            const uint8_t cmd = CmdReadAllEvents;
            err = tegra_i2c_transfer(kBus, kTouch, &cmd, 1, events, count * kEventSize);
        }
        if (err) panic("SwitchTouch: reading the FTM4 event FIFO failed: I2C error %d", err);
        if (!count) break;
        any = true;
        for (unsigned i = 0; i < count; ++i) {
            if (shown || events[i * kEventSize] == EvReady) self->handle(events + i * kEventSize);
        }
    }
    if (shown) {
        self->flush();
    } else {
        self->releaseAll();
    }
    if (any) {
        self->empty = 0;
    } else if (++self->empty > kEmptyLimit) {
        char state[256];
        tegra_gpio_describe(TEGRA_GPIO_TOUCH_INT, state, sizeof(state));
        panic("SwitchTouch: INT (PX1) fired %u times in a row with an empty event FIFO: %s", self->empty, state);
    }
}

void SwitchTouch::position(const uint8_t *event, Contact &contact) const
{
    uint16_t x = scale((unsigned)event[1] << 4 | event[3] >> 4, kRealMaxX, kSwitchTouchWidth);
    uint16_t y = scale((unsigned)event[2] << 4 | (event[3] & 0xf), kRealMaxY, kSwitchTouchHeight);
    if (flip & 1) x = (uint16_t)(kSwitchTouchWidth - 1 - x);
    if (flip & 2) y = (uint16_t)(kSwitchTouchHeight - 1 - y);
    // Portrait-native (SwitchHID.h): the panel's frame, as the display maps landscape pixel (x, y) to row 1279 - x, column y.
    contact.x = IOSWITCH_PORTRAIT_NATIVE ? y : x;
    contact.y = IOSWITCH_PORTRAIT_NATIVE ? (uint16_t)(kSwitchTouchWidth - 1 - x) : y;
}

// One FIFO event. A contact that goes down and up within one batch gets each transition in its own report.
void SwitchTouch::handle(const uint8_t *event)
{
    if (event[0] == EvReady) {  // the controller reset itself (ESD): its sensing and interrupt are off again
        releaseAll();
        enableSensing();
        return;
    }
    const uint8_t type = event[0] & 0x0f, id = event[0] >> 4;
    if (id >= kSlots || (type != EvEnter && type != EvMotion && type != EvLeave)) return;
    Contact &contact = contacts[id];
    const uint32_t bit = 1u << id;
    if (type == EvLeave) {
        if (!contact.down) return;
        if (changed & bit) flush();
        contact.down = false;
        changed |= bit;
    } else if (!contact.down) {
        if (type == EvMotion) return;  // as Linux: motion of a contact that has left is stale
        if (changed & bit) flush();
        position(event, contact);
        contact.down = true;
        changed |= bit;
        const bool atEdge = contact.x < kEdgeBand || contact.x >= kSwitchTouchReportWidth - kEdgeBand ||
                            contact.y < kEdgeBand || contact.y >= kSwitchTouchReportHeight - kEdgeBand;
        edge = atEdge ? edge | bit : edge & ~bit;
    } else {
        position(event, contact);
    }
    dirty = true;
}

void SwitchTouch::flush()
{
    if (!dirty) return;
    SwitchTouchReport report = {};
    report.id = 1;
    for (unsigned i = 0; i < kSlots; ++i) {
        report.slot[i].flags = (uint8_t)(kSwitchTouchValid | (contacts[i].down ? kSwitchTouchTip : 0));
        report.slot[i].contact = (uint8_t)i;
        report.slot[i].x = contacts[i].x;
        report.slot[i].y = contacts[i].y;
    }
    switch_hid_set_edge_contacts(hid, edge);
    switch_hid_post(hid, &report, sizeof(report));  // dropped by IOHIDFamily until the event driver has opened it
    changed = 0;
    dirty = false;
}

void SwitchTouch::releaseAll()
{
    for (Contact &contact : contacts) {
        if (contact.down) {
            contact.down = false;
            dirty = true;
        }
    }
    flush();
}

// Work loop: the panel powered off as hekate's touch_power_off (reset low, LDO6 off) with every contact lifted, the
// watchdog stopped. It comes back up from scratch by the same timer path as at start (touch_power_on, touch_init).
void SwitchTouch::powerDown()
{
    timer->cancelTimeout();
    if (!running) return;
    irq->disable();
    releaseAll();
    running = false;
    switch_als_supply(false);  // first: the sensor powers down while it still has LDO6
    tegra_gpio_output(TEGRA_GPIO_TOUCH_RESET, 0);
    uint8_t ldo = 0;
    if (!tegra_i2c_read(kPmicBus, kPmic, kLdo6Cfg, &ldo, 1)) {
        ldo = (uint8_t)(ldo & ~kLdoModeNormal);
        (void)tegra_i2c_write(kPmicBus, kPmic, kLdo6Cfg, &ldo, 1);
    }
}

// System sleep (SwitchSuspend's sleep and wake actions, thread context, I2C up). After SC7 the panel stays down while
// the screen is dark (a wake that did not light it, e.g. the RTC alarm); the next display power-on brings it up.
void SwitchTouch::suspend()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<SwitchTouch *>(owner)->powerDown();
            return kIOReturnSuccess;
        },
        this);
}

void SwitchTouch::resume()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchTouch *>(owner);
            if (!self->screenOff) self->timer->setTimeoutMS(1);
            return kIOReturnSuccess;
        },
        this);
}

// Display power (SwitchBacklight's work loop, thread context): down with the screen, as Linux ftm4 fts_suspend powers it
// off; back up when the screen lights, on this work loop (about 60 ms, while the panel takes about 0.5 s to wake).
void SwitchTouch::displayPower(bool on)
{
    loop->runAction(
        [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchTouch *>(owner);
            self->screenOff = arg == nullptr;
            if (self->screenOff)
                self->powerDown();
            else if (!self->running)
                self->timer->setTimeoutMS(1);
            return kIOReturnSuccess;
        },
        this, on ? this : nullptr);
}

namespace {
SwitchTouch *touch;
}

extern "C" void switch_touch_suspend(void)
{
    if (touch) touch->suspend();
}

extern "C" void switch_touch_resume(void)
{
    if (touch) touch->resume();
}

extern "C" void switch_touch_display_power(bool on)
{
    if (touch) touch->displayPower(on);
}

extern "C" int switch_touch_start(void)
{
    if (touch) return 0;
    IOService *resources = IOService::getResourceService();
    SwitchTouch *candidate = OSTypeAlloc(SwitchTouch);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources) ||
        !candidate->start(resources)) {
        panic("SwitchTouch: attach or start failed");
    }
    touch = candidate;  // the attachment keeps it
    candidate->release();
    return 0;
}
