// SPDX-License-Identifier: BSD-2-Clause-Patent
// The unchanged System/App cryptex images from Preboot as read-only block devices: read straight from the SD card into
// the requesting pages (r17-launch, the direct read map), or through the original DiskImages hook if that map is
// refused; the APFS volume each one publishes; and the boot prefetch of the cryptex volumes' files.
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/ubc.h>
#include <sys/vnode.h>
#include "IOSwitchCryptex.h"
#include "IOSwitchCryptexMap.h"

struct ioswitch_record_entry {  // bsd/vfs/vfs_cluster.c
    vnode_t vp;
    uint64_t offset;
    uint32_t length;
    uint32_t vid;
    uint16_t index;
    uint16_t when;
    uint32_t reserved;
};

extern "C" {
int di_root_image_ext(const char *, char *, size_t, dev_t *, bool);
void ioswitch_cluster_register_image(vnode_t);                                               // bsd/vfs/vfs_cluster.c
void ioswitch_cluster_register_cryptex_mount(unsigned int index, mount_t);                   // bsd/vfs/vfs_cluster.c
void ioswitch_cluster_record_start(struct ioswitch_record_entry *log, uint32_t capacity);    // bsd/vfs/vfs_cluster.c
uint32_t ioswitch_cluster_record_stop(void);                                                 // bsd/vfs/vfs_cluster.c
void ioswitch_cluster_image_direct(uint64_t bytes, uint32_t cardReads);                      // bsd/vfs/vfs_cluster.c
int vnode_pagein(vnode_t, upl_t, upl_offset_t, vm_object_offset_t, upl_size_t, int, int *);  // bsd/vm/vnode_pager.c
int VFS_VGET(mount_t, ino64_t, vnode_t *, vfs_context_t);                                    // bsd/vfs/kpi_vfs.c
void ioswitch_block_prefetch_thread(thread_t);                                               // NativeIOSwitchBlock.cpp
IOBlockStorageDevice *ioswitch_block_device(IOService **controller, dev_t *whole, uint64_t *sectors,
                                            uint32_t *maxReadSectors);  // NativeIOSwitchBlock.cpp
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
IOService *directDevice[IOSWITCH_CRYPTEX_COUNT];  // the image's direct read map, when it is the one attached

uint64_t uptimeMs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}

bool below(IORegistryEntry *media, IORegistryEntry *ancestor)
{
    bool found = false;
    IORegistryEntry *entry = media->copyParentEntry(gIOServicePlane);
    for (unsigned depth = 0; entry && depth < 32 && !found; ++depth) {
        found = entry == ancestor;
        IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
        entry->release();
        entry = parent;
    }
    OSSafeReleaseNULL(entry);
    return found;
}

// The IOMedia carrying this volume UUID once IOMediaBSDClient has named it (below `ancestor` if one is given).
int findVolume(const char *uuid, IORegistryEntry *ancestor, dev_t *device, char *path, size_t capacity)
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
        if (ancestor && !below(media, ancestor)) continue;
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
}  // namespace

