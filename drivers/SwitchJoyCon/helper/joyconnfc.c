// SPDX-License-Identifier: BSD-2-Clause-Patent
// joyconnfc: reads NFC tags (amiibo are NTAG215) with the NFC/IR MCU of a Joy-Con (R) or Pro Controller, on the
// console rail or over Bluetooth.
//
// iPadOS has no NFC stack to hand tags to: CoreNFC's readingAvailable is an XPC question to nfcd
// (com.apple.nfcd.service.corenfc), which drives Apple's NXP controller through libPN548_API and the Stockholm kernel
// driver, none of which a J120 has. So this tool reads the tag itself and leaves the result where a person or another
// program can use it: the UID and page dump go to the system log (subsystem com.ioswitch.joyconnfc) and to
// /private/var/ioswitch/nfc/<UID>.bin (the raw pages, 540 bytes for an NTAG215, the format amiibo tools use).
//
// Transport: the Joy-Con's own HID protocol through its IOHIDDevice, opened shared (JoyConHIDServicePlugin keeps it
// open as well), so one code path serves the rail (SwitchJoyConDevice, the kernel carries the reports, see
// ../SwitchJoyConRail.cpp) and Bluetooth (bluetoothd's IOHIDUserDevice). Output reports: 0x01 subcommands (0x03 input
// report mode, 0x21 MCU configuration, 0x22 MCU state) and 0x11 MCU requests; the MCU answers in the 362-byte input
// report 0x31. Sequence and offsets: CTCaer jc_toolkit jctool.cpp nfc_tag_info() (offsets below are into the report,
// ID at 0, as there) and yuzu input_common/helpers/joycon_protocol/nfc.cpp (EnableNfc, StartNFCPollingMode,
// IsTagInRange, GetAmiiboData; its mcu_data[n] is report byte 50 + n). MCU CRC: CRC-8, polynomial 0x07, init 0.
//   0x31 report: 49 = MCU report (0x01 state, 0x2a NFC state, 0x3a NFC data, 0xff empty); 56 = MCU mode for 0x01;
//   for 0x2a: 50..51 = 00 05, 55 = 0x31, 56 = NFC state (0 ready, 1 polling, 4 last package, 7 tag lost, 9 tag),
//   62 = tag type (2 NTAG), 64 = UID length, 65.. = UID; for 0x3a: 51 = 0x07, 52 = package (1, 2), 54..55 = payload
//   size (11 bits); package 1 carries a 60-byte header (74 = NTAG kind: 0 NTAG215, 3 NTAG213, 4 NTAG216) and its
//   pages from 116, later packages from 56.
// While the Joy-Con streams 0x31, JoyConHIDServicePlugin (which parses 0x21, 0x30 and 0x3F) gets no input from it over
// Bluetooth; on the rail the kernel repeats each 0x31's head as 0x30. The scan ends by restoring report mode 0x30.
// Writing is not offered: the MCU's NTAG write (command 0x08, yuzu WriteAmiiboData) is an amiibo-shaped transfer
// carrying the tag's magic, write counter and version, not a page write.
//
// Started by launchd (com.ioswitch.joyconnfc.plist) when the kernel publishes SwitchJoyConNFC, which it does only with
// switch-joycon-nfc=1. It then stays and scans once for 30 s each time a Joy-Con (R) or Pro Controller appears (attach
// it to the rail again, or reconnect it, to scan again). "joyconnfc --once" scans the controllers present now.
// IOKit's HID calls are exported by IOKit.framework on iOS but the iOS SDK has no hid/ headers: the prototypes below are
// those of macOS IOKit/hid/IOHIDDevice.h.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <os/lock.h>
#include <os/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <xpc/xpc.h>

typedef struct __IOHIDDevice *IOHIDDeviceRef;
typedef enum { kIOHIDReportTypeInput = 0, kIOHIDReportTypeOutput, kIOHIDReportTypeFeature } IOHIDReportType;
typedef void (*IOHIDReportCallback)(void *context, IOReturn result, void *sender, IOHIDReportType type,
                                    uint32_t reportID, uint8_t *report, CFIndex reportLength);
