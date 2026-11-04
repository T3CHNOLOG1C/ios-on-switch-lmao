// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Switch OLED's ambient light sensor as iPadOS CoreBrightness's ALS. On by default; boot-arg switch-als=0 leaves it
// off.
//
// Hardware: ROHM BH1730FVC on I2C2 (0x7000C400) at 0x29, behind the OLED panel.
//   * switchroot L4T: nx-platforms/tegra210-nx-sensor.dtsi i2c@7000c400 bh1730fvc@29 (rohm,integration-cycle 38,
//     rohm,lux-multiplier 3600), overridden for the OLED (Aula, "fric") in tegra210b01-fric.dts (rohm,opt-win-coeff and
//     rohm,gain-coeff below). Driver: CTCaer/switch-l4t-kernel-4.9 drivers/iio/light/bh1730.c (GPL-2.0, Copyright (c)
//     2021 CTCaer); the lux formula and gain steps below follow it. hekate bdk/input/als.c drives the same part.
//   * Power: MAX77620 LDO6, "Touch Panel, ALS", which SwitchTouch turns on (on its own work loop, a moment after
//     switch_touch_start returns) and off for SC7. So this driver first talks to the sensor at its first sample, after
//     start and after every wake, and gives it kErrorLimit samples to answer.
//   * No interrupt line. The OLED DT node has no interrupts. The reference design's ALS_PROX_INT pad (PX3) is the
//     VDD_5V3 enable output on Mariko boards (L4T tegra210-nx-power-fixed.dtsi v_vdd5v3 gpio X3, "Only enabled on
//     Modin/Vali/Fric"; Atmosphere gpio_internal_pad_map_combination.inc DeviceCode_Vdd5V3En = Port_X_3), with its
//     input buffer off (tegra210b01-fric-pinmux.dtsi; Atmosphere pinmux_initial_pad_config_aula.inc AlsProxInt 0x4),
//     and Horizon has no GPIO for the sensor at all. L4T, Android, hekate and Horizon all read it on demand.
//   * The clock: the sensor converts continuously on its own ADC clock (102.6 ms, 38 cycles, + 2 ms), and every
//     kFramesPerSample-th frame end of display controller A takes the result. FRAME_END stays unmasked while the panel
//     scans out (NativeLegacyDisplay feeds TegraPanel's DSI commands with it), so these interrupts arrive anyway;
//     tegra_panel_frame_end_filter() hands each to switch_als_frame_end() (primary interrupt context: a count and, every
//     seventh, a signal to this work loop, where the I2C read runs). When the scanout stops (display power off, SC7)
//     the sampling stops with it, which is when CoreBrightness ignores ALS events anyway ("ALS event arrived when
//     display OFF"). A clock that stops while the panel is lit panics after kClockDeadlineMS, a sensor that stops
//     answering after kErrorLimit samples.
//   * Lux: Linux bh1730_get_lux with the OLED coefficients, one division instead of three (the truncation of the first
//     cost up to 15% below 30 lux). lux = (cv * DATA0 - ci * DATA1) * 100 ms * multiplier / (gain * integration time
//     * 1000), with (cv, ci) from the first optical-window row whose rc exceeds 1000 * DATA1 / DATA0. Gain steps as
//     bh1730_adjust_gain: down when the larger channel passes the gain's fl, up when it falls under its cl, except that
//     a step up is taken only if the reading stays under the next gain's fl (the DT's 2x cl 1600 is 51200 at 64x, over
//     its fl 50000, which would flip between the two every sample). hekate's unused Aula table differs (row 0 ci 30300,
//     multiplier 1410); the DT is what L4T and Android use on this console. No source compensates for the panel's own
//     light reaching the sensor; the stats line below gives the panel level next to the readings to measure it.
//
// iPadOS side (21H461; CoreBrightness, libMobileGestalt and AccessibilitySettings disassembly):
//   * MobileGestalt ambient-light-sensor (lo3szoQ4sLy7o3+ZD0GcAQ) is IOServiceGetMatchingService(IOPropertyMatch
//     {PrimaryUsagePage 0xff00, PrimaryUsage 4}), which the light HID device below answers. Settings (AXDisplayController
//     specifiers, AUTO_BRIGHTNESS) and CoreBrightness (DisplayCreateWithID, DisplaySetProperty: "ALS is not supported on
//     this device -> ignore setting of DisplayBrightnessAuto") both gate on it. The answer is precacheable, so XNU drops
//     MobileGestalt's cache when switch_als_published() changes from the last boot (kern_exec.c).
//   * -[BLControl startHIDSystemClient] matches DeviceUsagePairs {0xff00, 0x0004}; AABC::registerService takes the
//     service only when Built-In is true (SwitchHIDDevice sets it; IOHIDEventService copies it from the interface).
//     Orientation, Placement, ALSSensorType and the integration times are optional (front ALS, defaults).
//   * AABC::HandleALSEvent reads IOHIDEventGetFloatValue(event, 0xc0000) (kIOHIDEventFieldAmbientLightSensorLevel, lux)
//     from kIOHIDEventTypeAmbientLightSensor (12) events; colour only when field 0xc0006 (colour space) is set.
//   * CoreBrightness also sets ReportInterval, ALSIntegrationMode, IOHIDALSContinuousMode and IOHIDBacklightLevel on the
//     service. IOHIDEventDriver forwards only ReportInterval (to the descriptor's Report Interval feature), which this
//     device does not take (IOHIDFamily logs "setValue failed"); the rate here is the sensor's.
// The HID service is a SwitchHIDDevice (kSwitchHIDLight) and its IOHIDEventDriver. IOHIDEventDriver has no ambient light
// element of its own, so the events are made here and handed to that driver's dispatchEvent (switch_hid_dispatch_event).
//
// IOHIDEvent is IOHIDFamily's class and has no binding here; its factories are statics, but setType is virtual and is
// initWithType: an IOHIDEvent allocated through its metaclass and given setType(12) is the same object
// ambientLightSensorEvent() returns. Slots in the 21H461 IOHIDFamily (vtable 0xfffffff005d07318 in our kernel): 16
// getTimeStamp (ldr x0,[x0,#0x30]), 17 setTimeStamp (str x1,[x0,#0x30]), 18 getType (data->type), 19 setType (b
// initWithType(this, type, 0)), 24 setIntegerValue(key, value, options); _data at +0x10.
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"
#include "../SwitchTouch/SwitchHID.h"
#include "SwitchBrightness.h"
#include "TegraPanel.h"