// ---- r17-launch: the direct read map ----
//
// The cryptex images used to be attached with DiskImages: each read of the image device (the cryptex APFS's) became a
// read of the image file on Preboot, through its page cache (r8-sdboot cleared VNOCACHE_DATA so the boot prefetch had a
// cache to fill), and was then copied page by page into the requesting buffer (bcopy_phys on DiskImages' kernel threads;
// with the per-read VNOP_READ, UPL and copy work those four threads used ~29 s of CPU per r7v boot, ~1.5 s of it the
// copy). The image is one unchanging file, so its extents are resolved once here instead: the file's blocks on Preboot
// (VNOP_BLOCKMAP) and where they sit on the SD card's APFS window (DKIOCGETPHYSICALEXTENT, as kern_symfile.c's
// direct-I/O files), checked by reading sampled blocks both ways. The cryptex device's reads then go straight to the SD
// device as sub-ranges of the request's own memory, one per extent piece, which the SD driver's ADMA2 table scatters
// into the requesting pages: no image page cache, no copy, no DiskImages thread.
//
// The device reports itself as SD storage, not a virtual (disk image) interface, so the cryptex APFS mounts without
// MNTK_VIRTUALDEV: its page-ins are then clustered reads checked by the cluster layer (cluster_EOT asks APFS for the
// seal's hashes when the I/O is issued, cluster_iodone checks every page against them: VNOP_VERIFY), as on every
// non-virtual volume and as grafted cryptexes are read on a stock iPad, instead of APFS's own one-4-KiB-block-at-a-time
// check for virtual devices (245,891 of 245,941 reads in the 11-04 census). The same hashes, root hash and trust cache
// apply to the same bytes. Completions go to the client from a thread call, never from the SD driver's worker, so the
// hash checks run beside the card's next command, and a completion that ever waited for another read of the same
// image could not block the thread that issues it.
//
// The image file stays open (its use count) while the device exists. A refused map (a hole, a compressed file, an
// extent outside the window, a sampled block that differs) logs one line and attaches through DiskImages as before.
namespace {
constexpr uint32_t kImageBlock = 4096;  // the cryptex device's block (APFS's), and the images' size multiple
constexpr uint32_t kSdSector = 512;
constexpr uint32_t kMaxExtents = 1u << 16;
constexpr unsigned kSampledExtents = 16;  // checked both ways at attach, besides the image's first and last block

using Extent = IOSwitchExtent;  // bytes: image offset, offset on the SD APFS window, length

struct Pending {
    Pending *next;
    thread_call_t call;
    IOStorageCompletion client;
    uint64_t length, bytes;
    uint32_t remaining;  // pieces outstanding, plus one while the request is being issued
    IOReturn status;
    bool prefetch;
};

IOLock *gPoolLock;
Pending *gPool;
const uint8_t gZeros[kImageBlock] = {};

// The boot prefetch thread's reads in flight on the direct devices (bytes), and the foreground's while it runs.
struct {
    IOLock *lock;
    thread_t thread;
    uint64_t inflight;
    bool waiting;
    uint64_t foregroundReads, foregroundBytes;
} gFlow;

void finishRequest(thread_call_param_t, thread_call_param_t);

Pending *takePending()
{
    IOLockLock(gPoolLock);
    Pending *p = gPool;
    if (p) gPool = p->next;
    IOLockUnlock(gPoolLock);
    if (p) return p;
    p = static_cast<Pending *>(IOMallocZero(sizeof(Pending)));
    if (!p) return nullptr;
    p->call = thread_call_allocate_with_options(finishRequest, p, THREAD_CALL_PRIORITY_KERNEL_HIGH, 0);
    if (!p->call) {
        IOFree(p, sizeof(Pending));
        return nullptr;
    }
    return p;
}

void returnPending(Pending *p)
{
    IOLockLock(gPoolLock);
    p->next = gPool;
    gPool = p;
    IOLockUnlock(gPoolLock);
}

// A thread call: the request's last piece has completed.
void finishRequest(thread_call_param_t parameter, thread_call_param_t)
{
    auto *p = static_cast<Pending *>(parameter);
    const IOStorageCompletion client = p->client;
    const IOReturn status = p->status;
    const uint64_t bytes = status == kIOReturnSuccess ? p->length : p->bytes;
    if (p->prefetch) {
        IOLockLock(gFlow.lock);
        gFlow.inflight -= p->length;
        const bool wake = gFlow.waiting;
        gFlow.waiting = false;
        IOLockUnlock(gFlow.lock);
        if (wake) IOLockWakeup(gFlow.lock, &gFlow.inflight, false);
    }
    returnPending(p);
    // IOStorage::complete, without binding another original IOStorageFamily symbol (as NativeIOSwitchBlock).
    if (client.action) client.action(client.target, client.parameter, status, bytes);
}

// The request's first error is its status.
void failPending(Pending *p, IOReturn status)
{
    IOReturn expected = kIOReturnSuccess;
    (void)__atomic_compare_exchange_n(&p->status, &expected, status, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

// One piece's completion, on the SD driver's worker.
void pieceDone(void *, void *parameter, IOReturn status, UInt64 actualByteCount)
{
    auto *p = static_cast<Pending *>(parameter);
    if (status != kIOReturnSuccess) failPending(p, status);
    __atomic_fetch_add(&p->bytes, actualByteCount, __ATOMIC_RELAXED);
    if (__atomic_sub_fetch(&p->remaining, 1, __ATOMIC_ACQ_REL) == 0) thread_call_enter(p->call);
}
}  // namespace

class IOSwitchCryptexDevice : public IOBlockStorageDevice {
OSDeclareDefaultStructors(IOSwitchCryptexDevice)

    public : bool configure(unsigned image, Extent *map, uint32_t count, uint64_t bytes, IOBlockStorageDevice *sd,
                            uint64_t sdSectors, uint32_t sdMaxSectors);
    IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
    IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
    UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
    IOReturn doUnmap(IOBlockStorageDeviceExtent *, UInt32, IOStorageUnmapOptions) override
    {
        return kIOReturnUnsupported;
    }
    char *getVendorString() override { return const_cast<char *>("IOSwitch"); }
    char *getProductString() override { return const_cast<char *>(index ? "App cryptex" : "System cryptex"); }
    char *getRevisionString() override { return const_cast<char *>("1.0"); }
    char *getAdditionalDeviceInfoString() override { return const_cast<char *>("cryptex image extents on SDMMC1"); }
    IOReturn reportBlockSize(UInt64 *value) override
    {
        *value = kImageBlock;
        return kIOReturnSuccess;
    }
    IOReturn reportMaxValidBlock(UInt64 *value) override
    {
        *value = blocks - 1;
        return kIOReturnSuccess;
    }
    IOReturn reportEjectability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }
    IOReturn reportRemovability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }
    IOReturn reportWriteProtection(bool *value) override
    {
        *value = true;
        return kIOReturnSuccess;
    }
    IOReturn reportMediaState(bool *present, bool *changed) override
    {
        *present = true;
        if (changed) *changed = false;
        return kIOReturnSuccess;
    }
    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
    IOReturn getWriteCacheState(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }
    IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *,
                              IOStorageCompletion *) override;

private:
    unsigned index = 0;
    Extent *extents = nullptr;
    uint32_t extentCount = 0;
    uint64_t blocks = 0;
    IOBlockStorageDevice *sd = nullptr;
    uint64_t maxPiece = 0;  // bytes per SD read
    bool holes = false;     // some of the image reads as zeros (a sparse file)
};

OSDefineMetaClassAndStructors(IOSwitchCryptexDevice, IOBlockStorageDevice)

    bool IOSwitchCryptexDevice::configure(unsigned image, Extent *map, uint32_t count, uint64_t bytes,
                                          IOBlockStorageDevice *sdDevice, uint64_t, uint32_t sdMaxSectors)
{
    index = image;
    extents = map;
    extentCount = count;
    blocks = bytes / kImageBlock;
    sd = sdDevice;
    maxPiece = uint64_t(sdMaxSectors) * kSdSector;
    for (uint32_t i = 0; i < count; ++i) holes |= map[i].disk == kIOSwitchHole;
    // A request larger than one SD read is split here anyway; IOBlockStorageDriver keeps them at 1 MiB.
    setProperty(kIOMaximumBlockCountReadKey, (1024 * 1024) / kImageBlock, 64);
    setProperty(kIOMaximumByteCountReadKey, 1024 * 1024, 64);
    if (OSDictionary *protocol = OSDictionary::withCapacity(2)) {
        protocol->setObject(kIOPropertyPhysicalInterconnectTypeKey, OSString::withCStringNoCopy("SD"));
        protocol->setObject(kIOPropertyPhysicalInterconnectLocationKey,
                            OSString::withCStringNoCopy(kIOPropertyInternalKey));
        setProperty(kIOPropertyProtocolCharacteristicsKey, protocol);
        protocol->release();
    }
    if (OSDictionary *device = OSDictionary::withCapacity(1)) {
        device->setObject(kIOPropertyMediumTypeKey, OSString::withCStringNoCopy(kIOPropertyMediumTypeSolidStateKey));
        setProperty(kIOPropertyDeviceCharacteristicsKey, device);
        device->release();
    }
    setProperty("IOSwitch image", paths[image]);
    setProperty("IOSwitch image extents", count, 32);
    setName(image ? "IOSwitchAppCryptex" : "IOSwitchSystemCryptex");
    return maxPiece >= kImageBlock;
}