extern IOHIDDeviceRef IOHIDDeviceCreate(CFAllocatorRef allocator, io_service_t service);
extern IOReturn IOHIDDeviceOpen(IOHIDDeviceRef device, IOOptionBits options);
extern IOReturn IOHIDDeviceClose(IOHIDDeviceRef device, IOOptionBits options);
extern void IOHIDDeviceSetDispatchQueue(IOHIDDeviceRef device, dispatch_queue_t queue);
extern void IOHIDDeviceSetCancelHandler(IOHIDDeviceRef device, dispatch_block_t handler);
extern void IOHIDDeviceActivate(IOHIDDeviceRef device);
extern void IOHIDDeviceCancel(IOHIDDeviceRef device);
extern void IOHIDDeviceRegisterInputReportCallback(IOHIDDeviceRef device, uint8_t *report, CFIndex reportLength,
                                                   IOHIDReportCallback callback, void *context);
extern IOReturn IOHIDDeviceSetReport(IOHIDDeviceRef device, IOHIDReportType reportType, CFIndex reportID,
                                     const uint8_t *report, CFIndex reportLength);

enum {
    kNintendo = 0x057e,
    kJoyConR = 0x2007,
    kProController = 0x2009,
    kOutputLength = 49,      // reports 0x01 and 0x11 with their ID (descriptor: 48 bytes)
    kMcuReportLength = 362,  // report 0x31 with its ID
    kQueued = 8,
    kNtag215Bytes = 135 * 4,
    kScanSeconds = 30,
    kSettleSeconds = 3,
    kReplyMS = 250,
};
enum { kSubReportMode = 0x03, kSubMcuConfig = 0x21, kSubMcuState = 0x22 };
enum { kMcuStatus = 0x01, kMcuNfc = 0x02 };  // byte 10 of output report 0x11
enum { kModeStandby = 1, kModeNfc = 4 };
enum { kNfcStartPolling = 0x01, kNfcStopPolling = 0x02, kNfcNextPackage = 0x04, kNfcReadNtag = 0x06 };
enum { kNfcReady = 0x00, kNfcPolling = 0x01, kNfcLastPackage = 0x04, kNfcTagLost = 0x07, kNfcTag = 0x09 };

typedef struct {
    IOHIDDeviceRef device;
    uint8_t buffer[0x200];  // IOHIDDevice's input buffer
    os_unfair_lock lock;    // HID queue <-> scan thread
    uint8_t queue[kQueued][kMcuReportLength];
    size_t length[kQueued];
    unsigned head, tail;  // reports 0x21 and 0x31, oldest first; the oldest go when full
    dispatch_semaphore_t arrived;
    uint8_t counter;
    char name[24];
} Pad;

static os_log_t logger;
static dispatch_queue_t hidQueue, scanQueue;

static uint8_t mcuCrc(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    while (length--) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

// ---- Reports ------------------------------------------------------------------------------------------------------

static void inputReport(void *context, IOReturn result, void *sender, IOHIDReportType type, uint32_t reportID,
                        uint8_t *report, CFIndex length)
{
    Pad *pad = context;
    if (result != kIOReturnSuccess || length < 1 || (report[0] != 0x21 && report[0] != 0x31)) return;
    if (length > kMcuReportLength) length = kMcuReportLength;
    os_unfair_lock_lock(&pad->lock);
    if (pad->tail - pad->head == kQueued) pad->head++;
    const unsigned at = pad->tail++ % kQueued;
    memset(pad->queue[at], 0, kMcuReportLength);
    memcpy(pad->queue[at], report, (size_t)length);
    pad->length[at] = (size_t)length;
    os_unfair_lock_unlock(&pad->lock);
    dispatch_semaphore_signal(pad->arrived);
}

typedef bool (*Match)(const uint8_t *report, const void *arg);

// Takes queued reports until one matches (copied to `out`, zero-padded to 362 bytes) or `ms` pass.
static bool await(Pad *pad, Match match, const void *arg, uint8_t *out, int ms)
{
    const dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC);
    for (;;) {
        bool found = false;
        os_unfair_lock_lock(&pad->lock);
        while (!found && pad->head != pad->tail) {
            const unsigned at = pad->head++ % kQueued;
            if (match(pad->queue[at], arg)) {
                memcpy(out, pad->queue[at], kMcuReportLength);
                found = true;
            }
        }
        os_unfair_lock_unlock(&pad->lock);
        if (found) return true;
        if (dispatch_semaphore_wait(pad->arrived, deadline)) return false;
    }
}

