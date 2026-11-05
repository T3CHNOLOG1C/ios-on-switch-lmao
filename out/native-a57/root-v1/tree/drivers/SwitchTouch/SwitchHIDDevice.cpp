// SPDX-License-Identifier: BSD-2-Clause-Patent
// The touch screen's and buttons' HID devices (see SwitchHID.h).
//
// SwitchHIDDevice subclasses the original IOHIDDevice through the qualified ABI and bindings of NativeHidInput
// (build/native-hid-input: OriginalHidDeviceABI.h, hid-original-bindings.o). Those bind every IOHIDDevice slot except
// the ones NativeHidInput overrides, so this class overrides exactly that set: free, start, stop, getWorkLoop,
// newTransportString, newProductString, newReportDescriptor; plus slot 183, setReport, as ../SwitchJoyCon does (the
// motion sensors' Report Interval feature, SwitchHID.h). Its metaclass can only be registered once IOHIDFamily is
// loaded, so this file is a late object (Drivers.cmake ioswitch_late_object) whose static initializer
// switch_hid_create() runs, as HidInputProbe.cpp does for NativeHidInput.
//
// Report descriptors are what IOHIDEventDriver (IOHIDFamily-2031.120.4, the 21H461 kext's version) turns into iOS
// events:
//  * Touch: Digitizer/Touch Screen with ten Finger collections. Touch Valid (0x47) = 1 makes each contact a finger
//    (createDigitizerTransducerEventForReport: otherwise "hand"); the Untouch usage (0x34) is present only because
//    it switches the driver to collection dispatch (parseDigitizerElement), one collection event with a child per
//    finger, which is what backboardd's touch processing takes. Contact Identifier is the slot number, so a lifted
//    finger's slot reports tip 0 once (the driver sends the lift) and is then dropped by the driver as a repeat.
//  * Buttons: Consumer/Consumer Control with Power (0x30), Volume Increment (0xE9), Volume Decrement (0xEA) and Menu
//    (0x40, J120's Home button, which SwitchButtons presses for a Bluetooth controller's Home with the panel off); the
//    driver's parseKeyboardElement takes every consumer usage and dispatches keyboard events with usage page 0x0C,
//    the pairs iOS's button handling uses (as AppleM68Buttons; ios-sweet SweetHIDButtons uses the same).
//
// Edge contacts: UIKit derives a touch's edge type only from kIOHIDDigitizerEventFromEdgeTip (0x800) in the finger
// event's mask (_UIEventHIDTouchEdgeTypeForHIDEvent); SpringBoard's home and switcher gestures fail without it. Apple's
// multitouch stack sets it; IOHIDEventDriver never does. As NativeHidInput's edge wrapper (int-163), the touch
// device's event driver gets a copy of its vtable whose dispatchEvent (IOHIDEventService slot 198, the eighth reserved
// slot of IOHIDEventService.h) adds the bit to the finger children whose contact the touch driver marked
// (switch_hid_set_edge_contacts) and to their collection, then calls the original. IOHIDEvent virtual slots of that
// version: getType 18, getIntegerValue 23, setIntegerValue 24; _children at +24 (IOHIDEvent.h).
#include "OriginalHidDeviceABI.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <mach/kmod.h>
#include "SwitchHID.h"

extern kmod_info_t g_kernel_kmod_info;
extern "C" void switch_hid_static_init(void) __asm__("__GLOBAL__sub_I_SwitchHIDDevice.cpp");