namespace {
constexpr unsigned kBus = 2;
constexpr uint8_t kSensor = 0x29;

// BH1730FVC: a command byte 0x80 | register (auto-increment) or 0xe0 | special command (bh1730.c, hekate als.h).
enum : uint8_t { RegControl = 0x00, RegTiming = 0x01, RegGain = 0x07, RegId = 0x12, RegData0 = 0x14 };
constexpr uint8_t kCommand = 0x80, kSoftwareReset = 0xe4;
constexpr uint8_t kPowerOn = 1u << 0, kAdcEnable = 1u << 1;
constexpr unsigned kCycles = 38;                     // rohm,integration-cycle; TIMING = 256 - cycles
constexpr unsigned kIntegrationUS = 2700 * kCycles;  // bh1730.c BH1730_ITIME_TO_US: 102.6 ms (+ 2 ms ADC)
constexpr unsigned kLuxMultiplier = 3600, kLuxMax = 100000;

const unsigned kGainFactor[4] = {1, 2, 64, 128};
struct GainLimits {
    uint32_t cl, fl;
};
const GainLimits kGainLimits[4] = {{2500, 0}, {1600, 7800}, {10, 50000}, {0, 1000}};  // tegra210b01-fric.dts
struct Window {
    uint32_t rc, cv, ci;
};
const Window kWindow[] = {{231, 9697, 7502}, {993, 3333, 2778}, {1478, 1621, 1053}, {7500, 81, 10}};

constexpr unsigned kFramesPerSample = 7;  // 116.7 ms at 60 Hz, one conversion (104.6 ms) and some
constexpr unsigned kClockDeadlineMS = 2000;
constexpr unsigned kErrorLimit = 20;       // samples in a row the sensor did not answer (about 2.3 s)
constexpr unsigned kUnclaimedLimit = 300;  // samples with no IOHIDEventDriver on the HID device (about 35 s)
constexpr unsigned kStatsSamples = 43;     // one log line per 5 s of samples

constexpr unsigned kSetTimeStamp = 17, kSetType = 19, kSetIntegerValue = 24;
constexpr uint32_t kAmbientLightType = 12, kFieldLevel = 0xc0000, kFieldChannel0 = 0xc0001, kFieldChannel1 = 0xc0002;

template <typename R, typename... A> R vcall(OSObject *object, unsigned slot, A... args)
{
    return reinterpret_cast<R (*)(OSObject *, A...)>((*reinterpret_cast<uintptr_t *const *>(object))[slot])(object,
                                                                                                            args...);
}

int writeRegister(uint8_t reg, uint8_t value)
{
    return tegra_i2c_write(kBus, kSensor, kCommand | reg, &value, 1);
}

// For the line that turns the sensor off: its supplies and its bus as they are now. The BH1730 has two (L4T
// tegra210b01-fric-power-tree.dtsi bh1730fvc@29): als-vid = MAX77620 LDO6 (2.9 V, which SwitchTouch switches) and
// als-vdd = vdd-3v3 (MAX77620 GPIO3). LDO6_CFG 7:6 is the power mode (3 = on) and 5:0 the voltage (42 = 2.9 V), LDO6_CFG2
// bit 3 its power-OK, FPS_LDO6 7:6 its sequencer source (3 = none); GPIO3 bit 3 the output and bit 2 the input level,
// AME_GPIO bit 3 set when GPIO3 is an alternate function (hekate max77620.h). SCL (PJ2) and SDA (PJ3) read 1 while the
// bus idles pulled up.
void describeSupplies(char *buffer, size_t size)
{
    constexpr unsigned kPmicBus = 5;
    constexpr uint8_t kPmic = 0x3c, kLdo6Cfg = 0x2f, kGpio3 = 0x39, kAmeGpio = 0x40, kFpsLdo6 = 0x4c;
    uint8_t ldo[2] = {}, gpio3 = 0, ame = 0, fps = 0;  // LDO6_CFG and LDO6_CFG2 (0x30), one auto-increment read
    int err = tegra_i2c_read(kPmicBus, kPmic, kLdo6Cfg, ldo, sizeof(ldo));
    if (!err) err = tegra_i2c_read(kPmicBus, kPmic, kGpio3, &gpio3, 1);
    if (!err) err = tegra_i2c_read(kPmicBus, kPmic, kAmeGpio, &ame, 1);
    if (!err) err = tegra_i2c_read(kPmicBus, kPmic, kFpsLdo6, &fps, 1);
    snprintf(
        buffer, size,
        "LDO6_CFG %#x (mode %u, %u mV) LDO6_CFG2 %#x (POK %u) FPS_LDO6 %#x, GPIO3 %#x AME %#x (PMIC read %d); SCL %d "
        "SDA %d",
        ldo[0], ldo[0] >> 6, 800 + 50 * (ldo[0] & 0x3f), ldo[1], (ldo[1] >> 3) & 1, fps, gpio3, ame, err,
        tegra_gpio_read(TEGRA_GPIO(J, 2)), tegra_gpio_read(TEGRA_GPIO(J, 3)));
}

uint32_t lux(uint32_t visible, uint32_t ir, unsigned gain)
{
    if (!visible) return 0;
    int64_t weighted = 0;
    for (const Window &w : kWindow) {
        if (1000ull * ir / visible < w.rc) {
            weighted = (int64_t)w.cv * visible - (int64_t)w.ci * ir;
            break;
        }
    }
    if (weighted <= 0) return 0;  // bh1730.c computes this in u64 and wraps; dark is the right reading
    const uint64_t value =
        (uint64_t)weighted * 100 * kLuxMultiplier / ((uint64_t)kGainFactor[gain] * kIntegrationUS * 1000);
    return value > kLuxMax ? kLuxMax : (uint32_t)value;
}
}  // namespace

