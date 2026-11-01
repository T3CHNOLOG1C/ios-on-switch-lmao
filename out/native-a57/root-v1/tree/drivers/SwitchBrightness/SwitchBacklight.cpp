// SPDX-License-Identifier: BSD-2-Clause-Patent
// The built-in display's backlight for iPadOS 17.7 CoreBrightness, and the display's power, driving the Switch OLED panel
// through TegraPanel (DCS commands; there is no PWM backlight). Boot-args in SwitchBrightness.h.
//
// What CoreBrightness expects (21H461; addresses in its dyld cache image and the kernelcache, scratchpad/power/re):
//   * -[BLControl findBacklight] / -[CBDisplayContaineriOS findBacklight] look up a registered service with
//     backlight-control = true; handleDisplayArrival: makes the built-in display module (CBDisplayModuleiOS) only if that
//     service conforms to "AppleARMBacklight" (IOObjectConformsTo, i.e. metaCast by class name). An IOHIDDevice gets the
//     external-display module instead.
//   * The module reads IODisplayParameters.brightness {min, max, value} (32-bit integers) and fails without it. Apple's
//     AppleARMBacklight publishes brightness 0..65536 (value 32768 at start), rawBrightness 0..2047, BrightnessMicroAmps
//     and BrightnessMilliNits.
//   * It writes with IORegistryEntrySetCFProperties(service, dict): "brightness" = ceil(fraction * (max - min)) + min,
//     fraction being linear luminance relative to maximum; "FadePeriod" = seconds * 65536 to ramp over; sometimes
//     "backlight-level" (not used here). A failed write makes it drop that update. It also sets IOMFBBrightnessLevel on
//     the framebuffer unless the backlight carries ignore-IOMFB.
// The original class lives in com.apple.driver.AppleARMPlatform, which is prelinked here (a dependency of
// IOMobileGraphicsFamily), so the name cannot be defined again (duplicate classes panic in OSMetaClass::postModLoad).
// SwitchBacklight is a plain IOService whose metaclass names the original AppleARMBacklight metaclass as its superclass
// (bound in original-bindings.S): metaCast and matching treat it as a subclass, and no AppleARMBacklight code ever runs on
// it. postModLoad only follows the superclass chain to count its depth, and the original metaclass is still zeroed
// storage when the kernel's own classes register, so this is not a late object. Kernel code that casts to
// AppleARMBacklight and touches its fields would break; in this kernelcache that is AppleARMPlatform itself, AppleSPU,
// AppleEmbeddedLightSensor and AppleM68Buttons, none of which is started here.
//
// Display power. On lock (power button, auto-lock) the render server in backboardd calls
// IOMobileFramebufferRequestPowerChange(fb, 0) (QuartzCore, through IOMobileFramebuffer's selector 12) and on wake (fb, 1).
// The original family only acts on that when a platform subclass has marked itself power capable, and then it only flips
// its own powered word; the hardware is the platform's. Here the framebuffer hands the request to
// switch_display_power_request(), and this work loop plays it: off = TegraPanel's L4T suspend sequence while the DC still
// scans out, then the display stops the DC (no fetches, no frame-end or vblank interrupts, so the cores sit in WFI); on =
// the display restarts the DC, then TegraPanel wakes the panel and restores the brightness.
//
// Requests (CoreBrightness's setProperties, the framebuffer's power requests) never wait for this work loop: they are
// left under a lock and the work timer is kicked (IOTimerEventSource::setTimeout takes no gate). The framebuffer's
// request comes from its own work loop, and this loop calls back into that one (native_legacy_display_set_scanout) and
// waits for frame ends it delivers, so waiting here would deadlock. The panel's 0x51 value (0..2047, hekate calls it
// candela) is taken as linear in luminance, so brightness maps linearly onto kFloorRaw..2047: the slider's bottom (and a
// brightness of 0) is dim but never dark, as on an iPad; only display power turns the panel off. A panel command takes
// four frames and blocks this loop, so at most one is ever out; a fade or a slider drag is played as a sequence of
// commands, at most one every three frames (50 ms), each toward the latest request: requests that arrive meanwhile only
// replace the target. The panel's own dimming (DCS 0x53 bit 3) smooths the steps between them.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include "TegraPanel.h"
#include "SwitchBrightness.h"