static void flush(Pad *pad)
{
    os_unfair_lock_lock(&pad->lock);
    pad->head = pad->tail;
    os_unfair_lock_unlock(&pad->lock);
    while (!dispatch_semaphore_wait(pad->arrived, DISPATCH_TIME_NOW)) {}
}

static IOReturn send(Pad *pad, uint8_t id, uint8_t command, const uint8_t *data, size_t length)
{
    uint8_t report[kOutputLength] = {
        id, (uint8_t)(pad->counter++ & 0xf), 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, command};
    memcpy(report + 11, data, length);
    if (id == 0x11 && command == kMcuNfc) report[47] = mcuCrc(report + 11, 36);
    return IOHIDDeviceSetReport(pad->device, kIOHIDReportTypeOutput, id, report, sizeof(report));
}

static bool isReply(const uint8_t *r, const void *arg)
{
    return r[0] == 0x21 && (r[13] & 0x80) && r[14] == *(const uint8_t *)arg;
}

// One subcommand (report 0x01) and its acknowledged 0x21 reply; three tries (jc_toolkit retries the same way).
static bool subcommand(Pad *pad, uint8_t sub, const uint8_t *data, size_t length)
{
    uint8_t reply[kMcuReportLength];
    for (int attempt = 0; attempt < 3; ++attempt) {
        flush(pad);
        if (send(pad, 0x01, sub, data, length) != kIOReturnSuccess) return false;
        if (await(pad, isReply, &sub, reply, kReplyMS)) return true;
    }
    return false;
}

static bool isMcuMode(const uint8_t *r, const void *arg)
{
    return r[0] == 0x31 && r[49] == 0x01 && r[56] == *(const uint8_t *)arg;
}

static bool waitMcuMode(Pad *pad, uint8_t mode)
{
    uint8_t reply[kMcuReportLength];
    for (int attempt = 0; attempt < 16; ++attempt) {
        flush(pad);
        if (send(pad, 0x11, kMcuStatus, NULL, 0) != kIOReturnSuccess) return false;
        if (await(pad, isMcuMode, &mode, reply, kReplyMS)) return true;
    }
    return false;
}

static bool isNfcState(const uint8_t *r, const void *arg)
{
    (void)arg;
    return r[0] == 0x31 && r[49] == 0x2a && r[50] == 0x00 && r[51] == 0x05;
}

static bool isNfcAnswer(const uint8_t *r, const void *arg)
{
    (void)arg;
    return r[0] == 0x31 && (r[49] == 0x2a || r[49] == 0x3a);
}

// An NFC command (report 0x11, request 0x02): command, block 0, package, "last packet" flag, data length, data.
static IOReturn nfcCommand(Pad *pad, uint8_t command, uint8_t package, const uint8_t *data, uint8_t length)
{
    uint8_t request[36] = {command, 0x00, package, 0x08, length};
    if (length) memcpy(request + 5, data, length);
    return send(pad, 0x11, kMcuNfc, request, sizeof(request));
}

// Asks for the next package until the NFC state is `state` (yuzu WaitUntilNfcIs).
static bool waitNfcState(Pad *pad, uint8_t state, uint8_t *reply)
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        flush(pad);
        if (nfcCommand(pad, kNfcNextPackage, 0, NULL, 0) != kIOReturnSuccess) return false;
        if (await(pad, isNfcState, NULL, reply, kReplyMS) && reply[55] == 0x31 && reply[56] == state) return true;
    }
    return false;
}

// ---- Scan ---------------------------------------------------------------------------------------------------------