class SwitchALS : public IOService {
    OSDeclareDefaultStructors(SwitchALS) IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *clock = nullptr;  // signalled from the DC A frame-end filter
    IOTimerEventSource *deadline = nullptr;   // the clock's kClockDeadlineMS
    IOService *hid = nullptr;
    unsigned gain = 2;  // 64x, as bh1730_init_config and hekate
    unsigned errors = 0, unclaimed = 0, skip = 0;
    bool lit = true, suspended = false, identified = false, configured = false;
    bool broken = false;  // a sensor or HID path that is not as expected: logged once, sampling off for this boot
    uint64_t ticksSeen = 0;
    // Frame-end filter state: `running` gates it; `frames` is the filter's own; `ticks` counts its signals.
    bool running = false;
    unsigned frames = 0;
    uint64_t ticks = 0;
    // The stats window.
    unsigned samples = 0, luxMin = ~0u, luxMax = 0;

    static void ticked(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void deadlinePassed(OSObject *owner, IOTimerEventSource *sender);
    void update();
    void giveUp();
    int configure();
    void sample();
    void dispatch(uint32_t level, uint32_t visible, uint32_t ir);
    void noteStats(uint32_t level, uint32_t visible, uint32_t ir);
    void setCondition(bool SwitchALS::*condition, bool value);

public:
    bool start(IOService *provider) override;
    void free() override;
    void frameEnd();
    void setLit(bool on) { setCondition(&SwitchALS::lit, on); }
    void setSuspended(bool on) { setCondition(&SwitchALS::suspended, on); }
    bool published() const { return hid != nullptr; }
};

OSDefineMetaClassAndStructors(SwitchALS, IOService)