IOReturn IOSwitchCryptexDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                                 IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !count || block >= blocks || count > blocks - block) return kIOReturnBadArgument;
    if (buffer->getDirection() != kIODirectionIn) return kIOReturnNotWritable;
    const uint64_t length = count * kImageBlock, start = block * kImageBlock;
    if (buffer->getLength() < length) return kIOReturnBadArgument;
    Pending *p = takePending();
    if (!p) return kIOReturnNoMemory;
    p->client = *completion;
    p->length = length;
    p->bytes = 0;
    p->status = kIOReturnSuccess;
    p->remaining = 1;
    p->prefetch = gFlow.thread != THREAD_NULL && current_thread() == gFlow.thread;
    if (p->prefetch) {
        IOLockLock(gFlow.lock);
        gFlow.inflight += length;
        IOLockUnlock(gFlow.lock);
    } else if (gFlow.thread != THREAD_NULL) {
        __atomic_fetch_add(&gFlow.foregroundReads, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&gFlow.foregroundBytes, length, __ATOMIC_RELAXED);
    }
    uint32_t pieces = 0, at = ioswitchExtentAt(extents, extentCount, start);
    uint64_t done = 0;
    if (holes) {
        // A range the image file does not store reads as zeros: written before any card read is issued into the same
        // request, so the SD driver's cache maintenance around its reads never meets a dirty line of ours.
        for (uint32_t z = at; done < length;) {
            const IOSwitchPiece piece =
                ioswitchNextPiece(extents, extentCount, &z, start + done, length - done, maxPiece);
            if (!piece.length) break;
            for (uint64_t k = 0; piece.disk == kIOSwitchHole && k < piece.length; k += kImageBlock)
                if (buffer->writeBytes(done + k, gZeros, kImageBlock) != kImageBlock) failPending(p, kIOReturnIOError);
            done += piece.length;
        }
        done = 0;
    }
    while (done < length) {
        const IOSwitchPiece piece = ioswitchNextPiece(extents, extentCount, &at, start + done, length - done, maxPiece);
        if (!piece.length) break;
        if (piece.disk == kIOSwitchHole) {
            done += piece.length;
            continue;
        }
        IOSubMemoryDescriptor *sub = IOSubMemoryDescriptor::withSubRange(buffer, done, piece.length, kIODirectionIn);
        if (!sub) break;
        __atomic_fetch_add(&p->remaining, 1, __ATOMIC_RELAXED);
        IOStorageCompletion pieceCompletion = {this, pieceDone, p};
        const IOReturn issued =
            sd->doAsyncReadWrite(sub, piece.disk / kSdSector, piece.length / kSdSector, attributes, &pieceCompletion);
        sub->release();
        if (issued != kIOReturnSuccess) pieceDone(this, p, issued, 0);
        ++pieces;
        done += piece.length;
    }
    if (done < length) failPending(p, kIOReturnNoMemory);  // the rest was never issued
    ioswitch_cluster_image_direct(length, pieces);
    pieceDone(this, p, kIOReturnSuccess, 0);  // the issuing reference
    return kIOReturnSuccess;
}