// The original AppleARMBacklight::gMetaClass (original-bindings.S), declared with a concrete type so that its address is
// a constant initializer. Nothing here constructs or calls it.
class OriginalBacklightMetaClass : public OSMetaClass {
public:
    OSObject *alloc() const override;
};
extern const OriginalBacklightMetaClass gOriginalAppleARMBacklightMeta __asm__("__ZN17AppleARMBacklight10gMetaClassE");

// The display driver's strong definition wins when it is linked; without it the panel still goes dark but the DC keeps
// scanning out.
extern "C" __attribute__((weak)) bool native_legacy_display_set_scanout(bool)
{
    return false;
}

namespace {
constexpr uint32_t kMax = 65536;  // IODisplayParameters.brightness max, as Apple's
constexpr uint64_t kLongestFadeNS = 10000000000ull;
constexpr uint64_t kSendIntervalNS = 50000000;  // three frames at 60 Hz between panel commands
constexpr unsigned kFloorRaw = 64;              // the dimmest panel value the slider reaches (~3% of full)
constexpr unsigned kSpanRaw = TEGRA_PANEL_BRIGHTNESS_MAX - kFloorRaw;

volatile bool gDisplayOn = true;

unsigned toRaw(uint32_t brightness)
{
    return kFloorRaw + (unsigned)(((uint64_t)brightness * kSpanRaw + kMax / 2) / kMax);
}

uint32_t toBrightness(unsigned raw)
{
    return raw <= kFloorRaw ? 0 : (uint32_t)((uint64_t)(raw - kFloorRaw) * kMax / kSpanRaw);
}

OSDictionary *range(uint32_t max, uint32_t value)
{
    OSDictionary *dict = OSDictionary::withCapacity(3);
    const struct {
        const char *key;
        uint32_t value;
    } entries[] = {{"min", 0}, {"max", max}, {"value", value}};
    for (const auto &entry : entries) {
        OSNumber *number = OSNumber::withNumber(entry.value, 32);
        if (dict && number) dict->setObject(entry.key, number);
        OSSafeReleaseNULL(number);
    }
    return dict;
}

bool bootArg(const char *name)
{
    unsigned value = 0;
    return PE_parse_boot_argn(name, &value, sizeof(value)) && value == 1;
}
}  // namespace

class SwitchBacklight : public IOService {
    OSDeclareDefaultStructors(SwitchBacklight) IOWorkLoop *loop = nullptr;
    IOTimerEventSource *work = nullptr;
    IOLock *requestLock = nullptr;

    // Requests, under requestLock.
    bool haveRequest = false, wantOn = true;
    uint32_t requestValue = 0;
    uint64_t requestFadeNS = 0;

    // Work loop state.
    uint32_t current = 0, from = 0, target = 0;  // brightness, 0..kMax
    uint32_t lit = kMax / 2;                     // the last nonzero target, restored on wake
    uint64_t rampStart = 0, rampLength = 0;      // mach_absolute_time units
    uint64_t lastSend = 0, sendInterval = 0;     // mach_absolute_time units
    unsigned sentRaw = ~0u;
    bool panelOn = true, scanoutStopped = false, reportedFailure = false;

    static void workFired(OSObject *owner, IOTimerEventSource *sender);
    void kick() { work->setTimeoutUS(1); }
    void startRamp(uint32_t value, uint64_t fadeNS);
    void step();
    void powerOff();
    void powerOn();
    bool send(unsigned raw);
    void publishParameters();

public:
    bool brightnessCommands = false, sleepEnabled = false;
    bool start(IOService *provider) override;
    void free() override;
    IOReturn setProperties(OSObject *properties) override;
    void requestPower(bool on);
};

// OSDefineMetaClassAndStructors(SwitchBacklight, IOService), except that the metaclass's superclass is the original
// AppleARMBacklight metaclass (see above). The C++ base class stays IOService.
SwitchBacklight::MetaClass SwitchBacklight::gMetaClass;
const OSMetaClass *const SwitchBacklight::metaClass = &SwitchBacklight::gMetaClass;
const OSMetaClass *const SwitchBacklight::superClass = &gOriginalAppleARMBacklightMeta;
SwitchBacklight::SwitchBacklight(const OSMetaClass *meta) : IOService(meta) {}
SwitchBacklight::~SwitchBacklight() {}
const OSMetaClass *SwitchBacklight::getMetaClass() const
{
    return &gMetaClass;
}
OSMetaClassConstructorInit(SwitchBacklight, IOService, ) OSDefineDefaultStructors(SwitchBacklight, IOService)

    static SwitchBacklight *volatile gBacklight;

