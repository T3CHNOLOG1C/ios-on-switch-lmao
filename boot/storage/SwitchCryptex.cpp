// SPDX-License-Identifier: BSD-2-Clause-Patent
// Boot-only original DiskImages binding; no custom block device or Apple code patch.
#ifdef SWITCH_CRYPTEX_TEST
#include "SwitchCryptexTest.h"
#else
#include <IOKit/IOBSD.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#endif
#include "block_backend.h"
#include "SwitchCryptex.h"

extern "C" int di_root_image_ext(const char *, char *, size_t, dev_t *, bool);
extern "C" int switch_raw_volume_status(const char *, dev_t *, char *, size_t, uint64_t *);

namespace {
const char *const paths[SWITCH_CRYPTEX_COUNT] = {
  "/private/preboot/Switch-Staging/044-76034-038.dmg",
  "/private/preboot/Switch-Staging/044-75188-038.dmg"
};
const char *const uuids[SWITCH_CRYPTEX_COUNT] = {
  SWITCH_SYSTEM_CRYPTEX_UUID, SWITCH_APP_CRYPTEX_UUID
};
const uint64_t imageBytes[SWITCH_CRYPTEX_COUNT] = {UINT64_C(3303014400), UINT64_C(10485760)};
// A fixed SystemOS development identifier, not a computed/authenticated hash.
const uint8_t systemIdentifier[48] = {
  0x13,0x14,0x8b,0xb4,0x2c,0x8c,0xe4,0xb5,0x6d,0x83,0x5d,0x89,
  0xf9,0x1a,0x4c,0xea,0x1a,0xc0,0x48,0x21,0x11,0xdc,0x34,0xa0,
  0x0c,0xfd,0xdf,0xee,0x36,0x03,0x7c,0xe5,0x04,0x92,0x3b,0xaa,
  0xe4,0xba,0xe2,0xba,0xe6,0xda,0x7d,0x82,0x29,0xc5,0x3e,0x54
};
struct Binding {
  vnode_t source = nullptr;
  mount_t mount = nullptr;
  dev_t preboot = NODEV;
  uint64_t fileID = 0;
  IOService *whole = nullptr, *file = nullptr, *url = nullptr, *controller = nullptr;
  int result = ENXIO;
  bool attempted = false, sourceIOHeld = false;
};
Binding bindings[SWITCH_CRYPTEX_COUNT];
uint32_t busy = 0;
struct Gate {
  bool held = __sync_bool_compare_and_swap(&busy, 0, 1);
  ~Gate() { if (held) __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); }
};
struct Snapshot {
  OSDictionary *value;
  explicit Snapshot(IORegistryEntry *entry) : value(entry->dictionaryWithProperties()) {}
  ~Snapshot() { OSSafeReleaseNULL(value); }
};
bool exactClass(IORegistryEntry *entry, const char *name) {
  return entry && entry->getMetaClass() && !strcmp(entry->getMetaClass()->getClassName(), name);
}
bool active(IORegistryEntry *entry) {
  auto *service = OSDynamicCast(IOService, entry);
  return service && !service->isInactive();
}
bool readonlyMedia(IORegistryEntry *entry) {
  if (!active(entry) || !entry->metaCast("IOMedia")) return false;
  Snapshot p(entry);
  return p.value && p.value->getObject("Writable") == kOSBooleanFalse;
}
bool deviceProperties(OSDictionary *p, dev_t *device, char *path, size_t capacity) {
  if (!p) return false;
  auto *mjr = OSDynamicCast(OSNumber, p->getObject(kIOBSDMajorKey));
  auto *mnr = OSDynamicCast(OSNumber, p->getObject(kIOBSDMinorKey));
  auto *name = OSDynamicCast(OSString, p->getObject(kIOBSDNameKey));
  if (!mjr || !mnr || mjr->unsigned64BitValue() > 255 || mnr->unsigned64BitValue() > 0xffffff ||
      !name || !name->getLength() || name->getLength() > 58 || name->getLength() + 6 > capacity) return false;
  const char *s = name->getCStringNoCopy();
  for (unsigned i = 0; i < name->getLength(); ++i)
    if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
          (s[i] >= '0' && s[i] <= '9'))) return false;
  *device = static_cast<dev_t>(makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
  if (*device == NODEV) return false;
  snprintf(path, capacity, "/dev/%s", s);
  return true;
}
OSIterator *matchingMedia(OSDictionary *properties) {
  OSDictionary *match = IOService::serviceMatching("IOMedia");
  if (!match) return nullptr;
  OSIterator *it = match->setObject("IOPropertyMatch", properties)
    ? IOService::getMatchingServices(match) : nullptr;
  match->release(); return it;
}
IOService *mediaForDevice(dev_t device, const char *bsd) {
  auto *properties = OSDictionary::withCapacity(2);
  auto *mjr = OSNumber::withNumber(static_cast<unsigned>(major(device)), 32);
  auto *mnr = OSNumber::withNumber(static_cast<unsigned>(minor(device)), 32);
  if (!properties || !mjr || !mnr) {
    OSSafeReleaseNULL(mnr); OSSafeReleaseNULL(mjr); OSSafeReleaseNULL(properties); return nullptr;
  }
  bool ready = properties->setObject(kIOBSDMajorKey, mjr) && properties->setObject(kIOBSDMinorKey, mnr);
  OSIterator *it = ready ? matchingMedia(properties) : nullptr;
  mjr->release(); mnr->release(); properties->release();
  if (!it) return nullptr;
  IOService *found = nullptr; unsigned count = 0;
  while (OSObject *object = it->getNextObject()) {
    auto *media = OSDynamicCast(IOService, object);
    if (++count != 1 || !media) { OSSafeReleaseNULL(found); break; }
    Snapshot p(media); dev_t observed = NODEV; char path[64] = {};
    if (readonlyMedia(media) && deviceProperties(p.value, &observed, path, sizeof(path)) &&
        observed == device && !strcmp(path + 5, bsd)) { found = media; found->retain(); }
  }
  it->release(); return count == 1 ? found : nullptr;
}
int prebootDevice(dev_t expected) {
  dev_t actual = NODEV; char path[64]; uint64_t bytes = 0;
  int status = switch_raw_volume_status(SWITCH_PREBOOT_UUID, &actual, path, sizeof(path), &bytes);
  return status == SWITCH_BLOCK_OK && actual == expected && bytes >= imageBytes[0] ? 0 : EAUTH;
}
int pathAttributes(const char *path, Binding &b, bool directory, uint64_t bytes, vnode_t *out) {
  vnode_t vp = nullptr;
  int error = vnode_lookup(path, VNODE_LOOKUP_NOFOLLOW, &vp, vfs_context_kernel());
  if (error) return error;
  struct vnode_attr va; VATTR_INIT(&va);
  VATTR_WANTED(&va, va_uid); VATTR_WANTED(&va, va_mode);
  VATTR_WANTED(&va, va_data_size); VATTR_WANTED(&va, va_fileid); VATTR_WANTED(&va, va_nlink);
  error = vnode_getattr(vp, &va, vfs_context_kernel());
  if (!error && (vnode_vtype(vp) != (directory ? VDIR : VREG) || vnode_mount(vp) != b.mount ||
      !VATTR_IS_SUPPORTED(&va, va_uid) || !VATTR_IS_SUPPORTED(&va, va_mode) ||
      va.va_uid != 0 || (va.va_mode & 022) || (!directory &&
      (!VATTR_IS_SUPPORTED(&va, va_data_size) || !VATTR_IS_SUPPORTED(&va, va_fileid) ||
       !VATTR_IS_SUPPORTED(&va, va_nlink) || va.va_data_size != bytes || va.va_nlink != 1 ||
       va.va_fileid != b.fileID || vp != b.source)))) error = EAUTH;
  if (!error && out) *out = vp; else vnode_put(vp);
  return error;
}
int sourceValid(unsigned index, Binding &b) {
  if (!b.source || !b.mount || prebootDevice(b.preboot) ||
      strcmp(vfs_statfs(b.mount)->f_fstypename, "apfs") || (vfs_flags(b.mount) & MNT_RDONLY)) return EAUTH;
  vnode_t device = vfs_devvp(b.mount);
  dev_t actual = device ? vnode_specrdev(device) : NODEV;
  if (device) vnode_put(device);
  if (actual != b.preboot) return EAUTH;
  int error = pathAttributes("/private/preboot", b, true, 0, nullptr);
  if (!error) error = pathAttributes("/private/preboot/Switch-Staging", b, true, 0, nullptr);
  if (!error) error = pathAttributes(paths[index], b, false, imageBytes[index], nullptr);
  return error;
}
bool originalFileBinding(Binding &b) {
  if (!active(b.file) || !exactClass(b.file, "KDIFileBackingStore")) return false;
  const OSMetaClass *meta = b.file->getMetaClass();
  OSKext *kext = meta->getKext();
  // Pinned original493.0.0 ABI: ctor class size0xc0; vnode_open output and every
  // vnode read/getattr/close use object+0xa8. RO byte+0x98 is initialized before
  // superclass start. See diskimages-native-review/original-components.json.
  if (meta->getClassSize() != 0xc0 || !kext ||
      strcmp(kext->getIdentifierCString(), "com.apple.driver.DiskImages.FileBackingStore") ||
      kext->getVersion() != OSKextParseVersionString("493.0.0")) return false;
  vnode_t actual = nullptr; uint8_t readonly = 0;
  memcpy(&actual, reinterpret_cast<const uint8_t *>(b.file) + 0xa8, sizeof(actual));
  memcpy(&readonly, reinterpret_cast<const uint8_t *>(b.file) + 0x98, sizeof(readonly));
  Snapshot properties(b.file);
  return actual == b.source && readonly == 1 && properties.value &&
    properties.value->getObject("write-protected") == kOSBooleanTrue;
}
bool providerBinding(Binding &b, uint64_t bytes) {
  if (!readonlyMedia(b.whole)) return false;
  Snapshot p(b.whole);
  auto *size = p.value ? OSDynamicCast(OSNumber, p.value->getObject("Size")) : nullptr;
  auto *block = p.value ? OSDynamicCast(OSNumber, p.value->getObject("Preferred Block Size")) : nullptr;
  if (!size || size->unsigned64BitValue() != bytes || !block || block->unsigned64BitValue() != 512 ||
      p.value->getObject("Whole") != kOSBooleanTrue) return false;
  const char *const chain[] = {"IOBlockStorageDriver", "IODiskImageBlockStorageDeviceInKernel",
    "IOHDIXHDDriveInKernel", "KDIDiskImageNub", "KDIReadWriteDiskImage", "KDIFileBackingStore",
    "KDIURL", "IOHDIXController"};
  IORegistryEntry *entry = b.whole->copyParentEntry(gIOServicePlane);
  unsigned step = 0;
  for (unsigned depth = 0; entry && depth < 64; ++depth) {
    if (!active(entry)) break;
    if (exactClass(entry, chain[step])) {
      if ((step == 5 && entry != b.file) || (step == 6 && entry != b.url) ||
          (step == 7 && entry != b.controller)) break;
      if (++step == sizeof(chain) / sizeof(chain[0])) break;
    }
    IORegistryEntry *next = entry->copyParentEntry(gIOServicePlane);
    entry->release(); entry = next;
  }
  OSSafeReleaseNULL(entry);
  return step == sizeof(chain) / sizeof(chain[0]) && originalFileBinding(b);
}
int captureProvider(IOService *returned, Binding &b, uint64_t bytes) {
  IORegistryEntry *entry = returned; entry->retain();
  for (unsigned depth = 0; entry && depth < 64; ++depth) {
    if (!active(entry)) break;
    if (entry->metaCast("IOMedia")) {
      Snapshot p(entry);
      if (p.value && p.value->getObject("Whole") == kOSBooleanTrue) {
        IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
        bool raw = exactClass(parent, "IOBlockStorageDriver");
        OSSafeReleaseNULL(parent);
        if (raw) {
          if (b.whole) break;
          b.whole = OSDynamicCast(IOService, entry); b.whole->retain();
        }
      }
    }
    if (exactClass(entry, "KDIFileBackingStore")) {
      if (b.file) break;
      b.file = OSDynamicCast(IOService, entry); b.file->retain();
    }
    if (exactClass(entry, "KDIURL")) {
      if (b.url) break;
      b.url = OSDynamicCast(IOService, entry); b.url->retain();
    }
    if (exactClass(entry, "IOHDIXController")) { b.controller = OSDynamicCast(IOService, entry); b.controller->retain(); break; }
    IORegistryEntry *next = entry->copyParentEntry(gIOServicePlane);
    entry->release(); entry = next;
  }
  OSSafeReleaseNULL(entry);
  if (!b.whole || !b.file || !b.url || !b.controller || !providerBinding(b, bytes)) return EAUTH;
  Snapshot props(b.url);
  auto *path = props.value ? OSDynamicCast(OSData, props.value->getObject("image-path")) : nullptr;
  const char *expected = paths[bytes == imageBytes[0] ? 0 : 1];
  // _di_root_image supplies OSData::withBytes(path, strlen(path)), without NUL.
  return path && path->getLength() == strlen(expected) &&
    !memcmp(path->getBytesNoCopy(), expected, path->getLength()) ? 0 : EAUTH;
}
bool descendant(IOService *volume, IOService *whole) {
  if (volume == whole) return false;
  IORegistryEntry *entry = volume->copyParentEntry(gIOServicePlane);
  bool found = false;
  for (unsigned depth = 0; entry && depth < 64; ++depth) {
    if (!active(entry)) break;
    if (entry == whole) { found = true; break; }
    IORegistryEntry *next = entry->copyParentEntry(gIOServicePlane);
    entry->release(); entry = next;
  }
  OSSafeReleaseNULL(entry); return found;
}
int queryVolume(unsigned index, Binding &b, dev_t *device, char *path, size_t capacity) {
  if (sourceValid(index, b) || !providerBinding(b, imageBytes[index])) return EAUTH;
  auto *properties = OSDictionary::withCapacity(1);
  auto *uuid = OSString::withCString(uuids[index]);
  if (!properties || !uuid) { OSSafeReleaseNULL(uuid); OSSafeReleaseNULL(properties); return ENOMEM; }
  bool ready = properties->setObject("UUID", uuid);
  OSIterator *it = ready ? matchingMedia(properties) : nullptr;
  uuid->release(); properties->release();
  if (!it) return EAGAIN;
  unsigned count = 0; int error = EAGAIN;
  while (OSObject *object = it->getNextObject()) {
    auto *volume = OSDynamicCast(IOService, object);
    if (++count != 1 || !volume) { error = EAUTH; break; }
    Snapshot p(volume);
    auto *identity = p.value ? OSDynamicCast(OSString, p.value->getObject("UUID")) : nullptr;
    if (!readonlyMedia(volume) || !volume->metaCast("AppleAPFSVolume") ||
        !identity || strcmp(identity->getCStringNoCopy(), uuids[index]) || !descendant(volume, b.whole)) {
      error = EAUTH; continue;
    }
    if (!p.value->getObject(kIOBSDNameKey) || !p.value->getObject(kIOBSDMajorKey) ||
        !p.value->getObject(kIOBSDMinorKey)) { error = EAGAIN; continue; }
    error = deviceProperties(p.value, device, path, capacity) ? 0 : EAUTH;
  }
  it->release();
  if (error) { *device = NODEV; path[0] = 0; }
  return error;
}
void releaseBinding(Binding &b) {
  OSSafeReleaseNULL(b.controller); OSSafeReleaseNULL(b.url);
  OSSafeReleaseNULL(b.file); OSSafeReleaseNULL(b.whole);
  if (b.source) {
    if (b.sourceIOHeld || !vnode_getwithref(b.source)) vnode_close(b.source, FREAD, vfs_context_kernel());
    b.source = nullptr;
  }
  b.mount = nullptr; b.sourceIOHeld = false;
}
int outputs(unsigned index, dev_t *device, char *path, size_t capacity) {
  if (device) *device = NODEV;
  if (path && capacity) path[0] = 0;
  return index < SWITCH_CRYPTEX_COUNT && device && path && capacity >= 64 ? 0 : EINVAL;
}
}