namespace {
// One synchronous read for the attach-time check: `bytes` at `sector` of the SD window into `buffer`.
struct SyncRead {
    IOLock *lock;
    bool done;
    IOReturn status;
};

void syncReadDone(void *, void *parameter, IOReturn status, UInt64)
{
    auto *wait = static_cast<SyncRead *>(parameter);
    IOLockLock(wait->lock);
    wait->status = status;
    wait->done = true;
    IOLockUnlock(wait->lock);
    IOLockWakeup(wait->lock, wait, false);
}

IOReturn readSd(IOBlockStorageDevice *sd, uint64_t sector, IOBufferMemoryDescriptor *buffer)
{
    SyncRead wait = {IOLockAlloc(), false, kIOReturnSuccess};
    if (!wait.lock) return kIOReturnNoMemory;
    IOStorageCompletion completion = {nullptr, syncReadDone, &wait};
    IOReturn status = sd->doAsyncReadWrite(buffer, sector, buffer->getLength() / kSdSector, nullptr, &completion);
    if (status == kIOReturnSuccess) {
        IOLockLock(wait.lock);
        while (!wait.done) IOLockSleep(wait.lock, &wait, THREAD_UNINT);
        IOLockUnlock(wait.lock);
        status = wait.status;
    }
    IOLockFree(wait.lock);
    return status;
}

// Whether block `offset` of the image reads the same through the file and through the map.
bool sameBothWays(vnode_t image, const Extent &e, uint64_t offset, IOBlockStorageDevice *sd,
                  IOBufferMemoryDescriptor *direct, uint8_t *file)
{
    vfs_context_t ctx = vfs_context_current();
    int resid = 0;
    if (vn_rdwr(UIO_READ, image, reinterpret_cast<caddr_t>(file), kImageBlock, static_cast<off_t>(offset), UIO_SYSSPACE,
                IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) ||
        resid)
        return false;
    if (e.disk == kIOSwitchHole) return !memcmp(gZeros, file, kImageBlock);
    if (readSd(sd, (e.disk + (offset - e.file)) / kSdSector, direct) != kIOReturnSuccess) return false;
    return !memcmp(direct->getBytesNoCopy(), file, kImageBlock);
}

// The image's extents on the SD APFS window, or nullptr with *why set. `image` is open; `preboot` its volume's device.
Extent *resolveExtents(vnode_t image, uint64_t bytes, dev_t preboot, dev_t whole, uint64_t windowBytes, uint32_t *count,
                       const char **why)
{
    vfs_context_t ctx = vfs_context_current();
    vnode_t devvp = vfs_devvp(vnode_mount(image));
    if (!devvp) return *why = "no Preboot device vnode", nullptr;
    Extent *map = nullptr;
    uint32_t n = 0, blockSize = 0;
    bool physical = true, asked = false, locked = false;
    if (vnode_specrdev(devvp) != preboot) {
        *why = "image not on the Preboot volume";
    } else if (VNOP_IOCTL(devvp, DKIOCGETBLOCKSIZE, reinterpret_cast<caddr_t>(&blockSize), 0, ctx) || !blockSize) {
        *why = "no Preboot block size";
    } else if (!(map = static_cast<Extent *>(IOMallocData(kMaxExtents * sizeof(Extent))))) {
        *why = "no memory";
    } else {
        locked = !VNOP_IOCTL(devvp, DKIOCLOCKPHYSICALEXTENTS, nullptr, 0, ctx);
        for (uint64_t at = 0; at < bytes && !*why;) {
            daddr64_t blkno = -1;
            size_t run = 0;
            const size_t ask = bytes - at > (1ull << 30) ? (1ull << 30) : size_t(bytes - at);
            if (VNOP_BLOCKMAP(image, static_cast<off_t>(at), ask, &blkno, &run, nullptr,
                              VNODE_READ | VNODE_BLOCKMAP_NO_TRACK, nullptr) ||
                !run) {
                *why = "VNOP_BLOCKMAP failed";
                break;
            }
            if (run > bytes - at) run = size_t(bytes - at);
            if (blkno < 0) {  // a hole: zeros, nothing to read
                if (!ioswitchAppendExtent(map, &n, kMaxExtents, at, kIOSwitchHole, run)) *why = "too many extents";
                at += run;
                continue;
            }
            const uint64_t logical = uint64_t(blkno) * blockSize;
            for (uint64_t part = 0; part < run && !*why;) {
                uint64_t disk = logical + part, length = run - part;
                if (physical) {
                    dk_physical_extent_t request = {};
                    request.offset = disk;
                    request.length = length;
                    const int error =
                        VNOP_IOCTL(devvp, DKIOCGETPHYSICALEXTENT, reinterpret_cast<caddr_t>(&request), 0, ctx);
                    const bool first = !asked;
                    asked = true;
                    if (error == ENOTTY && first) {
                        // No physical-extent answer: the container is the whole window (one physical store at offset 0),
                        // so its block addresses are the window's; the sampled reads below check that.
                        physical = false;
                    } else if (error || !request.length || request.dev != whole) {
                        *why = error ? "DKIOCGETPHYSICALEXTENT failed" : "an extent is not on the SD APFS window";
                        break;
                    } else {
                        disk = request.offset;
                        if (request.length < length) length = request.length;
                    }
                }
                if (disk % kSdSector || length % kSdSector || disk > windowBytes || length > windowBytes - disk ||
                    length % kImageBlock || (at + part) % kImageBlock) {
                    *why = "an extent outside the SD APFS window";
                    break;
                }
                if (!ioswitchAppendExtent(map, &n, kMaxExtents, at + part, disk, length)) {
                    *why = "too many extents";
                    break;
                }
                part += length;
            }
            at += run;
        }
        if (locked) (void)VNOP_IOCTL(devvp, DKIOCUNLOCKPHYSICALEXTENTS, nullptr, 0, ctx);
    }
    vnode_put(devvp);
    if (!*why && !ioswitchExtentsCover(map, n, bytes)) *why = "extents do not cover the image";
    if (*why) {
        if (map) IOFreeData(map, kMaxExtents * sizeof(Extent));
        return nullptr;
    }
    *count = n;
    return map;
}

// The direct read map for image `index`: its device registered on the SD controller, or *why.
IOService *attachDirect(unsigned index, dev_t preboot, const char **why)
{
    vfs_context_t ctx = vfs_context_current();
    IOService *controller = nullptr;
    dev_t whole = NODEV;
    uint64_t windowSectors = 0;
    uint32_t maxSectors = 0;
    IOBlockStorageDevice *sd = ioswitch_block_device(&controller, &whole, &windowSectors, &maxSectors);
    if (!sd || whole == NODEV || maxSectors * kSdSector < kImageBlock) return *why = "no SD device", nullptr;
    vnode_t image = NULLVP;
    if (vnode_open(paths[index], FREAD | O_NOFOLLOW, 0, 0, &image, ctx)) return *why = "image open failed", nullptr;
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    VATTR_WANTED(&va, va_flags);
    uint32_t count = 0;
    Extent *map = nullptr;
    IOBufferMemoryDescriptor *direct = nullptr;
    uint8_t *file = nullptr;
    IOSwitchCryptexDevice *device = nullptr;
    if (!vnode_isreg(image) || vnode_getattr(image, &va, ctx) || !VATTR_IS_SUPPORTED(&va, va_data_size)) {
        *why = "image attributes";
    } else if (VATTR_IS_SUPPORTED(&va, va_flags) && (va.va_flags & UF_COMPRESSED)) {
        *why = "the image file is compressed";
    } else if (!va.va_data_size || va.va_data_size % kImageBlock) {
        *why = "image size not a multiple of 4 KiB";
    } else if ((map = resolveExtents(image, va.va_data_size, preboot, whole, windowSectors * kSdSector, &count, why))) {
        direct = IOBufferMemoryDescriptor::withOptions(kIODirectionIn, kImageBlock, kImageBlock);
        file = static_cast<uint8_t *>(IOMallocData(kImageBlock));
        if (!direct || !file || direct->prepare() != kIOReturnSuccess) {
            *why = "no memory";
        } else {
            // The first and last block, and the first block of extents spread over the image.
            const Extent &last = map[count - 1];  // (a hole is compared with zeros)
            bool same = sameBothWays(image, map[0], 0, sd, direct, file) &&
                        sameBothWays(image, last, va.va_data_size - kImageBlock, sd, direct, file);
            for (unsigned k = 1; same && k <= kSampledExtents && count > 2; ++k) {
                const Extent &e = map[uint64_t(count - 1) * k / (kSampledExtents + 1)];
                same = sameBothWays(image, e, e.file, sd, direct, file);
            }
            direct->complete();
            if (!same) *why = "a sampled block differs between the file and the map";
        }
    }
    OSSafeReleaseNULL(direct);
    if (file) IOFreeData(file, kImageBlock);
    if (!*why) {
        device = OSTypeAlloc(IOSwitchCryptexDevice);
        if (!device || !device->init(static_cast<OSDictionary *>(nullptr)) ||
            !device->configure(index, map, count, va.va_data_size, sd, windowSectors, maxSectors) ||
            !device->attach(controller)) {
            *why = "device setup";
        } else if (!device->start(controller)) {
            device->detach(controller);
            *why = "device start";
        }
    }
    if (*why) {
        OSSafeReleaseNULL(device);
        if (map) IOFreeData(map, kMaxExtents * sizeof(Extent));
        vnode_close(image, FREAD, ctx);
        return nullptr;
    }
    IOLog("IOSwitch cryptex: %s read straight from the card: %u extents, %llu MiB\n", paths[index], count,
          va.va_data_size >> 20);
    vnode_put(image);  // the open's use count keeps the file (and its extents) for the device's life
    device->registerService();
    return device;
}

// The original DiskImages hook (the map refused), as before r17-launch: the image's pages cached in Preboot's page
// cache (DiskImages opens it VNOCACHE_DATA; the image is read-only, so the flag is cleared and the cluster layer told).
int attachDiskImages(unsigned index)
{
    char bsd[64] = {};
    dev_t attached = NODEV;
    const int status = di_root_image_ext(paths[index], bsd, sizeof(bsd), &attached, false);
    if (status) {
        IOLog("IOSwitch cryptex: attach %s failed: %d\n", paths[index], status);
        return EIO;
    }
    vnode_t image = NULLVP;
    if (const int lookup = vnode_lookup(paths[index], 0, &image, vfs_context_current())) {
        IOLog("IOSwitch cryptex: %s stays uncached: lookup %d\n", paths[index], lookup);
    } else {
        vnode_clearnocache(image);
        ioswitch_cluster_register_image(image);
        vnode_put(image);
    }
    return 0;
}

// The image's APFS container publishes its volume asynchronously.
int waitVolume(unsigned index, dev_t *device, char *path, size_t capacity)
{
    int error = EAGAIN;
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        error = findVolume(uuids[index], directDevice[index], device, path, capacity);
        if (error != EAGAIN) break;
        IOSleep(50);
    }
    if (error) IOLog("IOSwitch cryptex: %s attached but volume %s not found: %d\n", paths[index], uuids[index], error);
    return error;
}
}  // namespace