namespace {
constexpr uint32_t kVendorID = 0x057e;   // Nintendo
constexpr uint32_t kProbeScore = 10000;  // over AppleUserHIDEventDriver's 1 and IOHIDEventDriver's own 0

// Logical maxima of X and Y, little endian: the report frame (SwitchHID.h kSwitchTouchReportWidth/Height).
#if IOSWITCH_PORTRAIT_NATIVE
#define TOUCH_X_MAX 0xcf, 0x02 /* 719 */
#define TOUCH_Y_MAX 0xff, 0x04 /* 1279 */
#else
#define TOUCH_X_MAX 0xff, 0x04 /* 1279 */
#define TOUCH_Y_MAX 0xcf, 0x02 /* 719 */
#endif
static_assert(kSwitchTouchReportWidth == (IOSWITCH_PORTRAIT_NATIVE ? 720 : 1280), "touch descriptor X range");
#define FINGER                                                                                                    \
    0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02,                             /* Digitizer: Finger, Collection (Logical) */ \
        0x09, 0x42, 0x09, 0x47, 0x09, 0x34,                         /* Tip Switch, Touch Valid, Untouch */        \
        0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, /* 3 x 1 bit, Input (Data, Var, Abs) */       \
        0x95, 0x05, 0x81, 0x03,                                     /* 5 bits padding */                          \
        0x09, 0x51, 0x25, 0x09, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, /* Contact Identifier, 0..9 */                \
        0x05, 0x01, 0x09, 0x30, 0x26, TOUCH_X_MAX, 0x75, 0x10, 0x81, 0x02, /* Generic Desktop X */                \
        0x09, 0x31, 0x26, TOUCH_Y_MAX, 0x81, 0x02,                         /* Y */                                \
        0xc0
const uint8_t kTouchDescriptor[] = {
    0x05,   0x0d,   0x09,   0x04,   0xa1,   0x01,   0x85,   0x01,  // Digitizer: Touch Screen, Collection (Application), Report ID 1
    FINGER, FINGER, FINGER, FINGER, FINGER, FINGER, FINGER, FINGER, FINGER, FINGER, 0xc0,
};
#undef FINGER
#undef TOUCH_X_MAX
#undef TOUCH_Y_MAX
static_assert(sizeof(SwitchTouchReport) == 1 + kSwitchTouchSlots * 6, "touch report layout");

const uint8_t kButtonsDescriptor[] = {
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x01,  // Consumer: Consumer Control, Collection (Application), Report ID 1
    0x09, 0x30, 0x09, 0xe9, 0x09, 0xea, 0x09, 0x40,  // Power, Volume Increment, Volume Decrement, Menu (Home)
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02, 0x95, 0x04, 0x81, 0x03, 0xc0,
};

// Ambient light sensor (SwitchHID.h): Apple vendor AmbientLightSensor, feature report 1 = Sensor Report Interval (0x030e),
// 32 bits, microseconds.
const uint8_t kLightDescriptor[] = {
    0x06, 0x00, 0xff, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x01,  // Apple vendor: AmbientLightSensor, Collection (Application), ID 1
    0x05, 0x20, 0x0a, 0x0e, 0x03,                          // Sensor: Property Report Interval
    0x15, 0x00, 0x27, 0xff, 0xff, 0xff, 0x7f, 0x75, 0x20, 0x95, 0x01, 0xb1, 0x02,  // 0..2^31-1, 32 bits, Feature
    0xc0,
};

// Motion sensors (SwitchHID.h): Apple vendor top-level collection, input report 1 = three Sensor-page values, 32-bit
// signed, in 10^-6 of the unit IOHIDEventDriver's event carries (physical range = logical range, unit exponent -6, so
// getScaledFixedValue gives g or rad/s in 16.16); feature report 1 = Sensor Report Interval and Report Latency (the
// batch interval), microseconds, so the feature handler receives both (SwitchHID.h).
#define MOTION_DESCRIPTOR(usage, x)                                                                                  \
    0x06, 0x00, 0xff, 0x09, usage, 0xa1, 0x01, 0x85, 0x01, /* Apple vendor: usage, Collection (Application), ID 1 */ \
        0x05, 0x20, 0x0a, x, 0x04, 0x0a, x + 1, 0x04, 0x0a, x + 2, 0x04, /* Sensor: X, Y, Z */                       \
        0x17, 0x00, 0x00, 0x00, 0x80, 0x27, 0xff, 0xff, 0xff, 0x7f,      /* Logical -2^31 .. 2^31-1 */               \
        0x37, 0x00, 0x00, 0x00, 0x80, 0x47, 0xff, 0xff, 0xff, 0x7f,      /* Physical the same */                     \
        0x55, 0x0a, 0x75, 0x20, 0x95, 0x03, 0x81, 0x02,                  /* Unit Exponent -6, 3 x 32 bits, Input */  \
        0x0a, 0x0e, 0x03, 0x15, 0x00, 0x35, 0x00, 0x55, 0x00,            /* Report Interval, 0 .., exponent 0 */     \
        0x95, 0x01, 0xb1, 0x02,                                          /* 1 x 32 bits, Feature */                  \
        0x0a, 0x16, 0x03, 0xb1, 0x02,                                    /* Report Latency: 1 x 32 bits, Feature */  \
        0xc0
const uint8_t kAccelerometerDescriptor[] = {
    MOTION_DESCRIPTOR(0x03, 0x53)};                                 // Accelerometer; Acceleration Axis X 0x0453
const uint8_t kGyroDescriptor[] = {MOTION_DESCRIPTOR(0x09, 0x57)};  // Gyro; Angular Velocity X 0x0457
#undef MOTION_DESCRIPTOR

// Device orientation (SwitchHID.h): Sensor/Device Orientation, one bit per AppleVendorMotion usage 0x64..0x6a.
const uint8_t kOrientationDescriptor[] = {
    0x05, 0x20, 0x09, 0x8a, 0xa1, 0x01, 0x85, 0x01,              // Sensor: Device Orientation, Collection (App), ID 1
    0x06, 0x0c, 0xff, 0x19, 0x64, 0x29, 0x6a,                    // AppleVendorMotion: Usage 0x64 .. 0x6a
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x07, 0x81, 0x02,  // 7 x 1 bit, Input (Data, Var, Abs)
    0x95, 0x01, 0x81, 0x03, 0xc0,                                // 1 bit padding
};

struct Profile {
    const uint8_t *descriptor;
    size_t descriptorSize, reportSize;
    uint32_t productID, usagePage, usage;
    const char *product, *transport;
};
// Product IDs: 0x5104 is ../SwitchJoyCon's buttons device.
const Profile kProfiles[] = {
    {kTouchDescriptor, sizeof(kTouchDescriptor), sizeof(SwitchTouchReport), 0x5101, 0x0d, 0x04, "Switch Touch Screen",
     "SPI"},
    {kButtonsDescriptor, sizeof(kButtonsDescriptor), sizeof(SwitchButtonsReport), 0x5102, 0x0c, 0x01, "Switch Buttons",
     "GPIO"},
    {kLightDescriptor, sizeof(kLightDescriptor), 5, 0x5103, 0xff00, 0x04, "Switch Ambient Light Sensor", "I2C"},
    {kAccelerometerDescriptor, sizeof(kAccelerometerDescriptor), sizeof(SwitchMotionReport), 0x5105, 0xff00, 0x03,
     "Switch Accelerometer", "SPI"},
    {kGyroDescriptor, sizeof(kGyroDescriptor), sizeof(SwitchMotionReport), 0x5106, 0xff00, 0x09, "Switch Gyroscope",
     "SPI"},
    {kOrientationDescriptor, sizeof(kOrientationDescriptor), sizeof(SwitchOrientationReport), 0x5107, 0x20, 0x8a,
     "Switch Device Orientation", "SPI"},
};
static_assert(sizeof(kProfiles) / sizeof(kProfiles[0]) == kSwitchHIDOrientation + 1, "one profile per SwitchHIDKind");
static_assert(sizeof(SwitchMotionReport) == 13 && sizeof(SwitchOrientationReport) == 2, "motion report layouts");

namespace edge {
enum : unsigned { kSlots = 256, kDispatch = 198, kGetType = 18, kGetInteger = 23, kSetInteger = 24 };
enum : uint32_t { kDigitizer = 11, kIndex = 0xb0005, kMask = 0xb0007, kFromEdgeTip = 0x800 };
uintptr_t originalDispatch;
volatile uint32_t contacts;

template <typename R, typename... A> R call(void *object, unsigned slot, A... args)
{
    return reinterpret_cast<R (*)(void *, A...)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object, args...);
}

bool isDigitizer(OSObject *event)
{
    return call<uint32_t>(event, kGetType) == kDigitizer;
}

void addEdge(OSObject *event)
{
    const int32_t mask = call<int32_t, uint32_t, IOOptionBits>(event, kGetInteger, kMask, 0);
    call<void, uint32_t, int32_t, IOOptionBits>(event, kSetInteger, kMask, mask | (int32_t)kFromEdgeTip, 0);
}

void dispatch(IOService *driver, OSObject *event, IOOptionBits options)
{
    const uint32_t marked = contacts;
    if (marked && event && event->metaCast("IOHIDEvent") && isDigitizer(event)) {
        OSArray *children = *reinterpret_cast<OSArray **>(reinterpret_cast<uint8_t *>(event) + 24);
        bool any = false;
        for (unsigned i = 0; children && i < children->getCount(); ++i) {
            OSObject *child = children->getObject(i);
            if (!child || !child->metaCast("IOHIDEvent") || !isDigitizer(child)) continue;
            const uint32_t index = (uint32_t)call<int32_t, uint32_t, IOOptionBits>(child, kGetInteger, kIndex, 0);
            if (index < 32 && (marked & (1u << index))) {
                addEdge(child);
                any = true;
            }
        }
        if (any) addEdge(event);  // a collection's mask is the union of its fingers'
    }
    reinterpret_cast<void (*)(IOService *, OSObject *, IOOptionBits)>(originalDispatch)(driver, event, options);
}

// The IOHIDEventDriver that has opened this device's interface (device -> IOHIDInterface -> driver): its handleStart
// opens the interface, so an instance only attached for probing is never taken.
IOService *findDriver(IOService *device)
{
    IOService *found = nullptr;
    OSIterator *interfaces = device->getChildIterator(gIOServicePlane);
    while (interfaces && !found) {
        auto *interface = OSDynamicCast(IOService, interfaces->getNextObject());
        if (!interface) break;
        OSIterator *drivers = interface->getChildIterator(gIOServicePlane);
        while (drivers && !found) {
            auto *driver = OSDynamicCast(IOService, drivers->getNextObject());
            if (!driver) break;
            if (driver->metaCast("IOHIDEventDriver") && interface->isOpen(driver)) found = driver;
        }
        OSSafeReleaseNULL(drivers);
    }
    OSSafeReleaseNULL(interfaces);
    return found;
}

// Gives the driver its own vtable copy with the wrapped dispatchEvent; every other event driver keeps the original.
bool install(IOService *driver)
{
    uintptr_t *table = *reinterpret_cast<uintptr_t **>(driver);
    auto *copy = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));
    if (!copy) return false;
    memcpy(copy, table, kSlots * sizeof(uintptr_t));
    originalDispatch = copy[kDispatch];
    copy[kDispatch] = reinterpret_cast<uintptr_t>(&dispatch);
    __asm__ volatile("dmb ish" ::: "memory");
    *reinterpret_cast<uintptr_t **>(driver) = copy;
    return true;
}
}  // namespace edge
}  // namespace