void SwitchBacklight::publishParameters()
{
    OSDictionary *parameters = OSDictionary::withCapacity(2);
    OSDictionary *brightness = range(kMax, current);
    OSDictionary *raw = range(TEGRA_PANEL_BRIGHTNESS_MAX, toRaw(current));
    if (parameters && brightness && raw) {
        parameters->setObject("brightness", brightness);
        parameters->setObject("rawBrightness", raw);
        setProperty("IODisplayParameters", parameters);
    }
    OSSafeReleaseNULL(raw);
    OSSafeReleaseNULL(brightness);
    OSSafeReleaseNULL(parameters);
}

bool SwitchBacklight::send(unsigned raw)
{
    if (raw == sentRaw) return true;
    const int err = tegra_panel_set_brightness(raw);
    if (err) {
        if (!reportedFailure) IOLog("SwitchBacklight: panel brightness %u failed (%d)\n", raw, err);
        reportedFailure = true;
        return false;
    }
    reportedFailure = false;
    sentRaw = raw;
    return true;
}

// Work loop.
void SwitchBacklight::startRamp(uint32_t value, uint64_t fadeNS)
{
    from = current;
    target = value > kMax ? kMax : value;
    if (target) lit = target;
    rampStart = mach_absolute_time();
    nanoseconds_to_absolutetime(fadeNS > kLongestFadeNS ? kLongestFadeNS : fadeNS, &rampLength);
}

// Work loop. The DC still scans out here; it is stopped only once the panel is asleep.
void SwitchBacklight::powerOff()
{
    switch_als_set_active(false);
    const int err = tegra_panel_power_off();
    if (err) IOLog("SwitchBacklight: panel off failed (%d): the display is not running\n", err);
    scanoutStopped = native_legacy_display_set_scanout(false);
    panelOn = false;
    sentRaw = ~0u;
}

// Work loop. TegraPanel's first step waits for a frame end of the restarted DC, and panics if none comes.
void SwitchBacklight::powerOn()
{
    if (scanoutStopped && !native_legacy_display_set_scanout(true)) {
        panic("SwitchBacklight: the display did not restart DC A's scanout for wake");
    }
    scanoutStopped = false;
    const int level = brightnessCommands ? (int)toRaw(target ? target : lit) : -1;
    const int err = tegra_panel_power_on(level);
    if (err) IOLog("SwitchBacklight: panel on failed (%d): the display is not running\n", err);
    panelOn = true;
    sentRaw = err || level < 0 ? ~0u : (unsigned)level;
    if (target == 0 && level >= 0) target = current = lit;
    gDisplayOn = true;
    switch_als_set_active(true);
}

// Work loop: the latest requests, display power first, then one step of the current ramp. Re-arms itself until the
// ramp reaches its target; a panel command blocks this loop for four frames.
void SwitchBacklight::step()
{
    IOLockLock(requestLock);
    const bool on = wantOn, fresh = haveRequest;
    const uint32_t value = requestValue;
    const uint64_t fadeNS = requestFadeNS;
    haveRequest = false;
    IOLockUnlock(requestLock);

    if (fresh) startRamp(value, fadeNS);
    if (on != panelOn) {
        if (on) {
            powerOn();
        } else {
            powerOff();
        }
    }
    gDisplayOn = panelOn;  // also when a request to go dark was taken back before it ran
    // Dark, or brightness not driven: the request is only recorded, and played on wake.
    if (!panelOn || !brightnessCommands) {
        current = target;
        publishParameters();
        return;
    }
    uint32_t next = target;
    const uint64_t elapsed = mach_absolute_time() - rampStart;
    if (rampLength && elapsed < rampLength) {
        next = (uint32_t)((int64_t)from + ((int64_t)target - (int64_t)from) * (int64_t)elapsed / (int64_t)rampLength);
    }
    const unsigned raw = toRaw(next);
    if (raw != sentRaw) {
        const uint64_t now = mach_absolute_time();
        if (lastSend && now - lastSend < sendInterval) {
            work->wakeAtTime(lastSend + sendInterval);  // the latest target is taken then
            return;
        }
        lastSend = now;
    }
    if (!send(raw)) next = target;  // give up on this ramp, keep the request
    current = next;
    if (current != target) {
        work->setTimeoutMS(1);
    } else {
        publishParameters();
    }
}

void SwitchBacklight::workFired(OSObject *owner, IOTimerEventSource *)
{
    if (auto *self = OSDynamicCast(SwitchBacklight, owner)) self->step();
}