extern "C" int ioswitch_cryptex_attach(unsigned index, dev_t preboot, dev_t *device, char *path, size_t capacity)
{
    int error = outputs(index, device, path, capacity);
    if (error) return error;
    if (attempted[index])
        return results[index] ? results[index] : findVolume(uuids[index], directDevice[index], device, path, capacity);
    attempted[index] = true;
    if (!gPoolLock) gPoolLock = IOLockAlloc();
    if (!gFlow.lock) gFlow.lock = IOLockAlloc();
    const char *why = gPoolLock && gFlow.lock ? nullptr : "no memory";
    if (!why) directDevice[index] = attachDirect(index, preboot, &why);
    if (!directDevice[index]) {
        IOLog("IOSwitch cryptex: %s direct read map refused (%s); attaching it through DiskImages\n", paths[index],
              why);
        if ((error = attachDiskImages(index))) return results[index] = error;
    }
    return results[index] = waitVolume(index, device, path, capacity);
}

// The mount of a direct-attached image's volume failed: detach the map and attach the image through DiskImages.
extern "C" int ioswitch_cryptex_fallback(unsigned index, dev_t *device, char *path, size_t capacity)
{
    int error = outputs(index, device, path, capacity);
    if (error) return error;
    IOService *direct = directDevice[index];
    if (!direct) return EINVAL;  // already DiskImages: nothing else to try
    IOLog("IOSwitch cryptex: %s on its direct read map did not mount; attaching it through DiskImages\n", paths[index]);
    directDevice[index] = nullptr;
    direct->terminate(kIOServiceRequired | kIOServiceSynchronous);
    // The map's volume is gone once its container has terminated (the image file stays open: the map is never freed).
    for (unsigned attempt = 0; attempt < 100 && !findVolume(uuids[index], direct, device, path, capacity); ++attempt)
        IOSleep(50);
    if ((error = attachDiskImages(index))) return results[index] = error;
    return results[index] = waitVolume(index, device, path, capacity);
}

extern "C" int ioswitch_cryptex_volume_status(unsigned index, dev_t *device, char *path, size_t capacity)
{
    int error = outputs(index, device, path, capacity);
    if (error) return error;
    return results[index] ? results[index] : findVolume(uuids[index], directDevice[index], device, path, capacity);
}

extern "C" int ioswitch_cryptex_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
    if (!digest) return 0;
    bzero(digest, 48);
    dev_t actual = NODEV;
    char path[64];
    if (device == NODEV || !uuid || strcmp(uuid, uuids[0]) || results[0] ||
        findVolume(uuids[0], directDevice[0], &actual, path, sizeof(path)) || actual != device)
        return 0;
    memcpy(digest, systemIdentifier, sizeof(systemIdentifier));
    return 1;
}