    static SwitchALS *volatile gALS;

// Primary interrupt context (TegraPanel's frame-end filter), every DC A FRAME_END.
void SwitchALS::frameEnd()
{
    if (!__atomic_load_n(&running, __ATOMIC_ACQUIRE) || ++frames < kFramesPerSample) return;
    frames = 0;
    __atomic_fetch_add(&ticks, 1, __ATOMIC_RELAXED);
    clock->interruptOccurred(nullptr, nullptr, 0);
}

// Work loop. The sensor samples while the panel is lit and the console awake; off, it is powered down (it keeps its
// registers while LDO6 stays on) and the frame ends are ignored.
void SwitchALS::update()
{
    const bool want = lit && !suspended && !broken;
    if (want == __atomic_load_n(&running, __ATOMIC_RELAXED)) return;
    if (want) {
        frames = 0;
        configured = false;  // at the first sample: the sensor may have lost power
        ticksSeen = __atomic_load_n(&ticks, __ATOMIC_RELAXED);
        __atomic_store_n(&running, true, __ATOMIC_RELEASE);
        return;
    }
    __atomic_store_n(&running, false, __ATOMIC_RELEASE);
    deadline->cancelTimeout();
    if (configured) (void)writeRegister(RegControl, 0);  // power down; a sensor that is gone shows on the next start
    configured = false;
}

// Work loop. The ALS is not worth a boot: a part that is not a BH1730, a sensor that does not answer and an event path
// nothing takes each log one line (the caller's) and leave the sensor off; a lost display interrupt still panics.
void SwitchALS::giveUp()
{
    broken = true;
    update();
}

// The bus and a software reset once, then gain, integration time and continuous conversion (hekate als_power_on,
// bh1730_power_on and bh1730_init_config). The conversion in flight is the old configuration's, so the next sample is
// skipped. Nothing touches the hardware before this, the first sample: start() only publishes.
int SwitchALS::configure()
{
    int err;
    if (!identified) {
        uint8_t id = 0;
        if ((err = tegra_i2c_init(kBus))) return err;  // idempotent
        if ((err = tegra_i2c_read(kBus, kSensor, kCommand | RegId, &id, 1))) return err;
        // BH1730FVC PART ID register: part number 0111 in bits 7:4.
        if ((id >> 4) != 7) {
            IOLog("SwitchALS: off: I2C2 0x29 is not a BH1730 (id %#x)\n", id);
            broken = true;  // sample() turns the sensor off
            return -1;
        }
        const uint8_t reset = 0;
        if ((err = tegra_i2c_write(kBus, kSensor, kSoftwareReset, &reset, 1))) return err;
        identified = true;
        IOLog("SwitchALS: started (BH1730 id %#x, a sample every %u frame ends)\n", id, kFramesPerSample);
    }
    err = writeRegister(RegGain, (uint8_t)gain);
    if (!err) err = writeRegister(RegTiming, (uint8_t)(256 - kCycles));
    if (!err) err = writeRegister(RegControl, kPowerOn | kAdcEnable);
    skip = 1;
    return err;
}

void SwitchALS::dispatch(uint32_t level, uint32_t visible, uint32_t ir)
{
    OSObject *event = OSMetaClass::allocClassWithName("IOHIDEvent");
    if (!event) {
        IOLog("SwitchALS: off: OSMetaClass::allocClassWithName(\"IOHIDEvent\") failed (IOHIDFamily not loaded?)\n");
        giveUp();
        return;
    }
    vcall<void, uint32_t>(event, kSetType, kAmbientLightType);
    // initWithType allocates the event data; without it every accessor would dereference null.
    if (!*reinterpret_cast<void *const *>(reinterpret_cast<const uint8_t *>(event) + 0x10)) {
        IOLog("SwitchALS: off: IOHIDEvent setType(%u) left no event data (wrong vtable slot or no memory)\n",
              kAmbientLightType);
        event->release();
        giveUp();
        return;
    }
    vcall<void, uint64_t>(event, kSetTimeStamp, mach_absolute_time());
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSetIntegerValue, kFieldLevel, (int32_t)level, 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSetIntegerValue, kFieldChannel0, (int32_t)visible, 0);
    vcall<void, uint32_t, int32_t, IOOptionBits>(event, kSetIntegerValue, kFieldChannel1, (int32_t)ir, 0);
    // kIOReturnNotReady until IOHIDEventDriver has started on the device; CoreBrightness never sees the sensor without it.
    const IOReturn status = switch_hid_dispatch_event(hid, event);
    event->release();
    if (status == kIOReturnSuccess) {
        unclaimed = 0;
    } else if (++unclaimed >= kUnclaimedLimit) {
        IOLog("SwitchALS: off: no IOHIDEventDriver took the ALS HID device in %u samples (dispatch %#x)\n",
              kUnclaimedLimit, status);
        giveUp();
    }
}

