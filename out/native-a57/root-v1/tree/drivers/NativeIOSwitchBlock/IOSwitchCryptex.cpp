// SPDX-License-Identifier: BSD-2-Clause-Patent
// Attach the unchanged System/App cryptex images from Preboot with the original
// DiskImages hook and report the APFS volume each one publishes.
#include <IOKit/IOBSD.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/param.h>
#include <sys/ubc.h>
#include <sys/vnode.h>
#include "IOSwitchCryptex.h"

extern "C" {
int di_root_image_ext(const char *, char *, size_t, dev_t *, bool);
void ioswitch_cluster_register_image(vnode_t);                          // bsd/vfs/vfs_cluster.c
void ioswitch_cluster_record_start(vnode_t, uint8_t *map, uint32_t blocks);  // bsd/vfs/vfs_cluster.c
void ioswitch_cluster_record_stop(void);                                // bsd/vfs/vfs_cluster.c
void ioswitch_block_prefetch_thread(thread_t);                          // NativeIOSwitchBlock.cpp
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
processor_t cpu_to_processor(int);
void processor_cpu_load_info(processor_t, natural_t ticks[CPU_STATE_MAX]);
int ml_get_max_cpu_number(void);
}

namespace {
const char *const paths[IOSWITCH_CRYPTEX_COUNT] = {"/private/preboot/IOSwitch-Staging/044-76034-038.dmg",
                                                   "/private/preboot/IOSwitch-Staging/044-75188-038.dmg"};
const char *const uuids[IOSWITCH_CRYPTEX_COUNT] = {IOSWITCH_SYSTEM_CRYPTEX_UUID, IOSWITCH_APP_CRYPTEX_UUID};
// Fixed development identifier consumed by the owner boot-manifest code.
const uint8_t systemIdentifier[48] = {0x13, 0x14, 0x8b, 0xb4, 0x2c, 0x8c, 0xe4, 0xb5, 0x6d, 0x83, 0x5d, 0x89,
                                      0xf9, 0x1a, 0x4c, 0xea, 0x1a, 0xc0, 0x48, 0x21, 0x11, 0xdc, 0x34, 0xa0,
                                      0x0c, 0xfd, 0xdf, 0xee, 0x36, 0x03, 0x7c, 0xe5, 0x04, 0x92, 0x3b, 0xaa,
                                      0xe4, 0xba, 0xe2, 0xba, 0xe6, 0xda, 0x7d, 0x82, 0x29, 0xc5, 0x3e, 0x54};
int results[IOSWITCH_CRYPTEX_COUNT] = {ENXIO, ENXIO};
bool attempted[IOSWITCH_CRYPTEX_COUNT];

// The IOMedia carrying this volume UUID once IOMediaBSDClient has named it.
int findVolume(const char *uuid, dev_t *device, char *path, size_t capacity)
{
    OSDictionary *match = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    OSString *wanted = OSString::withCString(uuid);
    if (!match || !properties || !wanted) {
        OSSafeReleaseNULL(match);
        OSSafeReleaseNULL(properties);
        OSSafeReleaseNULL(wanted);
        return ENOMEM;
    }
    properties->setObject("UUID", wanted);
    match->setObject(gIOPropertyMatchKey, properties);
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    properties->release();
    wanted->release();
    if (!it) return EAGAIN;
    int error = EAGAIN;
    while (auto *media = OSDynamicCast(IOService, it->getNextObject())) {
        auto *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
        auto *majorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
        auto *minorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
        if (!name || !majorNumber || !minorNumber) continue;
        *device = makedev(majorNumber->unsigned32BitValue(), minorNumber->unsigned32BitValue());
        snprintf(path, capacity, "/dev/%s", name->getCStringNoCopy());
        error = 0;
        break;
    }
    it->release();
    return error;
}

int outputs(unsigned index, dev_t *device, char *path, size_t capacity)
{
    if (device) *device = NODEV;
    if (path && capacity) path[0] = 0;
    return index < IOSWITCH_CRYPTEX_COUNT && device && path && capacity >= 64 ? 0 : EINVAL;
}
}