// ---- r8-sdboot, r17-launch: boot prefetch of the cryptex volumes' files ----
//
// Until the UI is up the SD card is the boot's bottleneck, and most of what it reads is the dyld shared cache on the
// System cryptex, paged in where processes first touch it. The same bytes read as large runs take a fraction of that
// card time. So:
//  - record: on a boot with no valid list, every read of a cryptex volume's file that goes to the device (vfs_cluster.c
//    cluster_io: page-in clusters, read misses, read-ahead) is logged with its first-read second, from the mounts until
//    afterFrame (the later of kRecordAfterFrameS after the first frame and kRecordUntilS of uptime), and this file saves
//    the files' ranges (file id, pages) as runs of up to kMaxRun pages (through gaps of up to kMaxGap pages), in
//    first-read order, next to the System image (kListSuffix; the header keys both images' id, size and mtime);
//  - replay: on every later boot, right after both cryptexes are mounted, a kernel thread pages the runs in, in order
//    (vnode_pagein on the file: absent pages only, into the file's own pages, read and checked like any page-in), with
//    at most kInFlightBytes of its reads in flight on the direct devices. Its reads share the SD driver's high class first
//    come first served with the foreground's (NativeIOSwitchBlock.cpp), so it keeps a large share of the card and stays
//    ahead of the foreground, whose page-ins then find their pages resident (or in flight, and wait for them). It stops
//    when the list is done or when a recording would stop (afterFrame).
// The volumes are read-only, so a stale list costs only wasted reads; a changed image is re-recorded. A list this build
// cannot read is a recording boot with no prefetch, so loadList says why in one line, and the format is frozen
// (IOSwitchCryptexMap.h): a new one must read the old.
namespace {
constexpr uint32_t kPage = 4096;
constexpr uint32_t kMaxRun = 128;  // pages: 512 KiB per page-in, as the old 512 KiB advisory reads
constexpr uint32_t kMaxGap = 8;    // read through up to 32 KiB of unrecorded pages rather than start another page-in
constexpr uint64_t kInFlightBytes = 3 * kMaxRun * kPage;
constexpr uint64_t kRecordAfterFrameS = 30;
constexpr uint64_t kRecordUntilS = 125;
constexpr uint32_t kLogEntries = 1u << 16;  // 2 MiB, allocated on recording boots only
// NativeLegacyDisplay reports the first frame only in the unrotated layout; without a report, stop this long after the
// mounts (the 11-04 boots reached SpringBoard 110-135 s after them).
constexpr uint64_t kRecordBackstopS = 180;
constexpr char kListSuffix[] = ".boot-prefetch";

using ListHeader = IOSwitchListHeader;
using Run = IOSwitchRun;
static_assert(kIOSwitchListImages == IOSWITCH_CRYPTEX_COUNT, "the list keys both cryptex images");

struct {
    ListHeader key;  // this boot's images
    mount_t mounts[IOSWITCH_CRYPTEX_COUNT];
    ioswitch_record_entry *log = nullptr;  // never freed: vfs_cluster.c may still be writing an entry
    Run *runs = nullptr;
    uint32_t runCount = 0;
    uint64_t runCapacity = 0;  // the recording's runs allocation (afterFrame)
    thread_call_t frameCall = nullptr, loadCall = nullptr;
    volatile uint32_t frame = 0, stop = 0, started = 0;
} gPrefetch;

void listPath(char *out, size_t capacity)
{
    snprintf(out, capacity, "%s%s", paths[0], kListSuffix);
}

bool imageKeys(ListHeader *key)
{
    bzero(key, sizeof(*key));
    memcpy(key->magic, kIOSwitchListMagic, sizeof(kIOSwitchListMagic));
    key->version = kIOSwitchListVersion;
    key->headerBytes = sizeof(*key);
    for (unsigned i = 0; i < IOSWITCH_CRYPTEX_COUNT; ++i) {
        vnode_t image = NULLVP;
        if (vnode_lookup(paths[i], 0, &image, vfs_context_current())) return false;
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        VATTR_WANTED(&va, va_fileid);
        VATTR_WANTED(&va, va_modify_time);
        const bool ok = !vnode_getattr(image, &va, vfs_context_current()) && VATTR_IS_SUPPORTED(&va, va_data_size) &&
                        VATTR_IS_SUPPORTED(&va, va_fileid) && VATTR_IS_SUPPORTED(&va, va_modify_time);
        vnode_put(image);
        if (!ok) return false;
        key->images[i] = {va.va_data_size, va.va_fileid, va.va_modify_time.tv_sec, va.va_modify_time.tv_nsec};
    }
    return true;
}

// The saved runs for these images, or nullptr with *why (none, another format, other images, torn or unreadable: the
// caller records afresh).
Run *loadList(const ListHeader &key, uint32_t *count, const char **why)
{
    char path[128];
    listPath(path, sizeof(path));
    vfs_context_t ctx = vfs_context_current();
    vnode_t vp = NULLVP;
    if (vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx)) return *why = "none saved", nullptr;
    ListHeader header = {};
    int resid = 0;
    Run *runs = nullptr;
    if (vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(&header), sizeof(header), 0, UIO_SYSSPACE, IO_NOAUTH,
                vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) ||
        resid) {
        *why = "its header is unreadable";
    } else if (!(*why = ioswitchListRefusal(header, key.images, kLogEntries * 4u))) {
        const size_t bytes = header.runs * sizeof(Run);
        runs = static_cast<Run *>(IOMallocData(bytes));
        if (!runs) {
            *why = "no memory";
        } else if (vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(runs), static_cast<int>(bytes), sizeof(header),
                           UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) ||
                   resid || ioswitchFnv(reinterpret_cast<const uint8_t *>(runs), bytes) != header.checksum) {
            IOFreeData(runs, bytes);
            runs = nullptr;
            *why = "torn (its runs do not match its checksum)";
        } else {
            *count = header.runs;
        }
    }
    vnode_close(vp, FREAD, ctx);
    return runs;
}

// Pages the runs in, in order, with at most kInFlightBytes of the thread's reads in flight on the direct devices
// (finishRequest wakes it; on DiskImages nothing is counted and each page-in simply returns once queued).
void prefetchThread(void *, wait_result_t)
{
    ioswitch_block_prefetch_thread(current_thread());
    IOLockLock(gFlow.lock);
    gFlow.foregroundReads = gFlow.foregroundBytes = 0;
    gFlow.thread = current_thread();
    IOLockUnlock(gFlow.lock);
    vfs_context_t ctx = vfs_context_kernel();
    const uint64_t started = uptimeMs();
    uint64_t bytes = 0;
    uint32_t done = 0, files = 0, missing = 0;
    vnode_t vp = NULLVP;
    uint64_t vpFile = 0;
    unsigned vpIndex = IOSWITCH_CRYPTEX_COUNT;
    off_t vpSize = 0;
    for (; done < gPrefetch.runCount && !gPrefetch.stop; ++done) {
        const Run &run = gPrefetch.runs[done];
        IOLockLock(gFlow.lock);
        while (gFlow.inflight >= kInFlightBytes) {
            gFlow.waiting = true;
            IOLockSleep(gFlow.lock, &gFlow.inflight, THREAD_UNINT);
        }
        IOLockUnlock(gFlow.lock);
        if (run.index >= IOSWITCH_CRYPTEX_COUNT || !gPrefetch.mounts[run.index]) continue;
        if (run.fileId != vpFile || run.index != vpIndex) {
            if (vp) vnode_put(vp);
            vp = NULLVP;
            vpFile = run.fileId;
            vpIndex = run.index;
            if (VFS_VGET(gPrefetch.mounts[run.index], static_cast<ino64_t>(run.fileId), &vp, ctx) || !vnode_isreg(vp)) {
                if (vp) vnode_put(vp);
                vp = NULLVP;
                ++missing;
                continue;
            }
            vpSize = static_cast<off_t>(ubc_getsize(vp));
            ++files;
        }
        if (!vp) continue;
        const off_t offset = off_t(run.page) * kPage;
        if (offset >= vpSize) continue;
        off_t length = off_t(run.pages) * kPage;
        if (length > vpSize - offset) length = (vpSize - offset + kPage - 1) & ~off_t(kPage - 1);
        // On the direct maps the page-in only queues its reads (the in-flight bound above paces it); through
        // DiskImages, which this thread's reads are not counted on, each waits for its own.
        int error = 0;
        (void)vnode_pagein(vp, nullptr, 0, static_cast<vm_object_offset_t>(offset), static_cast<upl_size_t>(length),
                           directDevice[run.index] ? 0 : UPL_IOSYNC, &error);
        bytes += uint64_t(length);
    }
    if (vp) vnode_put(vp);
    // The summary's end: the last page-ins have arrived.
    IOLockLock(gFlow.lock);
    while (gFlow.inflight) {
        gFlow.waiting = true;
        IOLockSleep(gFlow.lock, &gFlow.inflight, THREAD_UNINT);
    }
    gFlow.thread = THREAD_NULL;
    IOLockUnlock(gFlow.lock);
    ioswitch_block_prefetch_thread(THREAD_NULL);
    const uint64_t now = uptimeMs();
    IOLog("IOSwitch boot prefetch: %llu MiB of the cryptexes' files (%u lookups) in %u of %u runs, %llu.%llu s to "
          "%llu.%llu s "
          "uptime%s; meanwhile the foreground read the direct maps %llu times (%llu MiB)%s\n",
          bytes >> 20, files, done, gPrefetch.runCount, started / 1000, started / 100 % 10, now / 1000, now / 100 % 10,
          gPrefetch.stop ? " (stopped)" : "", gFlow.foregroundReads, gFlow.foregroundBytes >> 20,
          missing ? ", some files gone" : "");
    IOFreeData(gPrefetch.runs, gPrefetch.runCount * sizeof(Run));
    gPrefetch.runs = nullptr;
    thread_terminate(current_thread());
}