IOReturn SwitchBacklight::setProperties(OSObject *properties)
{
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict) return kIOReturnBadArgument;
    OSNumber *brightness = OSDynamicCast(OSNumber, dict->getObject("brightness"));
    if (!brightness) return kIOReturnSuccess;  // FadePeriod / backlight-level alone: nothing to do
    OSNumber *fade = OSDynamicCast(OSNumber, dict->getObject("FadePeriod"));
    IOLockLock(requestLock);
    requestValue = brightness->unsigned32BitValue();
    requestFadeNS = fade ? fade->unsigned64BitValue() * 1000000000ull / 65536 : 0;
    haveRequest = true;
    IOLockUnlock(requestLock);
    kick();
    return kIOReturnSuccess;
}

void SwitchBacklight::requestPower(bool on)
{
    if (!on) gDisplayOn = false;  // input is ignored from the request on, not only once the panel is dark
    IOLockLock(requestLock);
    wantOn = on;
    IOLockUnlock(requestLock);
    kick();
}

bool SwitchBacklight::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    brightnessCommands = bootArg("switch-backlight");
    nanoseconds_to_absolutetime(kSendIntervalNS, &sendInterval);
    sleepEnabled = bootArg("switch-sleep");
    requestLock = IOLockAlloc();
    loop = IOWorkLoop::workLoop();
    work = loop ? IOTimerEventSource::timerEventSource(this, &SwitchBacklight::workFired) : nullptr;
    if (!requestLock || !work || loop->addEventSource(work) != kIOReturnSuccess) {
        IOLog("SwitchBacklight: work loop setup failed\n");
        return false;
    }
    // Start from what the boot chain left on the panel; nothing is sent until CoreBrightness asks.
    const int boot = tegra_panel_boot_brightness();
    current = target = boot >= 0 ? toBrightness((unsigned)boot) : kMax / 2;
    if (current) lit = current;
    setName("backlight");
    setProperty("backlight-control", kOSBooleanTrue);
    // CoreBrightness reads ignore-IOMFB (CFData, nonzero) from the backlight and its parents and then clears its use-IOMFB
    // flag (display module init, 0x1b62b55fc): the framebuffer here is a stand-in, so it should not get
    // IOMFBBrightnessLevel writes.
    uint32_t ignore = 1;
    setProperty("ignore-IOMFB", &ignore, sizeof(ignore));
    publishParameters();
    // backboardd sets FadePeriod/brightness properties on this service; its sandbox only allows that on the original
    // class, so our XNU grants it by name (IOUserClient.cpp native_sandbox_grant).
    setProperty("NativeSandboxGrantProcess", "backboardd");
    registerService();
    IOLog("SwitchBacklight: started (panel brightness %s, display power %s)\n", brightnessCommands ? "on" : "off",
          sleepEnabled ? "on" : "off");
    return true;
}

void SwitchBacklight::free()
{
    if (work) {
        work->cancelTimeout();
        if (loop) loop->removeEventSource(work);
        work->release();
    }
    OSSafeReleaseNULL(loop);
    if (requestLock) IOLockFree(requestLock);
    IOService::free();
}

extern "C" int switch_backlight_start(void)
{
    if (gBacklight) return 0;
    const OSSymbol *name = OSSymbol::withCStringNoCopy("AppleARMBacklight");
    const OSMetaClass *original = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (original != static_cast<const OSMetaClass *>(&gOriginalAppleARMBacklightMeta)) {
        IOLog("SwitchBacklight: AppleARMBacklight is %p, binding %p (AppleARMPlatform not loaded?)\n", original,
              &gOriginalAppleARMBacklightMeta);
        return -1;
    }
    IOService *resources = IOService::getResourceService();
    SwitchBacklight *candidate = OSTypeAlloc(SwitchBacklight);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        IOLog("SwitchBacklight: attach failed\n");
        OSSafeReleaseNULL(candidate);
        return -1;
    }
    if (!candidate->start(resources)) {
        candidate->detach(resources);
        candidate->release();
        return -1;
    }
    __atomic_store_n(&gBacklight, candidate, __ATOMIC_RELEASE);
    return 0;
}

extern "C" bool switch_display_power_request(bool on)
{
    SwitchBacklight *backlight = __atomic_load_n(&gBacklight, __ATOMIC_ACQUIRE);
    if (!backlight || !backlight->sleepEnabled) return false;
    backlight->requestPower(on);
    return true;
}

extern "C" bool switch_display_is_on(void)
{
    return gDisplayOn;
}
