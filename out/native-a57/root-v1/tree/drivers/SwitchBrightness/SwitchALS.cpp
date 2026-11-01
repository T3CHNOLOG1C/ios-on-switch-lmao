// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Switch OLED's ambient light sensor as iPadOS CoreBrightness's ALS (boot-arg switch-als=1).
//
// Hardware: ROHM BH1730FVC on I2C2 (0x7000C400) at 0x29, in the console body behind the front glass.
//   * switchroot L4T: nx-platforms/tegra210-nx-sensor.dtsi i2c@7000c400 bh1730fvc@29 (compatible rohm,bh1730fvc,
//     integration-cycle 38, lux-multiplier 3600; status disabled "only enable on distros with proper auto-brightness"),
//     overridden for the OLED in tegra210b01-fric.dts (rohm,opt-win-coeff and rohm,gain-coeff below). Driver:
//     CTCaer/switch-l4t-kernel-4.9 drivers/iio/light/bh1730.c. hekate bdk/input/als.c drives the same part on I2C2.
//   * Power: MAX77620 LDO6, "Touch Panel, ALS" (tegra210b01-nx-pmic.dtsi), which SwitchTouch turns on. So
//     switch_als_start() runs after switch_touch_start().
//   * No interrupt: the DT node has none, and the OLED pinmux leaves the reference design's ALS pin (als_prox_int_px3) an
//     input-disabled, pulled-down pad (tegra210b01-fric-pinmux.dtsi), so the BH1730's INT output is not wired. L4T reads
//     the sensor on demand and switchroot Android's light HAL (android_hardware_stmicroelectronics_sensorhal_iio
//     src/Light.cpp, "bh1730 light sensor support (sysfs based)") reads it on a timer of one integration time. Here the
//     sensor converts continuously and a timer takes each finished conversion: the timer is the sensor's own cadence,
//     not a stand-in for a lost interrupt.
//   * Lux: Linux bh1730_get_lux with the OLED coefficients. lux = (cv * DATA0 - ci * DATA1) * 100 ms /
//     (gain * integration time) * multiplier / 1000, with (cv, ci) from the first optical-window row whose rc exceeds
//     1000 * DATA1 / DATA0. Gain steps as bh1730_adjust_gain: down when the larger channel passes the gain's fl, up when it
//     falls under its cl. (hekate's unused Aula table differs in row 0 ci, 30300 against the DT's 7502; the DT is what
//     L4T and Android use on this console.)
//
// iPadOS side (21H461 CoreBrightness, scratchpad/power/re/cb.dis): -[BLControl startHIDSystemClient] matches HID services
// with DeviceUsagePairs {0xff00, 0x0004} (Apple vendor page, AmbientLightSensor); -[CBABModuleiOS addHIDServiceClient:]
// makes a CBALSNode for a service conforming to that pair, and -[CBALSEvent initWithHIDEvent:andService:] takes the lux
// from IOHIDEventGetIntegerValue(event, 0xc0000) of event type 12 (kIOHIDEventTypeAmbientLightSensor); colour only when
// field 0xc0006 (colour space) is 1. The HID service is a SwitchHIDDevice (kSwitchHIDLight: top-level collection
// 0xff00/0x0004 with a Sensor-page Report Interval feature, which IOHIDEventDriver needs to start) and its
// IOHIDEventDriver. IOHIDEventDriver has no ambient light element of its own, so the events are made here and handed to
// that driver's dispatchEvent (switch_hid_dispatch_event). backboardd's ColourSensorFilterPlugin only takes services
// whose IONameMatched is als,ct7xx / als,tcs3490 (Apple's chip drivers), so it leaves this one alone.
//
// IOHIDEvent is IOHIDFamily's class and has no binding here; its factories are statics, but setType is virtual and is
// initWithType: an IOHIDEvent allocated through its metaclass and given setType(12) is the same object
// ambientLightSensorEvent() returns. Slots in the 21H461 IOHIDFamily (vtable 0xfffffff005d07318 in our kernel): 17
// setTimeStamp (str x1,[x0,#0x30]), 19 setType (b initWithType(this, type, 0)), 24 setIntegerValue(key, value, options);
// _data at +0x10.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <pexpert/pexpert.h>
#include "../TegraI2C/TegraI2C.h"
#include "../SwitchTouch/SwitchHID.h"
#include "SwitchBrightness.h"