static void saveTag(const Pad *pad, const uint8_t *uid, unsigned uidLength, const uint8_t *pages, size_t length)
{
    char hex[2 * 10 + 1] = "";
    for (unsigned i = 0; i < uidLength; ++i) snprintf(hex + 2 * i, 3, "%02X", uid[i]);
    os_log(logger, "joyconnfc: %{public}s: tag %{public}s, %zu bytes of pages", pad->name, hex, length);
    printf("%s: tag %s, %zu bytes of pages\n", pad->name, hex, length);
    for (size_t at = 0; at < length; at += 128) {
        char line[2 * 128 + 1] = "";
        const size_t end = at + 128 < length ? at + 128 : length;
        for (size_t i = at; i < end; ++i) snprintf(line + 2 * (i - at), 3, "%02x", pages[i]);
        os_log(logger, "joyconnfc: %{public}s: page %zu: %{public}s", hex, at / 4, line);
        printf("  page %3zu: %s\n", at / 4, line);
    }
    if (!length) return;
    char path[96];
    mkdir("/private/var/ioswitch", 0755);
    mkdir("/private/var/ioswitch/nfc", 0755);
    snprintf(path, sizeof(path), "/private/var/ioswitch/nfc/%s.bin", hex);
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, pages, length) != (ssize_t)length) {
        os_log_error(logger, "joyconnfc: %{public}s: %{public}s", path, strerror(errno));
    }
    if (fd >= 0) close(fd);
}

// Reads the tag found by polling (yuzu GetAmiiboData): one NTAG read of pages 0-0x86 in three ranges (jc_toolkit's
// NTAG215 layout), then a "next package" request per answer, the package number moving on with each data package,
// until the MCU reports the last one.
static size_t readPages(Pad *pad, uint8_t *pages, uint8_t *reply)
{
    static const uint8_t request[19] = {0xd0, 0x07, 0,    0,    0,    0,    0,    0,    0,   0x00 /* any tag type */,
                                        3,    0x00, 0x3b, 0x3c, 0x77, 0x78, 0x86, 0x00, 0x00};
    size_t have = 0;
    uint8_t package = 0;
    flush(pad);
    if (nfcCommand(pad, kNfcReadNtag, 0, request, sizeof(request)) != kIOReturnSuccess) return 0;
    (void)await(pad, isNfcAnswer, NULL, reply, kReplyMS);
    for (int attempt = 0; attempt < 60; ++attempt) {
        flush(pad);
        if (nfcCommand(pad, kNfcNextPackage, package, NULL, 0) != kIOReturnSuccess) return 0;
        if (!await(pad, isNfcAnswer, NULL, reply, kReplyMS)) continue;
        const bool data = reply[49] == 0x3a && reply[51] == 0x07;  // otherwise byte 56 is the NFC state
        if (!data && reply[56] == kNfcTagLost) {
            os_log_error(logger, "joyconnfc: %{public}s: tag lost or read error %#x", pad->name, reply[50]);
            return 0;
        }
        if (data) {
            const size_t payload = (size_t)((reply[54] << 8) | reply[55]) & 0x7ff;
            const bool first = reply[52] == 0x01;
            const size_t from = first ? 116 : 56, length = first ? (payload > 60 ? payload - 60 : 0) : payload;
            if (first && reply[74] != 0)
                os_log(logger, "joyconnfc: %{public}s: not an NTAG215 (kind %u)", pad->name, reply[74]);
            if (from + length <= kMcuReportLength && have + length <= kNtag215Bytes) {
                memcpy(pages + have, reply + from, length);
                have += length;
            }
            ++package;
            continue;
        }
        if (reply[49] == 0x2a && reply[56] == kNfcLastPackage) return have;
    }
    return 0;
}