class SwitchHIDDevice : public IOHIDDevice {
    OSDeclareDefaultStructors(SwitchHIDDevice) IOWorkLoop *loop = nullptr;
    IOBufferMemoryDescriptor *buffer = nullptr;
    const Profile *profile = nullptr;
    IOService *eventDriver = nullptr;
    bool edgeInstalled = false;
    SwitchHIDFeatureHandler featureHandler = nullptr;
    void *featureContext = nullptr;

public:
    bool initWithProfile(const Profile *p)
    {
        profile = p;
        return init();
    }
    IOWorkLoop *getWorkLoop() const override { return loop; }
    OSString *newTransportString() const override { return OSString::withCString(profile->transport); }
    OSString *newProductString() const override { return OSString::withCString(profile->product); }
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override
    {
        *out = IOBufferMemoryDescriptor::withBytes(profile->descriptor, profile->descriptorSize, kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }

    bool start(IOService *provider) override
    {
        loop = IOWorkLoop::workLoop();
        buffer = IOBufferMemoryDescriptor::withCapacity(profile->reportSize, kIODirectionNone);
        if (!loop || !buffer) return false;
        // Matched by this module's personality; Built-In reaches the interface and the event service (backboardd's
        // built-in touch screen filter). DisplayIntegrated comes from the Touch Screen usage (IOHIDEventService).
        setProperty("VendorID", (unsigned long long)kVendorID, 32);
        setProperty("ProductID", (unsigned long long)profile->productID, 32);
        setProperty("Manufacturer", "Nintendo");
        setProperty("Built-In", true);
        timed = timestamps();
        return IOHIDDevice::start(provider);
    }

    void stop(IOService *provider) override { IOHIDDevice::stop(provider); }

    void free() override
    {
        OSSafeReleaseNULL(buffer);
        OSSafeReleaseNULL(loop);
        IOHIDDevice::free();
    }

    IOReturn post(const void *report, size_t length)
    {
        if (length != profile->reportSize) return kIOReturnBadArgument;
        if (profile == &kProfiles[kSwitchHIDTouch] && !edgeInstalled) {
            if (IOService *driver = edge::findDriver(this)) edgeInstalled = edge::install(driver);
        }
        memcpy(buffer->getBytesNoCopy(), report, length);
        return handleReport(buffer, 0 /* kIOHIDReportTypeInput */, 0);
    }

    // IOHIDDevice::handleReport (slot 181, not overridden here) is clock_get_uptime, the _readyForInputReports test,
    // WORKLOOP_LOCK (its event source's lock: the gate of getWorkLoop(), our `loop`), the virtual handleReportWithTime
    // (slot 197) and WORKLOOP_UNLOCK (IOHIDFamily-2031 IOHIDDevice.cpp:1291-1311). postAt is the same with the caller's
    // time; handleReportWithTime makes the same readiness test itself (IOHIDDevice.cpp:2111). Used only after
    // timestamps() found 21H461's code there (from our kernel's prelinked IOHIDFamily): +0x34 ldrb w8, [x19, #0xb0]
    // (_readyForInputReports), +0x48 ldr x1, [sp, #8] (the time), +0x50 ldr x8, [x8, #0x628] (slot 197).
    static constexpr unsigned kHandleReport = 181, kHandleReportWithTime = 197;
    bool timestamps() const
    {
        const uintptr_t *table = *reinterpret_cast<const uintptr_t *const *>(this);
        const uint32_t *code = reinterpret_cast<const uint32_t *>(table[kHandleReport]);
        return code && code[0x34 / 4] == 0x3942c268 && code[0x48 / 4] == 0xf94007e1 && code[0x50 / 4] == 0xf9431508;
    }

    IOReturn postAt(const void *report, size_t length, uint64_t timestamp)
    {
        if (length != profile->reportSize) return kIOReturnBadArgument;
        if (!timed) return kIOReturnUnsupported;
        memcpy(buffer->getBytesNoCopy(), report, length);
        // Under the work loop's gate, as handleReport's WORKLOOP_LOCK takes it (runAction closes and opens it).
        return loop->runAction(
            [](OSObject *owner, void *time, void *, void *, void *) -> IOReturn {
                auto *self = static_cast<SwitchHIDDevice *>(owner);
                return edge::call<IOReturn, uint64_t, IOMemoryDescriptor *, uint32_t, IOOptionBits>(
                    self, kHandleReportWithTime, *static_cast<const uint64_t *>(time), self->buffer,
                    0 /* kIOHIDReportTypeInput */, 0);
            },
            this, &timestamp);
    }
    bool timed = false;  // timestamps(), at start

    IOReturn deliver(OSObject *event)
    {
        if (!event || !event->metaCast("IOHIDEvent")) return kIOReturnBadArgument;
        if (!eventDriver) eventDriver = edge::findDriver(this);
        if (!eventDriver) return kIOReturnNotReady;
        edge::call<void, OSObject *, IOOptionBits>(eventDriver, edge::kDispatch, event, 0);
        return kIOReturnSuccess;
    }

    void setFeatureHandler(SwitchHIDFeatureHandler handler, void *context)
    {
        featureContext = context;
        __atomic_store_n(&featureHandler, handler, __ATOMIC_RELEASE);
    }

    // Slot 183 is IOHIDDevice::setReport(IOMemoryDescriptor *, IOHIDReportType, IOOptionBits) (../SwitchJoyCon's
    // SwitchJoyConDevice::opaque183 has the evidence); the generated ABI only knows it as opaque183. Its definition
    // (after the class) is one branch to switch_hid_set_report with the argument registers untouched.
    void opaque183() override;

    IOReturn setReport(IOMemoryDescriptor *report, uint32_t type, IOOptionBits options)
    {
        const SwitchHIDFeatureHandler handler = __atomic_load_n(&featureHandler, __ATOMIC_ACQUIRE);
        if (!handler || type != 2 /* kIOHIDReportTypeFeature */) return kIOReturnUnsupported;
        uint8_t bytes[16];
        const size_t length = report ? report->getLength() : 0;
        if (!length || length > sizeof(bytes) || report->readBytes(0, bytes, length) != length) {
            return kIOReturnBadArgument;
        }
        handler(featureContext, bytes, length);
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(SwitchHIDDevice, IOHIDDevice)

    namespace
{
    // The ambient light sensor's service properties for CoreBrightness (21H461 -[CBALSNode initALSProperties], read with
    // IOHIDServiceClientCopyProperty from the event service, i.e. this IOHIDEventDriver, which IOKit gives its
    // personality's keys; IOHIDEventService copies none of these from its provider):
    //   * Placement 1, kIOHIDPlacementTypeTop (IOHIDFamily AppleEmbeddedHIDKeys.h): the front of the device. CoreBrightness
    //     takes 2 (Bottom) for its rear ALS (-[CBABModuleiOS addHIDServiceClient:]) and anything else for the front.
    //   * Orientation 8, kIOHIDOrientationTypeSouthWest: where the sensor sits on the screen iOS is given. The BH1730 is
    //     behind the top-left of the bezel in handheld (landscape) use; the portrait panel (IOSwitchPortrait.h) shows
    //     landscape (x, y) at (y, 1279 - x), so the landscape top left is the portrait bottom left. Used only to place
    //     CBDigitizerFilter's hotspot, which needs the device tree's digitizer-filter-window-type (not set here).
    //   * ALSFastIntegrationTime, ALSSuperFastIntegrationTime, ALSSlowIntegrationTime: microseconds (its defaults 99000,
    //     100000, 250000). The BH1730 has one, 102600 (38 cycles of 2.7 us; SwitchALSMath.h), whatever the mode.
    //   * ALSSensorType is not published: its values are not known (one module compares it to 8); the default is 0.
    void addLightProperties(OSDictionary * dict)
    {
        const struct {
            const char *key;
            uint32_t value;
        } entries[] = {
            {"Placement", 1},
            {"Orientation", 8},
            {"ALSFastIntegrationTime", 102600},
            {"ALSSuperFastIntegrationTime", 102600},
            {"ALSSlowIntegrationTime", 102600},
        };
        for (const auto &entry : entries) {
            OSNumber *number = OSNumber::withNumber(entry.value, 32);
            if (!number || !dict->setObject(entry.key, number)) panic("SwitchHID: no memory for the ALS properties");
            number->release();
        }
    }

    // IOHIDEventDriver for one of our devices only: IOHIDFamily's MatchPropertyTable compares VendorID, ProductID and the
    // DeviceUsagePairs against the device.
    OSDictionary *personality(const Profile &p)
    {
        OSDictionary *dict = OSDictionary::withCapacity(8);
        OSDictionary *pair = OSDictionary::withCapacity(2);
        OSArray *pairs = OSArray::withCapacity(1);
        OSNumber *page = OSNumber::withNumber(p.usagePage, 32), *usage = OSNumber::withNumber(p.usage, 32);
        OSNumber *vendor = OSNumber::withNumber(kVendorID, 32), *product = OSNumber::withNumber(p.productID, 32);
        OSNumber *score = OSNumber::withNumber(kProbeScore, 32);
        OSString *ioClass = OSString::withCString("IOHIDEventDriver"),
                 *provider = OSString::withCString("IOHIDInterface");
        OSString *bundle = OSString::withCString("com.apple.iokit.IOHIDFamily");
        bool ok = dict && pair && pairs && page && usage && vendor && product && score && ioClass && provider && bundle;
        if (ok) {
            pair->setObject("DeviceUsagePage", page);
            pair->setObject("DeviceUsage", usage);
            pairs->setObject(pair);
            dict->setObject("IOClass", ioClass);
            dict->setObject("IOProviderClass", provider);
            dict->setObject("CFBundleIdentifier", bundle);
            dict->setObject("IOProbeScore", score);
            dict->setObject("VendorID", vendor);
            dict->setObject("ProductID", product);
            dict->setObject("DeviceUsagePairs", pairs);
            if (&p == &kProfiles[kSwitchHIDLight]) addLightProperties(dict);
        }
        OSSafeReleaseNULL(pair);
        OSSafeReleaseNULL(pairs);
        OSSafeReleaseNULL(page);
        OSSafeReleaseNULL(usage);
        OSSafeReleaseNULL(vendor);
        OSSafeReleaseNULL(product);
        OSSafeReleaseNULL(score);
        OSSafeReleaseNULL(ioClass);
        OSSafeReleaseNULL(provider);
        OSSafeReleaseNULL(bundle);
        if (!ok) OSSafeReleaseNULL(dict);
        return dict;
    }

    // IOHIDFamily loaded, our class registered, our personalities in the catalogue. Callers are serialized by
    // switch_hid_create's lock.
    void prepare()
    {
        static bool ready;
        if (ready) return;
        const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
        if (loaded != kOSReturnSuccess) panic("SwitchHID: loading com.apple.iokit.IOHIDFamily failed: %#x", loaded);
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (!handle) panic("SwitchHID: OSMetaClass::preModLoad failed");
        switch_hid_static_init();
        const OSReturn registered = OSMetaClass::postModLoad(handle);
        if (registered != kOSReturnSuccess) panic("SwitchHID: registering SwitchHIDDevice failed: %#x", registered);

        OSArray *personalities = OSArray::withCapacity(sizeof(kProfiles) / sizeof(kProfiles[0]));
        for (const Profile &p : kProfiles) {
            OSDictionary *dict = personality(p);
            if (!personalities || !dict) panic("SwitchHID: no memory for the IOHIDEventDriver personalities");
            personalities->setObject(dict);
            dict->release();
        }
        if (!gIOCatalogue->addDrivers(personalities, true))
            panic("SwitchHID: IOCatalogue::addDrivers refused the personalities");
        personalities->release();
        ready = true;
    }
}  // namespace

extern "C" __attribute__((used)) IOReturn switch_hid_set_report(SwitchHIDDevice *device, IOMemoryDescriptor *report,
                                                                uint32_t type, IOOptionBits options)
{
    return device->setReport(report, type, options);
}

// SwitchHIDDevice::opaque183(), see the declaration. Darwin assembler: one statement per line.
__asm__(".text\n"
        ".globl __ZN15SwitchHIDDevice9opaque183Ev\n"
        ".p2align 2\n"
        "__ZN15SwitchHIDDevice9opaque183Ev:\n"
        "  b _switch_hid_set_report\n");

IOService *switch_hid_create(SwitchHIDKind kind)
{
    static IOLock *volatile lock;
    if (!lock) {
        IOLock *fresh = IOLockAlloc();
        if (fresh && !OSCompareAndSwapPtr(nullptr, fresh, &lock)) IOLockFree(fresh);
    }
    if (!lock || kind > kSwitchHIDOrientation) panic("SwitchHID: bad device kind %u or no memory", kind);
    IOLockLock(lock);
    prepare();
    IOLockUnlock(lock);

    const Profile &profile = kProfiles[kind];
    IOService *resources = IOService::getResourceService();
    auto *device = OSTypeAlloc(SwitchHIDDevice);
    if (!resources || !device || !device->initWithProfile(&profile) || !device->attach(resources)) {
        panic("SwitchHID: %s: attach failed", profile.product);
    }
    if (!device->start(resources)) panic("SwitchHID: %s: IOHIDDevice::start failed", profile.product);
    device->release();  // the attachment keeps it
    return device;
}

IOReturn switch_hid_post(IOService *device, const void *report, size_t length)
{
    auto *hid = OSDynamicCast(SwitchHIDDevice, device);
    return hid ? hid->post(report, length) : kIOReturnBadArgument;
}

IOReturn switch_hid_post_at(IOService *device, const void *report, size_t length, uint64_t timestamp)
{
    auto *hid = OSDynamicCast(SwitchHIDDevice, device);
    return hid ? hid->postAt(report, length, timestamp) : kIOReturnBadArgument;
}

bool switch_hid_supports_timestamps(IOService *device)
{
    auto *hid = OSDynamicCast(SwitchHIDDevice, device);
    return hid && hid->timed;
}

IOReturn switch_hid_dispatch_event(IOService *device, OSObject *event)
{
    auto *hid = OSDynamicCast(SwitchHIDDevice, device);
    return hid ? hid->deliver(event) : kIOReturnBadArgument;
}

void switch_hid_set_feature_handler(IOService *device, SwitchHIDFeatureHandler handler, void *context)
{
    auto *hid = OSDynamicCast(SwitchHIDDevice, device);
    if (!hid) panic("SwitchHID: feature handler for a device that is not ours");
    hid->setFeatureHandler(handler, context);
}

void switch_hid_set_edge_contacts(IOService *device, uint32_t contacts)
{
    edge::contacts = contacts;
}