namespace {
constexpr unsigned kBus = 2;
constexpr uint8_t kSensor = 0x29;

// BH1730FVC: a command byte 0x80 | register (auto-increment) or 0xe0 | special command (bh1730.c, hekate als.h).
enum : uint8_t { RegControl = 0x00, RegTiming = 0x01, RegGain = 0x07, RegId = 0x12, RegData0 = 0x14, RegData1 = 0x16 };
constexpr uint8_t kCommand = 0x80, kSoftwareReset = 0xe4;
constexpr uint8_t kPowerOn = 1u << 0, kAdcEnable = 1u << 1;
constexpr unsigned kCycles = 38;                     // rohm,integration-cycle; TIMING = 256 - cycles
constexpr unsigned kIntegrationUS = 2700 * kCycles;  // bh1730.c BH1730_ITIME_TO_US: 102.6 ms
constexpr unsigned kPeriodMS = 110;                  // integration + 2 ms ADC (BH1730_ADC_CALC_DELAY_US), rounded up
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

constexpr unsigned kSetTimeStamp = 17, kSetType = 19, kSetIntegerValue = 24;
constexpr uint32_t kAmbientLightType = 12, kFieldLevel = 0xc0000, kFieldChannel0 = 0xc0001, kFieldChannel1 = 0xc0002;
constexpr unsigned kErrorLimit = 20;       // conversions in a row the sensor did not answer (about 2 s)
constexpr unsigned kUnclaimedLimit = 300;  // conversions with no IOHIDEventDriver on the HID device (about 33 s)

template <typename R, typename... A> R vcall(OSObject *object, unsigned slot, A... args)
{
    return reinterpret_cast<R (*)(OSObject *, A...)>((*reinterpret_cast<uintptr_t *const *>(object))[slot])(object,
                                                                                                            args...);
}

int writeRegister(uint8_t reg, uint8_t value)
{
    return tegra_i2c_write(kBus, kSensor, kCommand | reg, &value, 1);
}

int readWord(uint8_t reg, uint32_t *value)
{
    uint8_t b[2] = {};
    const int err = tegra_i2c_read(kBus, kSensor, kCommand | reg, b, sizeof(b));
    *value = (uint32_t)b[1] << 8 | b[0];
    return err;
}

uint32_t lux(uint32_t visible, uint32_t ir, unsigned gain)
{
    if (!visible) return 0;
    int64_t value = 0;
    for (const Window &w : kWindow) {
        if (1000ull * ir / visible < w.rc) {
            value = (int64_t)w.cv * visible - (int64_t)w.ci * ir;
            break;
        }
    }
    if (value <= 0) return 0;  // bh1730.c computes this in u64 and wraps; dark is the right reading
    const uint64_t result =
        (uint64_t)value * 100 / ((uint64_t)kGainFactor[gain] * kIntegrationUS) * kLuxMultiplier / 1000;
    return result > kLuxMax ? kLuxMax : (uint32_t)result;
}
}  // namespace

class SwitchALS : public IOService {
    OSDeclareDefaultStructors(SwitchALS) IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOService *hid = nullptr;
    unsigned gain = 2;  // 64x, as bh1730_init_config and hekate
    unsigned errors = 0, unclaimed = 0;
    bool active = false, skipNext = false;

    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    void sample();
    void dispatch(uint32_t level, uint32_t visible, uint32_t ir);
    int configure();

public:
    bool start(IOService *provider) override;
    void free() override;
    void setActive(bool on);
};

OSDefineMetaClassAndStructors(SwitchALS, IOService)

    static SwitchALS *volatile gALS;

// Gain, integration time, continuous conversion on (bh1730_power_on / bh1730_init_config; hekate als_power_on).
int SwitchALS::configure()
{
    int err = writeRegister(RegGain, (uint8_t)gain);
    if (!err) err = writeRegister(RegTiming, (uint8_t)(256 - kCycles));
    if (!err) err = writeRegister(RegControl, kPowerOn | kAdcEnable);
    skipNext = true;  // the conversion in flight started before this configuration
    return err;
}