// ---- r8-sdboot: boot prefetch of the System cryptex image ----
//
// Until the UI is up the SD card is the boot's bottleneck: ~1.4 GiB in ~230K mostly distinct random 4 KiB reads (11-04
// census, boot-1104-1552/1611), ~1.1 GiB of them dyld shared cache data on the System cryptex, which its APFS (mounted
// on a disk image, so with cluster-layer verification off) pages in 4 KiB at a time through the image file. The same
// bytes read as large sequential runs take a fraction of that card time. So:
//  - record: on a boot with no valid list, vfs_cluster.c notes the first-read second of every 4 KiB block of the image
//    file (one byte per block), from the attach until kRecordAfterFrameS after the first frame, and this file saves
//    the map next to the image (kListSuffix, header keyed on the image file's id, size and mtime);
//  - replay: on every later boot, right after the attach, a kernel thread reads the recorded blocks into the image
//    file's page cache as runs of up to kMaxRun blocks (gaps of up to kMaxGap unread blocks are read through), in
//    first-read order, one run in flight. Its I/O takes the SD driver's background class, so any queued demand read
//    goes first (NativeIOSwitchBlock.cpp); a demand read that needs a page of the run in flight waits for that run. The demand reads (the cryptex APFS -> DiskImages -> this file) then find
//    their pages cached. It stops when the list is done or kRecordAfterFrameS after the first frame.
// The image is read-only, so a stale list costs only wasted reads; a changed image (id/size/mtime) is re-recorded.
namespace {
constexpr uint32_t kBlock = 4096;
constexpr uint32_t kMaxRun = 128;  // 512 KiB per advisory read: two 256 KiB card commands
constexpr uint32_t kMaxGap = 8;    // read through up to 32 KiB of unrecorded blocks rather than start a new command
constexpr uint64_t kRecordAfterFrameS = 30;
// NativeLegacyDisplay reports the first frame only in the unrotated layout; without a report, stop this long after the
// attach (the 11-04 boots reached SpringBoard 110-135 s after it).
constexpr uint64_t kRecordBackstopS = 180;
constexpr char kListSuffix[] = ".boot-prefetch";
constexpr char kMagic[8] = {'I', 'O', 'S', 'W', 'P', 'F', '0', '1'};

struct ListHeader {
    char magic[8];
    uint32_t version, headerBytes;
    uint64_t imageBytes, imageFileId;
    int64_t imageMtimeSec, imageMtimeNsec;
    uint32_t blocks, recorded;
    uint64_t checksum;  // FNV-1a 64 of the map
};
struct Run {
    uint32_t start;
    uint16_t blocks;
    uint8_t when;
    uint8_t pad;
};

struct {
    ListHeader key;               // the image this boot attached
    uint8_t *recording = nullptr;  // the map being recorded (never freed: vfs_cluster.c may still write it)
    Run *runs = nullptr;
    uint32_t runCount = 0;
    thread_call_t frameCall = nullptr, loadCall = nullptr;
    volatile uint32_t frame = 0, stop = 0;
} gPrefetch;

uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

uint64_t uptimeMs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}

void listPath(char *out, size_t capacity)
{
    snprintf(out, capacity, "%s%s", paths[0], kListSuffix);
}

bool imageKey(vnode_t image, ListHeader *key)
{
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    VATTR_WANTED(&va, va_fileid);
    VATTR_WANTED(&va, va_modify_time);
    if (vnode_getattr(image, &va, vfs_context_current()) || !VATTR_IS_SUPPORTED(&va, va_data_size) ||
        !VATTR_IS_SUPPORTED(&va, va_fileid) || !VATTR_IS_SUPPORTED(&va, va_modify_time) || va.va_data_size < kBlock ||
        va.va_data_size / kBlock > UINT32_MAX)
        return false;
    bzero(key, sizeof(*key));
    memcpy(key->magic, kMagic, sizeof(kMagic));
    key->version = 1;
    key->headerBytes = sizeof(*key);
    key->imageBytes = va.va_data_size;
    key->imageFileId = va.va_fileid;
    key->imageMtimeSec = va.va_modify_time.tv_sec;
    key->imageMtimeNsec = va.va_modify_time.tv_nsec;
    key->blocks = static_cast<uint32_t>(va.va_data_size / kBlock);
    return true;
}

