// SPDX-License-Identifier: BSD-2-Clause-Patent
// The touch screen's and buttons' HID devices (see SwitchHID.h).
//
// SwitchHIDDevice subclasses the original IOHIDDevice through the qualified ABI and bindings of NativeHidInput
// (build/native-hid-input: OriginalHidDeviceABI.h, hid-original-bindings.o). Those bind every IOHIDDevice slot except
// the ones NativeHidInput overrides, so this class overrides exactly that set: free, start, stop, getWorkLoop,
// newTransportString, newProductString, newReportDescriptor. Its metaclass can only be registered once IOHIDFamily is
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
//  * Buttons: Consumer/Consumer Control with Power (0x30), Volume Increment (0xE9), Volume Decrement (0xEA); the
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
        0x05, 0x01, 0x09, 0x30, 0x26, TOUCH_X_MAX, 0x75, 0x10, 0x81, 0x02, /* Generic Desktop X */               \
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
    0x09, 0x30, 0x09, 0xe9, 0x09, 0xea,              // Power, Volume Increment, Volume Decrement
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x95, 0x05, 0x81, 0x03, 0xc0,
};

// Ambient light sensor (SwitchHID.h): Apple vendor AmbientLightSensor, feature report 1 = Sensor Report Interval (0x030e),
// 32 bits, microseconds.
const uint8_t kLightDescriptor[] = {
    0x06, 0x00, 0xff, 0x09, 0x04, 0xa1, 0x01, 0x85, 0x01,  // Apple vendor: AmbientLightSensor, Collection (Application), ID 1
    0x05, 0x20, 0x0a, 0x0e, 0x03,                          // Sensor: Property Report Interval
    0x15, 0x00, 0x27, 0xff, 0xff, 0xff, 0x7f, 0x75, 0x20, 0x95, 0x01, 0xb1, 0x02,  // 0..2^31-1, 32 bits, Feature
    0xc0,
};

struct Profile {
    const uint8_t *descriptor;
    size_t descriptorSize, reportSize;
    uint32_t productID, usagePage, usage;
    const char *product, *transport;
};
const Profile kProfiles[] = {
    {kTouchDescriptor, sizeof(kTouchDescriptor), sizeof(SwitchTouchReport), 0x5101, 0x0d, 0x04, "Switch Touch Screen",
     "SPI"},
    {kButtonsDescriptor, sizeof(kButtonsDescriptor), sizeof(SwitchButtonsReport), 0x5102, 0x0c, 0x01, "Switch Buttons",
     "GPIO"},
    {kLightDescriptor, sizeof(kLightDescriptor), 5, 0x5103, 0xff00, 0x04, "Switch Ambient Light Sensor", "I2C"},
};

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

    IOReturn deliver(OSObject *event)
    {
        if (!event || !event->metaCast("IOHIDEvent")) return kIOReturnBadArgument;
        if (!eventDriver) eventDriver = edge::findDriver(this);
        if (!eventDriver) return kIOReturnNotReady;
        edge::call<void, OSObject *, IOOptionBits>(eventDriver, edge::kDispatch, event, 0);
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(SwitchHIDDevice, IOHIDDevice)

    namespace
{
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

        OSArray *personalities = OSArray::withCapacity(3);
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

IOService *switch_hid_create(SwitchHIDKind kind)
{
    static IOLock *volatile lock;
    if (!lock) {
        IOLock *fresh = IOLockAlloc();
        if (fresh && !OSCompareAndSwapPtr(nullptr, fresh, &lock)) IOLockFree(fresh);
    }
    if (!lock || kind > kSwitchHIDLight) panic("SwitchHID: bad device kind %u or no memory", kind);
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

IOReturn switch_hid_dispatch_event(IOService *device, OSObject *event)
{
    auto *hid = OSDynamicCast(SwitchHIDDevice, device);
    return hid ? hid->deliver(event) : kIOReturnBadArgument;
}

void switch_hid_set_edge_contacts(IOService *device, uint32_t contacts)
{
    edge::contacts = contacts;
}
