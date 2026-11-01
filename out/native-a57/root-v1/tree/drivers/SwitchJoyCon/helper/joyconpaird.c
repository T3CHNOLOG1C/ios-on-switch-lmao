// SPDX-License-Identifier: BSD-2-Clause-Patent
// joyconpaird: hands the kernel's rail pairings of Joy-Cons to bluetoothd.
//
// The rail driver (../SwitchJoyConRail.cpp) pairs a Joy-Con to the console's Bluetooth address over the rail and
// publishes one SwitchJoyConPairing registry entry per Joy-Con: AddressString ("xx:xx:xx:xx:xx:xx"), LinkKey (16 bytes,
// HCI order), Name, ClassOfDevice. bluetoothd keeps classic link keys in the system keychain (generic password,
// service "MobileBluetooth", account = address, access group com.apple.bluetooth: BTKCSetData in bluetoothd) and the
// device records in the com.apple.MobileBluetooth.devices preferences domain. Neither is reachable from the kernel,
// and writing them behind bluetoothd's back would race its caches; MobileBluetooth.framework has the call Apple uses
// for accessories paired over a wire instead:
//   BTAccessoryManagerSetLinkKeyEx(manager, address[6], name, classOfDevice, serviceMask, linkKey[16], &device)
// (XPC kCBMsgIdAccessorySetLinkKeyExMsg; bluetoothd BTAccessoryMsgHandler::handleBTAccessorySetLinkKeyExMsg creates
// the paired device and stores the key itself). The address bytes come from BTDeviceAddressFromString, so they are in
// bluetoothd's own order. Service mask 0x20 is "ServiceHID" in bluetoothd's record writer.
//
// Launched by launchd on IOKit matching of IOProviderClass SwitchJoyConPairing (com.ioswitch.joyconpaird.plist), runs
// as root with com.apple.bluetooth.system/.internal (what sharingd holds for MobileBluetooth's accessory calls),
// imports every record present, and exits when idle. MobileBluetooth is private: it is loaded with dlopen, and the
// prototypes below are from its disassembly (21H461).
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <os/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <xpc/xpc.h>

typedef void *BTSession;
typedef void *BTAccessoryManager;
typedef void *BTDevice;
typedef struct {
    uint8_t bytes[6];
} BTDeviceAddress;
typedef void (*BTSessionEventCallback)(BTSession session, int event, int result, void *context);
// Event 0 with result 0 = attached, 1 detached, 2 terminated, 3 failed (GameController's JoyConHIDServicePlugin).
typedef struct {
    BTSessionEventCallback sessionEvent;
    void *reserved[3];
} BTSessionCallbacks;

static int (*BTSessionAttachWithQueue)(const char *name, const BTSessionCallbacks *callbacks, void *context,
                                       dispatch_queue_t queue);
static int (*BTAccessoryManagerGetDefault)(BTSession session, BTAccessoryManager *manager);
static int (*BTDeviceAddressFromString)(const char *string, BTDeviceAddress *address);
static int (*BTAccessoryManagerSetLinkKeyEx)(BTAccessoryManager manager, const BTDeviceAddress *address,
                                             const char *name, uint32_t classOfDevice, uint32_t serviceMask,
                                             const uint8_t key[16], BTDevice *device);

enum { kServiceHID = 0x20, kIdleSeconds = 30 };

static dispatch_queue_t queue;
static BTAccessoryManager manager;
static IONotificationPortRef port;
static io_iterator_t records;
static os_log_t logger;

static void scheduleExit(void)
{
    static dispatch_source_t timer;
    if (!timer) {
        timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        dispatch_source_set_event_handler(timer, ^{ exit(0); });
        dispatch_resume(timer);
    }
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)kIdleSeconds * NSEC_PER_SEC),
                              DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
}

static CFTypeRef property(io_service_t service, const char *key, CFTypeID type)
{
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    CFTypeRef value = name ? IORegistryEntryCreateCFProperty(service, name, kCFAllocatorDefault, 0) : NULL;
    if (name) CFRelease(name);
    if (value && CFGetTypeID(value) != type) {
        CFRelease(value);
        value = NULL;
    }
    return value;
}