using Range = IOSwitchRange;  // a logged read's file, pages [first, end) and first-read second

int compareEntries(const void *a, const void *b)
{
    auto *x = static_cast<const ioswitch_record_entry *>(a), *y = static_cast<const ioswitch_record_entry *>(b);
    if (x->vp != y->vp) return x->vp < y->vp ? -1 : 1;
    return x->vid < y->vid ? -1 : x->vid > y->vid;
}

int compareRanges(const void *a, const void *b)
{
    return ioswitchCompareRanges(*static_cast<const Range *>(a), *static_cast<const Range *>(b));
}

int compareRuns(const void *a, const void *b)
{
    return ioswitchCompareRuns(*static_cast<const Run *>(a), *static_cast<const Run *>(b));
}

// One logged vnode (its entries are log[first, end) after sorting) and the file it is, once named.
struct Group {
    vnode_t vp;
    uint32_t vid, first, end;
    uint64_t fileId;
};
struct Naming {
    Group *groups;
    uint32_t count;
};

// vnode_iterate on a cryptex mount: a live vnode that the log holds (same vid) gets its file id. The log's vnode
// pointers are never dereferenced: a vnode recycled since its read is simply not found, and its reads not kept.
int nameVnode(vnode_t vp, void *argument)
{
    auto *naming = static_cast<Naming *>(argument);
    // The groups are sorted by (vp, vid): the first with this vnode pointer (it may appear with several vids).
    uint32_t lo = 0, hi = naming->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (naming->groups[mid].vp < vp)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (Group *hit = naming->groups + lo; hit < naming->groups + naming->count && hit->vp == vp; ++hit) {
        if (hit->vid != vnode_vid(vp) || !vnode_isreg(vp) || vnode_isnamedstream(vp)) continue;
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        if (!vnode_getattr(vp, &va, vfs_context_kernel()) && VATTR_IS_SUPPORTED(&va, va_fileid))
            hit->fileId = va.va_fileid;
    }
    return VNODE_RETURNED;
}

// The log's reads as runs (in first-read order); returns their count, *files the distinct files, *out the runs.
uint32_t buildRuns(ioswitch_record_entry *log, uint32_t entries, Run **out, uint32_t *files, uint32_t *dropped)
{
    *out = nullptr;
    *files = *dropped = 0;
    qsort(log, entries, sizeof(*log), compareEntries);
    auto *groups = static_cast<Group *>(IOMallocData((entries ? entries : 1) * sizeof(Group)));
    auto *ranges = static_cast<Range *>(IOMallocData((entries ? entries : 1) * sizeof(Range)));
    uint32_t groupCount = 0, n = 0, count = 0;
    uint64_t pages = 0;
    Run *runs = nullptr;
    if (groups && ranges) {
        for (uint32_t i = 0; i < entries;) {
            uint32_t j = i;
            while (j < entries && log[j].vp == log[i].vp && log[j].vid == log[i].vid) ++j;
            groups[groupCount++] = {log[i].vp, log[i].vid, i, j, 0};
            i = j;
        }
        Naming naming = {groups, groupCount};
        for (unsigned index = 0; index < IOSWITCH_CRYPTEX_COUNT; ++index)
            if (gPrefetch.mounts[index]) (void)vnode_iterate(gPrefetch.mounts[index], 0, nameVnode, &naming);
        for (uint32_t g = 0; g < groupCount; ++g) {
            for (uint32_t i = groups[g].first; i < groups[g].end; ++i) {
                const uint64_t first = log[i].offset / kPage, end = (log[i].offset + log[i].length + kPage - 1) / kPage;
                if (!groups[g].fileId || !log[i].when || end > UINT32_MAX) {
                    ++*dropped;
                    continue;
                }
                ranges[n++] = {groups[g].fileId, uint32_t(first), uint32_t(end), log[i].when, uint8_t(log[i].index)};
                pages += end - first;
            }
        }
        qsort(ranges, n, sizeof(Range), compareRanges);
        const uint64_t capacity = ioswitchRunCapacity(n, pages, kMaxRun, kMaxGap);
        runs = n && capacity <= kLogEntries * 4ull ? static_cast<Run *>(IOMallocData(capacity * sizeof(Run))) : nullptr;
        if (runs) count = ioswitchMergeRuns(ranges, n, runs, capacity, kMaxRun, kMaxGap, files);
        if (runs) qsort(runs, count, sizeof(Run), compareRuns);
        if (runs && !count) {
            IOFreeData(runs, capacity * sizeof(Run));
            runs = nullptr;
        }
        gPrefetch.runCapacity = runs ? capacity : 0;
    }
    if (groups) IOFreeData(groups, (entries ? entries : 1) * sizeof(Group));
    if (ranges) IOFreeData(ranges, (entries ? entries : 1) * sizeof(Range));
    *out = runs;
    return runs ? count : 0;
}