// The saved map for this image, or nullptr (none, another image, torn or unreadable: the caller records afresh).
uint8_t *loadList(const ListHeader &key)
{
    char path[128];
    listPath(path, sizeof(path));
    vfs_context_t ctx = vfs_context_current();
    vnode_t vp = NULLVP;
    if (vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx)) return nullptr;
    ListHeader header = {};
    int resid = 0;
    uint8_t *map = nullptr;
    if (!vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(&header), sizeof(header), 0, UIO_SYSSPACE, IO_NOAUTH,
                 vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) &&
        !resid && !memcmp(header.magic, key.magic, sizeof(key.magic)) && header.version == key.version &&
        header.headerBytes == sizeof(header) && header.imageBytes == key.imageBytes &&
        header.imageFileId == key.imageFileId && header.imageMtimeSec == key.imageMtimeSec &&
        header.imageMtimeNsec == key.imageMtimeNsec && header.blocks == key.blocks && header.recorded) {
        map = static_cast<uint8_t *>(IOMallocData(key.blocks));
        if (map && (vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(map), static_cast<int>(key.blocks), sizeof(header),
                            UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) ||
                    resid || fnv(map, key.blocks) != header.checksum)) {
            IOFreeData(map, key.blocks);
            map = nullptr;
        }
    }
    vnode_close(vp, FREAD, ctx);
    return map;
}

int compareRuns(const void *a, const void *b)
{
    const Run *x = static_cast<const Run *>(a), *y = static_cast<const Run *>(b);
    if (x->when != y->when) return x->when < y->when ? -1 : 1;
    return x->start < y->start ? -1 : x->start > y->start;
}

// Recorded blocks as runs of at most kMaxRun blocks (through gaps of up to kMaxGap), in first-read order.
uint32_t buildRuns(const uint8_t *map, uint32_t blocks, Run **out)
{
    uint32_t count = 0;
    for (int pass = 0; pass < 2; ++pass) {
        uint32_t n = 0, start = 0, end = 0;
        uint8_t when = 0;
        bool open = false;
        for (uint32_t b = 0; b <= blocks; ++b) {
            const bool read = b < blocks && map[b];
            if (read && open && b - end <= kMaxGap && b - start < kMaxRun) {
                end = b + 1;
                if (map[b] < when) when = map[b];
                continue;
            }
            if (open && (read || b == blocks || b - end > kMaxGap)) {
                if (pass) (*out)[n] = {start, static_cast<uint16_t>(end - start), when, 0};
                ++n;
                open = false;
            }
            if (read) {
                start = b;
                end = b + 1;
                when = map[b];
                open = true;
            }
        }
        if (!pass) {
            count = n;
            if (!count || !(*out = static_cast<Run *>(IOMallocData(count * sizeof(Run))))) return 0;
        }
    }
    qsort(*out, count, sizeof(Run), compareRuns);
    return count;
}