// Periodic, to the log only: what the sensor saw over the last 5 s, and the panel level it saw it at.
void SwitchALS::noteStats(uint32_t level, uint32_t visible, uint32_t ir)
{
    if (level < luxMin) luxMin = level;
    if (level > luxMax) luxMax = level;
    if (++samples < kStatsSamples) return;
    os_log(OS_LOG_DEFAULT, "SwitchALS: %u samples, lux %u (%u..%u), ch0 %u ch1 %u gain %ux, panel %d", samples, level,
           luxMin, luxMax, visible, ir, kGainFactor[gain], tegra_panel_brightness());
    samples = 0;
    luxMin = ~0u;
    luxMax = 0;
}

// Work loop, every kFramesPerSample frame ends while running. The first one after start or wake configures the sensor.
void SwitchALS::sample()
{
    ticksSeen = __atomic_load_n(&ticks, __ATOMIC_RELAXED);
    deadline->setTimeoutMS(kClockDeadlineMS);
    uint8_t data[4] = {};  // DATA0 (visible) and DATA1 (infrared), little endian, in one auto-increment read
    const int err = configured ? tegra_i2c_read(kBus, kSensor, kCommand | RegData0, data, sizeof(data)) : configure();
    if (err) {
        if (broken) {
            giveUp();  // configure() logged why
        } else if (++errors >= kErrorLimit) {
            char supplies[200];
            describeSupplies(supplies, sizeof(supplies));
            IOLog("SwitchALS: off: BH1730 on I2C2 0x29 %s: I2C error %d (%s) %u times in a row; %s\n",
                  identified ? "stopped answering" : "never answered", err,
                  err == TEGRA_I2C_ENACK ? "no ACK" : "bus fault", errors, supplies);
            giveUp();
        }
        return;
    }
    errors = 0;
    if (!configured) {
        configured = true;
        return;
    }
    if (skip) {
        --skip;
        return;
    }
    const uint32_t visible = (uint32_t)data[1] << 8 | data[0], ir = (uint32_t)data[3] << 8 | data[2];
    const uint32_t level = lux(visible, ir, gain);
    dispatch(level, visible, ir);
    noteStats(level, visible, ir);

    const uint32_t highest = visible > ir ? visible : ir;
    unsigned next = gain;
    if (gain > 0 && highest > kGainLimits[gain].fl) {
        --next;
    } else if (gain < 3 && highest < kGainLimits[gain].cl &&
               (uint64_t)highest * kGainFactor[gain + 1] / kGainFactor[gain] < kGainLimits[gain + 1].fl) {
        ++next;
    }
    if (next == gain) return;
    if (writeRegister(RegGain, (uint8_t)next)) return;  // counted on the next read if the sensor is gone
    gain = next;
    skip = 1;  // the conversion in flight finishes at the old gain
}