static void import(io_service_t service)
{
    CFStringRef addressString = property(service, "AddressString", CFStringGetTypeID());
    CFStringRef nameString = property(service, "Name", CFStringGetTypeID());
    CFDataRef key = property(service, "LinkKey", CFDataGetTypeID());
    CFNumberRef cod = property(service, "ClassOfDevice", CFNumberGetTypeID());
    char address[18] = "", name[32] = "Joy-Con";
    uint32_t classOfDevice = 0x000508;
    BTDeviceAddress bytes;
    BTDevice device = NULL;
    int result = -1;
    if (addressString && key && CFDataGetLength(key) == 16 &&
        CFStringGetCString(addressString, address, sizeof(address), kCFStringEncodingUTF8)) {
        if (nameString) CFStringGetCString(nameString, name, sizeof(name), kCFStringEncodingUTF8);
        if (cod) CFNumberGetValue(cod, kCFNumberSInt32Type, &classOfDevice);
        result = BTDeviceAddressFromString(address, &bytes);
        if (!result) {
            result = BTAccessoryManagerSetLinkKeyEx(manager, &bytes, name, classOfDevice, kServiceHID,
                                                    CFDataGetBytePtr(key), &device);
        }
    }
    if (result)
        os_log_error(logger, "joyconpaird: %{public}s (%{public}s) not imported: %d", name, address, result);
    else
        os_log(logger, "joyconpaird: %{public}s (%{public}s) paired with bluetoothd", name, address);
    if (addressString) CFRelease(addressString);
    if (nameString) CFRelease(nameString);
    if (key) CFRelease(key);
    if (cod) CFRelease(cod);
}

static void matched(void *context, io_iterator_t iterator)
{
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        import(service);
        IOObjectRelease(service);
    }
    scheduleExit();
}

static void sessionEvent(BTSession session, int event, int result, void *context)
{
    if (event != 0 || result != 0) {
        os_log_error(logger, "joyconpaird: bluetoothd session event %d result %d", event, result);
        if (event != 0) exit(1);
        return;
    }
    int error = BTAccessoryManagerGetDefault(session, &manager);
    if (error) {
        os_log_error(logger, "joyconpaird: no accessory manager: %d", error);
        exit(1);
    }
    // Records already published, then any the rail driver publishes while this process lives.
    port = IONotificationPortCreate(kIOMainPortDefault);
    IONotificationPortSetDispatchQueue(port, queue);
    kern_return_t kr = IOServiceAddMatchingNotification(
        port, kIOFirstMatchNotification, IOServiceMatching("SwitchJoyConPairing"), matched, NULL, &records);
    if (kr != KERN_SUCCESS) {
        os_log_error(logger, "joyconpaird: IOServiceAddMatchingNotification: %#x", kr);
        exit(1);
    }
    matched(NULL, records);
}

int main(void)
{
    logger = os_log_create("com.ioswitch.joyconpaird", "pairing");
    queue = dispatch_queue_create("com.ioswitch.joyconpaird", DISPATCH_QUEUE_SERIAL);
    void *mb = dlopen("/System/Library/PrivateFrameworks/MobileBluetooth.framework/MobileBluetooth", RTLD_NOW);
    BTSessionAttachWithQueue = mb ? dlsym(mb, "BTSessionAttachWithQueue") : NULL;
    BTAccessoryManagerGetDefault = mb ? dlsym(mb, "BTAccessoryManagerGetDefault") : NULL;
    BTDeviceAddressFromString = mb ? dlsym(mb, "BTDeviceAddressFromString") : NULL;
    BTAccessoryManagerSetLinkKeyEx = mb ? dlsym(mb, "BTAccessoryManagerSetLinkKeyEx") : NULL;
    if (!BTSessionAttachWithQueue || !BTAccessoryManagerGetDefault || !BTDeviceAddressFromString ||
        !BTAccessoryManagerSetLinkKeyEx) {
        os_log_error(logger, "joyconpaird: MobileBluetooth unavailable: %{public}s", dlerror());
        return 1;
    }
    // launchd started this job for an IOKit match: take the stream's events so it does not hold them for us (the
    // registry is read directly above). The SDK marks the call unavailable on iOS; libxpc has it (gamecontrollerd uses
    // LaunchEvents the same way).
    void (*setStreamHandler)(const char *, dispatch_queue_t, xpc_handler_t) =
        (void (*)(const char *, dispatch_queue_t, xpc_handler_t))dlsym(RTLD_DEFAULT, "xpc_set_event_stream_handler");
    if (setStreamHandler) setStreamHandler("com.apple.iokit.matching", queue, ^(xpc_object_t event) { (void)event; });

    static const BTSessionCallbacks callbacks = {sessionEvent, {NULL, NULL, NULL}};
    dispatch_async(queue, ^{
        int error = BTSessionAttachWithQueue("com.ioswitch.joyconpaird", &callbacks, NULL, queue);
        if (error) {
            os_log_error(logger, "joyconpaird: BTSessionAttachWithQueue: %d", error);
            exit(1);
        }
        scheduleExit();
    });
    dispatch_main();
}