// Reads the runs into the image file's page cache, one at a time: advisory_read queues a run's I/O asynchronously, then
// a one-page read of the run's last page waits until it has arrived (or reads it itself if the prefetch failed there,
// so nothing can hang on it). One run in flight keeps the card fed while bounding how long a demand read can wait
// behind background-class prefetch I/O for a page it needs (DiskImages serves the image on a single thread).
void prefetchThread(void *, wait_result_t)
{
    ioswitch_block_prefetch_thread(current_thread());
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t image = NULLVP;
    uint8_t *page = static_cast<uint8_t *>(IOMallocData(kBlock));
    uint64_t bytes = 0, started = uptimeMs();
    uint32_t done = 0;
    if (page && !vnode_lookup(paths[0], 0, &image, ctx)) {
        const off_t size = static_cast<off_t>(gPrefetch.key.imageBytes);
        for (; done < gPrefetch.runCount && !gPrefetch.stop; ++done) {
            const Run &run = gPrefetch.runs[done];
            (void)advisory_read(image, size, static_cast<off_t>(run.start) * kBlock, run.blocks * kBlock);
            int resid = 0;
            (void)vn_rdwr(UIO_READ, image, reinterpret_cast<caddr_t>(page), kBlock,
                          static_cast<off_t>(run.start + run.blocks - 1) * kBlock, UIO_SYSSPACE, IO_NOAUTH,
                          vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
            bytes += run.blocks * kBlock;
        }
        vnode_put(image);
    }
    ioswitch_block_prefetch_thread(THREAD_NULL);
    const uint64_t now = uptimeMs();
    IOLog("IOSwitch boot prefetch: %llu MiB of the System cryptex in %u of %u runs, %llu.%llu s to %llu.%llu s uptime%s\n",
          bytes >> 20, done, gPrefetch.runCount, started / 1000, started / 100 % 10, now / 1000,
          now / 100 % 10, gPrefetch.stop ? " (stopped after the first frame)" : "");
    if (page) IOFreeData(page, kBlock);
    IOFreeData(gPrefetch.runs, gPrefetch.runCount * sizeof(Run));
    gPrefetch.runs = nullptr;
    thread_terminate(current_thread());
}

// kRecordAfterFrameS after the first frame (or kRecordBackstopS after the attach): stop the prefetch, save a recording.
void afterFrame(thread_call_param_t, thread_call_param_t)
{
    gPrefetch.stop = 1;
    uint8_t *map = __atomic_exchange_n(&gPrefetch.recording, nullptr, __ATOMIC_ACQ_REL);
    if (!map) return;
    ioswitch_cluster_record_stop();
    ListHeader header = gPrefetch.key;
    for (uint32_t b = 0; b < header.blocks; ++b) header.recorded += map[b] != 0;
    header.checksum = fnv(map, header.blocks);
    char path[128];
    listPath(path, sizeof(path));
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int error = vnode_open(path, O_CREAT | O_TRUNC | FWRITE | O_NOFOLLOW, 0644, 0, &vp, ctx);
    if (!error) {
        int resid = 0;
        error = vn_rdwr(UIO_WRITE, vp, reinterpret_cast<caddr_t>(&header), sizeof(header), 0, UIO_SYSSPACE,
                        IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (!error)
            error = vn_rdwr(UIO_WRITE, vp, reinterpret_cast<caddr_t>(map), static_cast<int>(header.blocks), sizeof(header),
                            UIO_SYSSPACE, IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        vnode_close(vp, FWRITE, ctx);
    }
    if (error)
        IOLog("IOSwitch boot prefetch: %s not saved: %d\n", path, error);
    else
        IOLog("IOSwitch boot prefetch: recorded %u blocks (%u MiB) of the System cryptex for the next boots\n",
              header.recorded, header.recorded >> 8);
}

// Temporary (r8-sdboot, 11-04): CPU time per core from boot to the first frame, to tell what bounds the boot once the
// SD card no longer does. One line, once.
void cpuLoad(thread_call_param_t, thread_call_param_t)
{
    char line[256];
    int at = snprintf(line, sizeof(line), "IOSwitch boot CPU to first frame (user/system/idle s per core):");
    for (int cpu = 0; cpu <= ml_get_max_cpu_number() && at > 0 && at < static_cast<int>(sizeof(line)); ++cpu) {
        processor_t processor = cpu_to_processor(cpu);
        if (!processor) continue;
        natural_t ticks[CPU_STATE_MAX] = {};
        processor_cpu_load_info(processor, ticks);
        at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), " %d: %u/%u/%u", cpu, ticks[CPU_STATE_USER] / 100,
                       ticks[CPU_STATE_SYSTEM] / 100, ticks[CPU_STATE_IDLE] / 100);
    }
    IOLog("%s\n", line);
}

// After the System image's attach: replay its list, or record one.
void prefetchStart(vnode_t image)
{
    if (!imageKey(image, &gPrefetch.key)) return;
    gPrefetch.loadCall = thread_call_allocate_with_options(cpuLoad, nullptr, THREAD_CALL_PRIORITY_LOW, 0);
    gPrefetch.frameCall = thread_call_allocate_with_options(afterFrame, nullptr, THREAD_CALL_PRIORITY_LOW, 0);
    if (!gPrefetch.frameCall) return;
    uint64_t backstop = 0;
    clock_interval_to_deadline(static_cast<uint32_t>(kRecordBackstopS), NSEC_PER_SEC, &backstop);
    thread_call_enter_delayed(gPrefetch.frameCall, backstop);
    if (uint8_t *map = loadList(gPrefetch.key)) {
        gPrefetch.runCount = buildRuns(map, gPrefetch.key.blocks, &gPrefetch.runs);
        IOFreeData(map, gPrefetch.key.blocks);
        thread_t thread = THREAD_NULL;
        if (gPrefetch.runCount && kernel_thread_start(prefetchThread, nullptr, &thread) == KERN_SUCCESS)
            thread_deallocate(thread);
        return;
    }
    if ((gPrefetch.recording = static_cast<uint8_t *>(IOMallocZeroData(gPrefetch.key.blocks))))
        ioswitch_cluster_record_start(image, gPrefetch.recording, gPrefetch.key.blocks);
}
}  // namespace