void SwitchALS::ticked(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchALS *>(owner);
    if (__atomic_load_n(&self->running, __ATOMIC_ACQUIRE)) self->sample();
}

// The frame ends stopped while the panel is lit: the display lost its interrupt. A tick this work loop has not taken
// yet only means the loop was late.
void SwitchALS::deadlinePassed(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = static_cast<SwitchALS *>(owner);
    if (!__atomic_load_n(&self->running, __ATOMIC_ACQUIRE)) return;
    if (__atomic_load_n(&self->ticks, __ATOMIC_RELAXED) != self->ticksSeen) {
        sender->setTimeoutMS(kClockDeadlineMS);
        return;
    }
    panic("SwitchALS: no DC A frame end in %u ms with the panel lit (display interrupt lost)", kClockDeadlineMS);
}

// Thread context: the panel goes dark or lights up (SwitchBacklight), or the console sleeps or wakes (SwitchSuspend).
void SwitchALS::setCondition(bool SwitchALS::*condition, bool value)
{
    struct Change {
        bool SwitchALS::*condition;
        bool value;
    } change = {condition, value};
    loop->runAction(
        [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchALS *>(owner);
            const auto *change = static_cast<const Change *>(arg);
            self->*change->condition = change->value;
            self->update();
            return kIOReturnSuccess;
        },
        this, &change);
}

bool SwitchALS::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    loop = IOWorkLoop::workLoop();
    clock = IOInterruptEventSource::interruptEventSource(this, &SwitchALS::ticked);
    deadline = IOTimerEventSource::timerEventSource(this, &SwitchALS::deadlinePassed);
    if (!loop || !clock || !deadline || loop->addEventSource(clock) != kIOReturnSuccess ||
        loop->addEventSource(deadline) != kIOReturnSuccess) {
        panic("SwitchALS: work loop setup failed");
    }
    hid = switch_hid_create(kSwitchHIDLight);
    registerService();
    setLit(switch_display_is_on());
    return true;
}

void SwitchALS::free()
{
    if (loop) {
        if (clock) loop->removeEventSource(clock);
        if (deadline) loop->removeEventSource(deadline);
    }
    OSSafeReleaseNULL(clock);
    OSSafeReleaseNULL(deadline);
    OSSafeReleaseNULL(loop);
    IOService::free();
}

extern "C" int switch_als_start(void)
{
    unsigned enabled = 1;
    PE_parse_boot_argn("switch-als", &enabled, sizeof(enabled));
    if (!enabled || gALS) return 0;
    IOService *resources = IOService::getResourceService();
    SwitchALS *candidate = OSTypeAlloc(SwitchALS);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources) ||
        !candidate->start(resources)) {
        panic("SwitchALS: attach or start failed");
    }
    __atomic_store_n(&gALS, candidate, __ATOMIC_RELEASE);
    candidate->release();  // the attachment keeps it
    return 0;
}

extern "C" bool switch_als_published(void)
{
    SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_ACQUIRE);
    return als && als->published();
}

extern "C" void switch_als_frame_end(void)
{
    if (SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_ACQUIRE)) als->frameEnd();
}

extern "C" void switch_als_set_active(bool active)
{
    if (SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_ACQUIRE)) als->setLit(active);
}

extern "C" void switch_als_suspend(void)
{
    if (SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_ACQUIRE)) als->setSuspended(true);
}

extern "C" void switch_als_resume(void)
{
    if (SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_ACQUIRE)) als->setSuspended(false);
}