void SwitchALS::dispatch(uint32_t level, uint32_t visible, uint32_t ir)
{
    OSObject *event = OSMetaClass::allocClassWithName("IOHIDEvent");
    if (!event) panic("SwitchALS: OSMetaClass::allocClassWithName(\"IOHIDEvent\") failed (IOHIDFamily not loaded?)");
    vcall<void, uint32_t>(event, kSetType, kAmbientLightType);
    // initWithType allocates the event data; without it every accessor would dereference null.
    if (!*reinterpret_cast<void *const *>(reinterpret_cast<const uint8_t *>(event) + 0x10)) {
        panic("SwitchALS: IOHIDEvent setType(%u) left no event data (wrong vtable slot or no memory)",
              kAmbientLightType);
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
        panic("SwitchALS: no IOHIDEventDriver took the ALS HID device in %u s (dispatch %#x): personality not matched",
              kUnclaimedLimit * kPeriodMS / 1000, status);
    }
}

// Work loop, once per conversion while the panel is lit.
void SwitchALS::sample()
{
    uint32_t visible = 0, ir = 0;
    int err = readWord(RegData0, &visible);
    if (!err) err = readWord(RegData1, &ir);
    if (err) {
        if (++errors >= kErrorLimit) {
            panic("SwitchALS: BH1730 on I2C2 0x29 stopped answering: I2C error %d %u times in a row", err, errors);
        }
        return;
    }
    errors = 0;
    if (skipNext) {
        skipNext = false;
        return;
    }
    const uint32_t highest = visible > ir ? visible : ir;
    unsigned next = gain;
    if (highest > kGainLimits[gain].fl && gain > 0) {
        --next;
    } else if (highest < kGainLimits[gain].cl && gain < 3) {
        ++next;
    }
    if (next != gain) {
        gain = next;
        if (writeRegister(RegGain, (uint8_t)gain)) return;  // counted on the next read if the sensor is gone
        skipNext = true;
        return;
    }
    dispatch(lux(visible, ir, gain), visible, ir);
}

void SwitchALS::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = OSDynamicCast(SwitchALS, owner);
    if (!self || !self->active) return;
    self->sample();
    sender->setTimeoutMS(kPeriodMS);
}

void SwitchALS::setActive(bool on)
{
    loop->runAction(
        [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchALS *>(owner);
            const bool want = arg != nullptr;
            if (want == self->active) return kIOReturnSuccess;
            self->active = want;
            if (want) {
                if (self->configure()) panic("SwitchALS: BH1730 on I2C2 0x29 did not answer on wake");
                self->timer->setTimeoutMS(kPeriodMS);
            } else {
                self->timer->cancelTimeout();
                writeRegister(RegControl, 0);  // power down; a failure shows on wake
            }
            return kIOReturnSuccess;
        },
        this, on ? this : nullptr);
}

bool SwitchALS::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    int err = tegra_i2c_init(kBus);
    if (err) panic("SwitchALS: I2C2 init failed: %d", err);
    uint8_t id = 0;
    err = tegra_i2c_read(kBus, kSensor, kCommand | RegId, &id, 1);
    // BH1730FVC PART ID register: part number 0111 in bits 7:4.
    if (err || (id >> 4) != 7) {
        panic("SwitchALS: no BH1730 on I2C2 0x29: I2C error %d, id %#x (LDO6 off? SwitchTouch must start first)", err,
              id);
    }
    const uint8_t reset = 0;
    err = tegra_i2c_write(kBus, kSensor, kSoftwareReset, &reset, 1);  // as hekate als_power_on
    if (!err) err = configure();
    if (err) panic("SwitchALS: BH1730 configuration failed: I2C error %d", err);

    loop = IOWorkLoop::workLoop();
    timer = loop ? IOTimerEventSource::timerEventSource(this, &SwitchALS::timerFired) : nullptr;
    if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) panic("SwitchALS: work loop setup failed");
    hid = switch_hid_create(kSwitchHIDLight);
    active = switch_display_is_on();
    if (active) {
        timer->setTimeoutMS(kPeriodMS);
    } else {
        writeRegister(RegControl, 0);
    }
    registerService();
    IOLog("SwitchALS: started (BH1730 id %#x)\n", id);
    return true;
}

void SwitchALS::free()
{
    if (timer) {
        timer->cancelTimeout();
        if (loop) loop->removeEventSource(timer);
    }
    OSSafeReleaseNULL(timer);
    OSSafeReleaseNULL(loop);
    IOService::free();
}

extern "C" int switch_als_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("switch-als", &enabled, sizeof(enabled)) || enabled != 1) return 0;
    if (gALS) return 0;
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

extern "C" void switch_als_set_active(bool active)
{
    if (SwitchALS *als = __atomic_load_n(&gALS, __ATOMIC_ACQUIRE)) als->setActive(active);
}