// NativeLegacyDisplay calls this (weakly) at each frame until the first one shows, in any context, interrupts possibly
// disabled: the first call moves afterFrame to kRecordAfterFrameS from now (re-arming replaces the backstop deadline).
extern "C" void ioswitch_boot_io_report(void)
{
    if (__atomic_exchange_n(&gPrefetch.frame, 1u, __ATOMIC_RELAXED) || !gPrefetch.frameCall) return;
    if (gPrefetch.loadCall) thread_call_enter(gPrefetch.loadCall);
    uint64_t deadline = 0;
    clock_interval_to_deadline(static_cast<uint32_t>(kRecordAfterFrameS), NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(gPrefetch.frameCall, deadline);
}

extern "C" int ioswitch_cryptex_attach(unsigned index, dev_t, dev_t *device, char *path, size_t capacity)
{
    int error = outputs(index, device, path, capacity);
    if (error) return error;
    if (attempted[index]) return results[index] ? results[index] : findVolume(uuids[index], device, path, capacity);
    attempted[index] = true;
    char bsd[64] = {};
    dev_t attached = NODEV;
    int status = di_root_image_ext(paths[index], bsd, sizeof(bsd), &attached, false);
    if (status) {
        IOLog("IOSwitch cryptex: attach %s failed: %d\n", paths[index], status);
        return results[index] = EIO;
    }
    // r8-sdboot: DiskImages' FileBackingStore marks the image vnode VNOCACHE_DATA when it opens it, so every read of the
    // image device became an uncached read of the same size on the SD card, with no read-ahead. The image is read-only
    // here, so its pages can stay in the page cache like any file's: clear the flag (the vnode is the one the backing
    // store holds, one vnode per file; its reads pass IO_PASSIVE only) and register the image with the cluster layer
    // (vfs_cluster.c: cached pages keep their reference). Then replay or record the System image's boot prefetch.
    vnode_t image = NULLVP;
    if (const int lookup = vnode_lookup(paths[index], 0, &image, vfs_context_current())) {
        IOLog("IOSwitch cryptex: %s stays uncached: lookup %d\n", paths[index], lookup);
    } else {
        vnode_clearnocache(image);
        ioswitch_cluster_register_image(image);
        if (index == 0) prefetchStart(image);
        vnode_put(image);
    }
    // The image's APFS container publishes its volume asynchronously.
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        error = findVolume(uuids[index], device, path, capacity);
        if (error != EAGAIN) break;
        IOSleep(50);
    }
    if (error)
        IOLog("IOSwitch cryptex: %s attached as %s but volume %s not found: %d\n", paths[index], bsd, uuids[index],
              error);
    return results[index] = error;
}

extern "C" int ioswitch_cryptex_volume_status(unsigned index, dev_t *device, char *path, size_t capacity)
{
    int error = outputs(index, device, path, capacity);
    if (error) return error;
    return results[index] ? results[index] : findVolume(uuids[index], device, path, capacity);
}

extern "C" int ioswitch_cryptex_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
    if (!digest) return 0;
    bzero(digest, 48);
    dev_t actual = NODEV;
    char path[64];
    if (device == NODEV || !uuid || strcmp(uuid, uuids[0]) || results[0] ||
        findVolume(uuids[0], &actual, path, sizeof(path)) || actual != device)
        return 0;
    memcpy(digest, systemIdentifier, sizeof(systemIdentifier));
    return 1;
}