static void scan(Pad *pad)
{
    uint8_t reply[kMcuReportLength], pages[kNtag215Bytes];
    const uint8_t nfcMode = 0x31, standardMode = 0x30, mcuOn = 0x01, mcuOff = 0x00;
    uint8_t config[38] = {0x21, 0x00, kModeNfc};  // MCU command "set mode", NFC; CRC over bytes 1-36
    config[37] = mcuCrc(config + 1, 36);
    static const uint8_t polling[5] = {0x00, 0x00, 0x00, 0x2c, 0x01};
    const char *failed = NULL;

    if (!subcommand(pad, kSubReportMode, &nfcMode, 1))
        failed = "input report mode 0x31";
    else if (!subcommand(pad, kSubMcuState, &mcuOn, 1))
        failed = "MCU on";
    else if (!waitMcuMode(pad, kModeStandby))
        failed = "MCU standby";
    else if (!subcommand(pad, kSubMcuConfig, config, sizeof(config)))
        failed = "MCU NFC mode";
    else if (!waitMcuMode(pad, kModeNfc))
        failed = "MCU in NFC mode";
    else if (!waitNfcState(pad, kNfcReady, reply))
        failed = "NFC ready";
    else if (nfcCommand(pad, kNfcStartPolling, 0, polling, sizeof(polling)) != kIOReturnSuccess ||
             !waitNfcState(pad, kNfcPolling, reply))
        failed = "NFC polling";
    if (failed) {
        os_log_error(logger, "joyconnfc: %{public}s: no NFC (%{public}s)", pad->name, failed);
        printf("%s: no NFC (%s)\n", pad->name, failed);
    } else {
        os_log(logger, "joyconnfc: %{public}s: hold a tag to the NFC area (%d s)", pad->name, kScanSeconds);
        printf("%s: hold a tag to the NFC area (%d s)\n", pad->name, kScanSeconds);
        const dispatch_time_t end = dispatch_time(DISPATCH_TIME_NOW, (int64_t)kScanSeconds * NSEC_PER_SEC);
        bool tag = false;
        // The MCU reports only when asked: one "next package" request per answer until a tag shows (yuzu IsTagInRange).
        while (!tag && dispatch_time(DISPATCH_TIME_NOW, 0) < end) {
            flush(pad);
            if (nfcCommand(pad, kNfcNextPackage, 0, NULL, 0) != kIOReturnSuccess) break;
            tag =
                await(pad, isNfcState, NULL, reply, kReplyMS) && (reply[56] == kNfcTag || reply[56] == kNfcLastPackage);
        }
        if (tag) {
            uint8_t uid[10];
            const unsigned uidLength = reply[64] < sizeof(uid) ? reply[64] : sizeof(uid);
            memcpy(uid, reply + 65, uidLength);
            const size_t length = reply[62] == 0x02 ? readPages(pad, pages, reply) : 0;  // 2: NTAG (else MIFARE)
            saveTag(pad, uid, uidLength, pages, length);
        } else {
            os_log(logger, "joyconnfc: %{public}s: no tag", pad->name);
            printf("%s: no tag\n", pad->name);
        }
        nfcCommand(pad, kNfcStopPolling, 0, NULL, 0);
    }
    // Back to what JoyConHIDServicePlugin set up: MCU off, standard full reports.
    subcommand(pad, kSubMcuState, &mcuOff, 1);
    subcommand(pad, kSubReportMode, &standardMode, 1);
}

// ---- Devices ------------------------------------------------------------------------------------------------------

static void scanService(io_service_t service, dispatch_group_t group, int64_t settle)
{
    Pad *pad = calloc(1, sizeof(*pad));
    IOHIDDeviceRef device = pad ? IOHIDDeviceCreate(kCFAllocatorDefault, service) : NULL;
    if (!device) {
        free(pad);
        return;
    }
    CFNumberRef product = IORegistryEntryCreateCFProperty(service, CFSTR("ProductID"), kCFAllocatorDefault, 0);
    int32_t id = 0;
    if (product) {
        CFNumberGetValue(product, kCFNumberSInt32Type, &id);
        CFRelease(product);
    }
    snprintf(pad->name, sizeof(pad->name), "%s", id == kProController ? "Pro Controller" : "Joy-Con (R)");
    pad->device = device;
    pad->lock = OS_UNFAIR_LOCK_INIT;
    pad->arrived = dispatch_semaphore_create(0);
    if (group) dispatch_group_enter(group);
    // Let JoyConHIDServicePlugin finish its own set-up (it sets report mode 0x30 when it activates) before taking over.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, settle * (int64_t)NSEC_PER_SEC), scanQueue, ^{
        const IOReturn opened = IOHIDDeviceOpen(device, 0);
        if (opened != kIOReturnSuccess) {
            os_log_error(logger, "joyconnfc: %{public}s: IOHIDDeviceOpen: %#x", pad->name, opened);
        } else {
            IOHIDDeviceRegisterInputReportCallback(device, pad->buffer, sizeof(pad->buffer), inputReport, pad);
            IOHIDDeviceSetDispatchQueue(device, hidQueue);
            dispatch_semaphore_t cancelled = dispatch_semaphore_create(0);
            IOHIDDeviceSetCancelHandler(device, ^{ dispatch_semaphore_signal(cancelled); });
            IOHIDDeviceActivate(device);
            scan(pad);
            IOHIDDeviceCancel(device);
            dispatch_semaphore_wait(cancelled, DISPATCH_TIME_FOREVER);
            IOHIDDeviceClose(device, 0);
        }
        CFRelease(device);
        free(pad);
        if (group) dispatch_group_leave(group);
    });
}