// kRecordAfterFrameS after the first frame (or kRecordBackstopS after the mounts): stop the prefetch, save a recording.
void afterFrame(thread_call_param_t, thread_call_param_t)
{
    gPrefetch.stop = 1;
    ioswitch_record_entry *log = __atomic_exchange_n(&gPrefetch.log, nullptr, __ATOMIC_ACQ_REL);
    if (!log) return;
    const uint32_t appended = ioswitch_cluster_record_stop();
    const uint32_t entries = appended < kLogEntries ? appended : kLogEntries;
    Run *runs = nullptr;
    uint32_t files = 0, dropped = 0;
    const uint32_t count = buildRuns(log, entries, &runs, &files, &dropped);
    ListHeader header = gPrefetch.key;
    header.runs = count;
    header.files = files;
    header.checksum = ioswitchFnv(reinterpret_cast<const uint8_t *>(runs), count * sizeof(Run));
    uint64_t pages = 0;
    for (uint32_t i = 0; i < count; ++i) pages += runs[i].pages;
    char path[128];
    listPath(path, sizeof(path));
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int error = !count ? ENOENT : vnode_open(path, O_CREAT | O_TRUNC | FWRITE | O_NOFOLLOW, 0644, 0, &vp, ctx);
    if (!error) {
        int resid = 0;
        error = vn_rdwr(UIO_WRITE, vp, reinterpret_cast<caddr_t>(&header), sizeof(header), 0, UIO_SYSSPACE,
                        IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (!error)
            error = vn_rdwr(UIO_WRITE, vp, reinterpret_cast<caddr_t>(runs), static_cast<int>(count * sizeof(Run)),
                            sizeof(header), UIO_SYSSPACE, IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid,
                            vfs_context_proc(ctx));
        vnode_close(vp, FWRITE, ctx);
    }
    if (runs) IOFreeData(runs, gPrefetch.runCapacity * sizeof(Run));
    if (error)
        IOLog("IOSwitch boot prefetch: %s not saved: %d (%u reads logged)\n", path, error, appended);
    else
        IOLog("IOSwitch boot prefetch: recorded %u runs (%llu MiB) of %u cryptex files for the next boots (%u of %u "
              "reads not kept)\n",
              count, (pages * kPage) >> 20, files, dropped + (appended - entries), appended);
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
        at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), " %d: %u/%u/%u", cpu,
                       ticks[CPU_STATE_USER] / 100, ticks[CPU_STATE_SYSTEM] / 100, ticks[CPU_STATE_IDLE] / 100);
    }
    IOLog("%s\n", line);
}

// Both cryptexes are mounted: replay the list, or record one.
void prefetchStart()
{
    if (gPrefetch.started) return;
    gPrefetch.started = 1;
    if (!imageKeys(&gPrefetch.key)) {
        IOLog("IOSwitch boot prefetch: off this boot (the images' attributes)\n");
        return;
    }
    gPrefetch.loadCall = thread_call_allocate_with_options(cpuLoad, nullptr, THREAD_CALL_PRIORITY_LOW, 0);
    gPrefetch.frameCall = thread_call_allocate_with_options(afterFrame, nullptr, THREAD_CALL_PRIORITY_LOW, 0);
    if (!gPrefetch.frameCall) return;
    uint64_t backstop = 0;
    clock_interval_to_deadline(static_cast<uint32_t>(kRecordBackstopS), NSEC_PER_SEC, &backstop);
    thread_call_enter_delayed(gPrefetch.frameCall, backstop);
    uint32_t count = 0;
    const char *why = nullptr;
    if (Run *runs = loadList(gPrefetch.key, &count, &why)) {
        gPrefetch.runs = runs;
        gPrefetch.runCount = count;
        thread_t thread = THREAD_NULL;
        if (kernel_thread_start(prefetchThread, nullptr, &thread) == KERN_SUCCESS)
            thread_deallocate(thread);
        else
            IOFreeData(runs, count * sizeof(Run));
        return;
    }
    IOLog("IOSwitch boot prefetch: no list to replay (%s); this boot records one\n", why);
    auto *log = static_cast<ioswitch_record_entry *>(IOMallocZeroData(kLogEntries * sizeof(ioswitch_record_entry)));
    if (!log) return;
    gPrefetch.log = log;
    ioswitch_cluster_record_start(log, kLogEntries);
}
}  // namespace

// IOSwitchRawMount.inc, right after each cryptex mount: the cluster layer's cryptex mounts (the recording, the kprof
// waits line's "cx"), and once both are mounted, the boot prefetch.
extern "C" void ioswitch_cryptex_mounted(unsigned index, mount_t mount)
{
    if (index >= IOSWITCH_CRYPTEX_COUNT || !mount) return;
    ioswitch_cluster_register_cryptex_mount(index, mount);
    gPrefetch.mounts[index] = mount;
    if (gPrefetch.mounts[0] && gPrefetch.mounts[IOSWITCH_CRYPTEX_COUNT - 1]) prefetchStart();
}

// NativeLegacyDisplay calls this (weakly) at each frame until the first one shows, in any context, interrupts possibly
// disabled: the first call moves afterFrame to the later of kRecordAfterFrameS from now and kRecordUntilS of uptime
// (re-arming replaces the backstop deadline).
extern "C" void ioswitch_boot_io_report(void)
{
    if (__atomic_exchange_n(&gPrefetch.frame, 1u, __ATOMIC_RELAXED) || !gPrefetch.frameCall) return;
    if (gPrefetch.loadCall) thread_call_enter(gPrefetch.loadCall);
    uint64_t deadline = 0, until = 0;
    clock_interval_to_deadline(static_cast<uint32_t>(kRecordAfterFrameS), NSEC_PER_SEC, &deadline);
    nanoseconds_to_absolutetime(kRecordUntilS * NSEC_PER_SEC, &until);
    thread_call_enter_delayed(gPrefetch.frameCall, deadline > until ? deadline : until);
}