extern "C" int switch_cryptex_attach(unsigned index, dev_t preboot,
    dev_t *device, char *path, size_t capacity) {
  int error = outputs(index, device, path, capacity);
  if (error || preboot == NODEV) return error ? error : EINVAL;
  Gate lock; if (!lock.held) return EBUSY;
  Binding &b = bindings[index];
  if (b.attempted) return preboot != b.preboot ? EAUTH :
    b.result ? b.result : queryVolume(index, b, device, path, capacity);
  if (prebootDevice(preboot)) return EAUTH;
  error = vnode_open(paths[index], FREAD | O_NOFOLLOW, 0, VNODE_LOOKUP_NOFOLLOW,
    &b.source, vfs_context_kernel());
  if (error) return error;
  b.sourceIOHeld = true; b.mount = vnode_mount(b.source); b.preboot = preboot;
  struct vnode_attr va; VATTR_INIT(&va); VATTR_WANTED(&va, va_fileid);
  error = vnode_getattr(b.source, &va, vfs_context_kernel());
  if (!error && !VATTR_IS_SUPPORTED(&va, va_fileid)) error = EAUTH;
  if (!error) { b.fileID = va.va_fileid; error = sourceValid(index, b); }
  if (!error) {
    char bsd[64] = {}; dev_t attached = NODEV;
    b.attempted = true;
    int status = di_root_image_ext(paths[index], bsd, sizeof(bsd), &attached, false);
    error = status ? EIO : (!memchr(bsd, 0, sizeof(bsd)) || !bsd[0] || attached == NODEV) ? EAUTH : 0;
    IOService *returned = !error ? mediaForDevice(attached, bsd) : nullptr;
    if (!error) error = returned ? captureProvider(returned, b, imageBytes[index]) : EAUTH;
    OSSafeReleaseNULL(returned);
    if (!error) error = sourceValid(index, b);
    for (unsigned attempt = 0; !error && attempt < 101; ++attempt) {
      error = queryVolume(index, b, device, path, capacity);
      if (error != EAGAIN || attempt == 100) break;
      IOSleep(100); error = 0;
    }
  }
  if (error) { releaseBinding(b); *device = NODEV; path[0] = 0; }
  else { vnode_put(b.source); b.sourceIOHeld = false; }
  b.result = error;
  return error;
}
extern "C" int switch_cryptex_volume_status(unsigned index, dev_t *device,
    char *path, size_t capacity) {
  int error = outputs(index, device, path, capacity);
  if (error) return error;
  Gate lock; if (!lock.held) return EBUSY;
  Binding &b = bindings[index];
  return b.result ? b.result : queryVolume(index, b, device, path, capacity);
}
extern "C" int switch_cryptex_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]) {
  if (!digest) return 0;
  bzero(digest, 48);
  if (device == NODEV || !uuid || strcmp(uuid, uuids[0])) return 0;
  Gate lock; if (!lock.held) return 0;
  dev_t actual = NODEV; char path[64]; Binding &b = bindings[0];
  if (b.result || queryVolume(0, b, &actual, path, sizeof(path)) || actual != device) return 0;
  memcpy(digest, systemIdentifier, sizeof(systemIdentifier)); return 1;
}