static void appeared(void *context, io_iterator_t iterator)
{
    io_service_t service;
    while ((service = IOIteratorNext(iterator))) {
        scanService(service, context, kSettleSeconds);
        IOObjectRelease(service);
    }
}

static CFMutableDictionaryRef matching(int32_t product)
{
    CFMutableDictionaryRef dict = IOServiceMatching("IOHIDDevice");
    int32_t vendor = kNintendo;
    CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vendor);
    CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &product);
    CFDictionarySetValue(dict, CFSTR("VendorID"), v);
    CFDictionarySetValue(dict, CFSTR("ProductID"), p);
    CFRelease(v);
    CFRelease(p);
    return dict;
}

// switch-joycon-nfc in the boot-args, as PE_parse_boot_argn reads it (bare name = 1).
static bool enabled(void)
{
    char args[1024] = "";
    size_t size = sizeof(args) - 1;
    if (sysctlbyname("kern.bootargs", args, &size, NULL, 0)) return false;
    static const char name[] = "switch-joycon-nfc";
    for (char *word = strtok(args, " "); word; word = strtok(NULL, " ")) {
        if (strncmp(word, name, sizeof(name) - 1)) continue;
        const char *value = word + sizeof(name) - 1;
        if (!*value) return true;
        if (*value == '=') return strtol(value + 1, NULL, 0) != 0;
    }
    return false;
}

int main(int argc, char **argv)
{
    logger = os_log_create("com.ioswitch.joyconnfc", "nfc");
    hidQueue = dispatch_queue_create("com.ioswitch.joyconnfc.hid", DISPATCH_QUEUE_SERIAL);
    scanQueue = dispatch_queue_create("com.ioswitch.joyconnfc.scan", DISPATCH_QUEUE_SERIAL);
    if (!enabled()) {
        fprintf(stderr, "joyconnfc: switch-joycon-nfc is not set\n");
        return 0;
    }
    static const int32_t products[] = {kJoyConR, kProController};
    if (argc > 1 && !strcmp(argv[1], "--once")) {
        dispatch_group_t group = dispatch_group_create();
        for (unsigned i = 0; i < sizeof(products) / sizeof(products[0]); ++i) {
            io_iterator_t iterator = IO_OBJECT_NULL;
            if (IOServiceGetMatchingServices(kIOMainPortDefault, matching(products[i]), &iterator) != KERN_SUCCESS)
                continue;
            io_service_t service;
            while ((service = IOIteratorNext(iterator))) {
                scanService(service, group, 0);
                IOObjectRelease(service);
            }
            IOObjectRelease(iterator);
        }
        dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
        return 0;
    }
    // launchd started this job for the SwitchJoyConNFC match: take the stream's events so it does not hold them for us
    // (as joyconpaird; the SDK marks the call unavailable on iOS, libxpc exports it).
    void (*setStreamHandler)(const char *, dispatch_queue_t, xpc_handler_t) =
        (void (*)(const char *, dispatch_queue_t, xpc_handler_t))dlsym(RTLD_DEFAULT, "xpc_set_event_stream_handler");
    if (setStreamHandler)
        setStreamHandler("com.apple.iokit.matching", scanQueue, ^(xpc_object_t event) { (void)event; });
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    IONotificationPortSetDispatchQueue(port, scanQueue);
    for (unsigned i = 0; i < sizeof(products) / sizeof(products[0]); ++i) {
        io_iterator_t iterator = IO_OBJECT_NULL;
        const kern_return_t kr = IOServiceAddMatchingNotification(port, kIOFirstMatchNotification,
                                                                  matching(products[i]), appeared, NULL, &iterator);
        if (kr != KERN_SUCCESS) {
            os_log_error(logger, "joyconnfc: IOServiceAddMatchingNotification: %#x", kr);
            return 1;
        }
        appeared(NULL, iterator);  // arms the notification; controllers already present are scanned too
    }
    dispatch_main();
}
