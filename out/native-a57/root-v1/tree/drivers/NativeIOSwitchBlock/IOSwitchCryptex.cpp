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
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/vnode.h>
#include "IOSwitchCryptex.h"

extern "C" int di_root_image_ext(const char *, char *, size_t, dev_t *, bool);
extern "C" void ioswitch_cluster_register_image(vnode_t);  // bsd/vfs/vfs_cluster.c

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
    // image device becomes an uncached read of the same size on the SD card. The System cryptex's APFS reads 4 KiB
    // B_META blocks one at a time (235-246K per boot before the first frame on 11-04, most of the SD's reads).
    // Registering the image with the cluster layer (vfs_cluster.c ioswitch_cluster_register_image) sends its reads of at
    // most 16 KiB through the page cache (repeats are copies, sequential runs get read-ahead) and leaves larger reads, the
    // dyld cache page-ins the cryptex's own page cache keeps, uncached so the same pages are not held twice (r7c cleared
    // the flag for every read). The image is read-only here, so there is nothing to keep coherent. The vnode is the one
    // the backing store holds (one vnode per file).
    vnode_t image = NULLVP;
    if (const int lookup = vnode_lookup(paths[index], 0, &image, vfs_context_current())) {
        IOLog("IOSwitch cryptex: %s stays uncached: lookup %d\n", paths[index], lookup);
    } else {
        ioswitch_cluster_register_image(image);
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
