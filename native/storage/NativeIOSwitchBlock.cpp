// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only logical disk children for the already qualified native SD backend.
#include <IOKit/IOBSD.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include "block_backend.h"

extern "C" {
volatile uint32_t ioswitch_block_devices_published = 0;
volatile uint64_t ioswitch_block_read_requests = 0;
volatile uint64_t ioswitch_block_read_bytes = 0;
volatile uint64_t ioswitch_block_read_failures = 0;
volatile uint32_t ioswitch_block_last_status = IOSW_BLOCK_NOT_READY;
volatile uint32_t ioswitch_block_volume_matches[IOSW_BLOCK_DISK_COUNT] = {};
}

class NativeIOSwitchBlock;
class NativeIOSwitchBlockDevice : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeIOSwitchBlockDevice)
  NativeIOSwitchBlock *owner = nullptr;
  unsigned disk = IOSW_BLOCK_DISK_COUNT;
  uint64_t blocks = 0;
  char label[16] = {};
  bool started = false;
public:
  bool configure(NativeIOSwitchBlock *, unsigned, const iosw_block_info &);
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;
  bool belongsTo(NativeIOSwitchBlock *, unsigned) const;
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  IOReturn doUnmap(IOBlockStorageDeviceExtent *, UInt32, IOStorageUnmapOptions) override {
    return kIOReturnNotWritable;
  }
  char *getVendorString() override { return const_cast<char *>("IOSwitch"); }
  char *getProductString() override { return label; }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override {
    return const_cast<char *>("Native SD/FAT image, read-only");
  }
  IOReturn reportBlockSize(UInt64 *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = IOSW_BLOCK_SECTOR_BYTES; return kIOReturnSuccess;
  }
  IOReturn reportMaxValidBlock(UInt64 *value) override {
    if (!value || !blocks) return kIOReturnBadArgument;
    *value = blocks - 1; return kIOReturnSuccess;
  }
  IOReturn reportEjectability(bool *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { return reportEjectability(value); }
  IOReturn reportWriteProtection(bool *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = true; return kIOReturnSuccess;
  }
  IOReturn reportMediaState(bool *, bool *) override;
  IOReturn doSynchronizeCache() override;
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
  IOReturn getWriteCacheState(bool *value) override { return reportEjectability(value); }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64,
                          IOStorageAttributes *, IOStorageCompletion *) override;
};

class NativeIOSwitchBlock : public IOService {
  OSDeclareDefaultStructors(NativeIOSwitchBlock)
  IOLock *gate = nullptr;
  iosw_block_backend *backend = nullptr;
  uint8_t *scratch = nullptr;
  iosw_block_info info[IOSW_BLOCK_DISK_COUNT] = {};
  bool online = false;
  int startStatus = IOSW_BLOCK_NOT_READY;
  bool healthyLocked(unsigned);
public:
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;
  int status() const { return startStatus; }
  void quiesce();
  bool healthy(unsigned);
  IOReturn read(unsigned, IOMemoryDescriptor *, UInt64, uint32_t, IOStorageCompletion *);
};
OSDefineMetaClassAndStructors(NativeIOSwitchBlockDevice, IOBlockStorageDevice)
OSDefineMetaClassAndStructors(NativeIOSwitchBlock, IOService)

namespace {
// The boot retains this owner. No removal/reinitialization path is advertised.
NativeIOSwitchBlock *publishedOwner = nullptr;
uint32_t publicationState = 0; // 0=unattempted, 1=preparing, 2=published, 3=failed
int publicationStatus = IOSW_BLOCK_NOT_READY;
const char *const volumeUUIDs[IOSW_BLOCK_DISK_COUNT] = {
  "04769D26-2CA0-4825-B443-E2E5F88300F2",
  "8846FE78-2910-4297-AF97-D59BB6F8FC16"
};
void *backendAllocate(void *, size_t bytes) { return bytes ? IOMalloc(bytes) : nullptr; }
void backendRelease(void *, void *allocation, size_t bytes) {
  if (allocation) IOFree(allocation, bytes);
}
IOReturn ioStatus(int status) {
  switch (status) {
  case IOSW_BLOCK_OK: return kIOReturnSuccess;
  case IOSW_BLOCK_NOT_READY: return kIOReturnNotReady;
  case IOSW_BLOCK_INVALID: return kIOReturnBadArgument;
  case IOSW_BLOCK_TIMEOUT: return kIOReturnTimeout;
  case IOSW_BLOCK_NOMEM: return kIOReturnNoMemory;
  default: return kIOReturnIOError;
  }
}
}

bool NativeIOSwitchBlockDevice::configure(NativeIOSwitchBlock *provider,
                                         unsigned index, const iosw_block_info &metadata)
{
  if (owner || !provider || index >= IOSW_BLOCK_DISK_COUNT || !metadata.size_bytes ||
      metadata.size_bytes % IOSW_BLOCK_SECTOR_BYTES ||
      !metadata.label[0] || !memchr(metadata.label, 0, sizeof(metadata.label))) return false;
  if (!setProperty(kIOMaximumBlockCountReadKey,
      static_cast<UInt64>(IOSW_BLOCK_MAX_TRANSFER_BYTES / IOSW_BLOCK_SECTOR_BYTES), 64) ||
      !setProperty(kIOMaximumByteCountReadKey,
      static_cast<UInt64>(IOSW_BLOCK_MAX_TRANSFER_BYTES), 64)) return false;
  owner = provider;
  owner->retain(); // Survives detach until the child's last client releases it.
  disk = index;
  blocks = metadata.size_bytes / IOSW_BLOCK_SECTOR_BYTES;
  strlcpy(label, metadata.label, sizeof(label));
  setName(label);
  return true;
}

bool NativeIOSwitchBlockDevice::start(IOService *provider)
{
  if (provider != owner || !owner || !blocks || !IOBlockStorageDevice::start(provider)) return false;
  __atomic_store_n(&started, true, __ATOMIC_RELEASE);
  // The owner registers both children only after both starts succeed.
  return true;
}

void NativeIOSwitchBlockDevice::stop(IOService *provider)
{
  // Both children share one immutable physical transport. Stopping either one
  // drains the current request and prevents new reads on both children.
  if (owner) owner->quiesce();
  __atomic_store_n(&started, false, __ATOMIC_RELEASE);
  IOBlockStorageDevice::stop(provider);
}

void NativeIOSwitchBlockDevice::free()
{
  if (owner) { owner->release(); owner = nullptr; }
  IOBlockStorageDevice::free();
}

bool NativeIOSwitchBlockDevice::belongsTo(NativeIOSwitchBlock *provider, unsigned index) const
{
  return owner == provider && disk == index && __atomic_load_n(&started, __ATOMIC_ACQUIRE);
}

IOReturn NativeIOSwitchBlockDevice::reportMediaState(bool *present, bool *changed)
{
  if (!present) return kIOReturnBadArgument;
  *present = owner && __atomic_load_n(&started, __ATOMIC_ACQUIRE) && owner->healthy(disk);
  if (changed) *changed = false;
  return kIOReturnSuccess;
}

IOReturn NativeIOSwitchBlockDevice::doSynchronizeCache()
{
  return owner && __atomic_load_n(&started, __ATOMIC_ACQUIRE) && owner->healthy(disk)
    ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn NativeIOSwitchBlockDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer,
    UInt64 block, UInt64 count, IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= blocks ||
      count > blocks - block || count > IOSW_BLOCK_MAX_TRANSFER_BYTES / IOSW_BLOCK_SECTOR_BYTES ||
      buffer->getLength() < count * IOSW_BLOCK_SECTOR_BYTES) return kIOReturnBadArgument;
  if (buffer->getDirection() != kIODirectionIn) return kIOReturnNotWritable;
  if (!owner || !__atomic_load_n(&started, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
  return owner->read(disk, buffer, block * IOSW_BLOCK_SECTOR_BYTES,
      static_cast<uint32_t>(count * IOSW_BLOCK_SECTOR_BYTES), completion);
}

bool NativeIOSwitchBlock::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  NativeIOSwitchBlockDevice *pending[IOSW_BLOCK_DISK_COUNT] = {};
  bool attached[IOSW_BLOCK_DISK_COUNT] = {}, started[IOSW_BLOCK_DISK_COUNT] = {};
  gate = IOLockAlloc();
  scratch = static_cast<uint8_t *>(IOMalloc(IOSW_BLOCK_MAX_TRANSFER_BYTES));
  startStatus = IOSW_BLOCK_NOMEM;
  if (gate && scratch) {
    iosw_block_allocator allocator = {nullptr, backendAllocate, backendRelease};
    IOLockLock(gate);
    startStatus = iosw_block_create(&allocator, &backend);
    if (!startStatus && !backend) startStatus = IOSW_BLOCK_CORRUPT;
    for (unsigned i = 0; i < IOSW_BLOCK_DISK_COUNT && !startStatus; ++i) {
      startStatus = iosw_block_get_info(backend, i, &info[i]);
      if (!startStatus && (!info[i].size_bytes || info[i].size_bytes % IOSW_BLOCK_SECTOR_BYTES ||
          !info[i].label[0] || !memchr(info[i].label, 0, sizeof(info[i].label)))) {
        startStatus = IOSW_BLOCK_CORRUPT;
      }
    }
    IOLockUnlock(gate);
    for (unsigned i = 0; i < IOSW_BLOCK_DISK_COUNT && !startStatus; ++i) {
      pending[i] = OSTypeAlloc(NativeIOSwitchBlockDevice);
      if (!pending[i] || !pending[i]->init(static_cast<OSDictionary *>(nullptr)) ||
          !pending[i]->configure(this, i, info[i])) { startStatus = IOSW_BLOCK_NOMEM; break; }
      if (!pending[i]->attach(this)) { startStatus = IOSW_BLOCK_NOT_READY; break; }
      attached[i] = true;
      if (!pending[i]->start(this)) { startStatus = IOSW_BLOCK_NOT_READY; break; }
      started[i] = true;
    }
  }
  if (startStatus) {
    for (unsigned i = 0; i < IOSW_BLOCK_DISK_COUNT; ++i) {
      if (!pending[i]) continue;
      if (started[i]) pending[i]->stop(this);
      if (attached[i]) pending[i]->detach(this);
      pending[i]->release();
    }
    IOService::stop(provider);
    return false;
  }
  IOLockLock(gate);
  online = true;
  IOLockUnlock(gate);
  // No fallible work follows the first publication. Existing registry/client
  // references own each published child; never undo publication as startup cleanup.
  for (unsigned i = 0; i < IOSW_BLOCK_DISK_COUNT; ++i) {
    pending[i]->registerService();
    __atomic_fetch_add(&ioswitch_block_devices_published, 1, __ATOMIC_RELAXED);
    pending[i]->release();
  }
  return true;
}

bool NativeIOSwitchBlock::healthyLocked(unsigned disk)
{
  if (!online || !backend || disk >= IOSW_BLOCK_DISK_COUNT) return false;
  iosw_block_info current = {};
  int result = iosw_block_get_info(backend, disk, &current);
  if (!result && (current.size_bytes != info[disk].size_bytes ||
      memcmp(current.label, info[disk].label, sizeof(current.label)))) result = IOSW_BLOCK_CORRUPT;
  if (result) {
    online = false;
    __atomic_store_n(&ioswitch_block_last_status, static_cast<uint32_t>(result), __ATOMIC_RELAXED);
  }
  return !result;
}

bool NativeIOSwitchBlock::healthy(unsigned disk)
{
  if (!gate) return false;
  IOLockLock(gate);
  bool result = healthyLocked(disk);
  IOLockUnlock(gate);
  return result;
}

IOReturn NativeIOSwitchBlock::read(unsigned disk, IOMemoryDescriptor *buffer,
    UInt64 offset, uint32_t bytes, IOStorageCompletion *completion)
{
  // The child checks every public request before reaching this serialized owner.
  IOStorageCompletion done = *completion;
  IOLockLock(gate);
  if (!healthyLocked(disk)) { IOLockUnlock(gate); return kIOReturnNotReady; }
  __atomic_fetch_add(&ioswitch_block_read_requests, 1, __ATOMIC_RELAXED);
  int result = iosw_block_read(backend, disk, offset, scratch, bytes);
  __atomic_store_n(&ioswitch_block_last_status, static_cast<uint32_t>(result), __ATOMIC_RELAXED);
  UInt64 actual = 0;
  IOReturn status = ioStatus(result);
  if (result) {
    // A transport error never exposes a partially filled backend buffer.
    online = false;
  } else {
    actual = buffer->writeBytes(0, scratch, bytes);
    if (actual > bytes) actual = 0;
    if (actual != bytes) status = kIOReturnIOError;
    __atomic_fetch_add(&ioswitch_block_read_bytes, actual, __ATOMIC_RELAXED);
  }
  if (status) __atomic_fetch_add(&ioswitch_block_read_failures, 1, __ATOMIC_RELAXED);
  IOLockUnlock(gate);
  // Completion may synchronously submit another request to either child.
  done.action(done.target, done.parameter, status, actual);
  return kIOReturnSuccess;
}

void NativeIOSwitchBlock::quiesce()
{
  if (gate) { IOLockLock(gate); online = false; IOLockUnlock(gate); }
}

void NativeIOSwitchBlock::stop(IOService *provider)
{
  quiesce();
  // Keep the backend/scratch until the last child and client release this owner.
  IOService::stop(provider);
}

void NativeIOSwitchBlock::free()
{
  if (gate) {
    IOLockLock(gate);
    online = false;
    if (backend) { iosw_block_destroy(backend); backend = nullptr; }
    if (scratch) { IOFree(scratch, IOSW_BLOCK_MAX_TRANSFER_BYTES); scratch = nullptr; }
    IOLockUnlock(gate);
    IOLockFree(gate); gate = nullptr;
  } else if (scratch) { IOFree(scratch, IOSW_BLOCK_MAX_TRANSFER_BYTES); scratch = nullptr; }
  IOService::free();
}

extern "C" int ioswitch_block_publish(void)
{
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&publicationState, &expected, 1, false,
      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    if (expected == 1) return IOSW_BLOCK_NOT_READY;
    if (expected == 2) return publishedOwner && publishedOwner->healthy(0)
      ? IOSW_BLOCK_OK : IOSW_BLOCK_NOT_READY;
    return publicationStatus;
  }
  IOService *resources = IOService::getResourceService();
  NativeIOSwitchBlock *owner = OSTypeAlloc(NativeIOSwitchBlock);
  bool attached = false;
  int result = IOSW_BLOCK_NOMEM;
  if (owner && owner->init(static_cast<OSDictionary *>(nullptr))) {
    if (resources && owner->attach(resources)) {
      attached = true;
      if (owner->start(resources)) result = IOSW_BLOCK_OK;
      else result = owner->status();
    } else result = IOSW_BLOCK_NOT_READY;
  }
  if (!result) {
    publishedOwner = owner; // Keep the allocation reference for this boot.
  } else if (owner) {
    if (attached) owner->detach(resources);
    owner->release();
  }
  publicationStatus = result;
  __atomic_store_n(&ioswitch_block_last_status, static_cast<uint32_t>(result), __ATOMIC_RELAXED);
  __atomic_store_n(&publicationState, result ? 3 : 2, __ATOMIC_RELEASE);
  return result;
}

extern "C" int ioswitch_block_volume_path(unsigned disk, char *path, unsigned size)
{
  if (!path || !size) return IOSW_BLOCK_INVALID;
  path[0] = 0;
  if (disk >= IOSW_BLOCK_DISK_COUNT || size < 7) return IOSW_BLOCK_INVALID;
  if (__atomic_load_n(&publicationState, __ATOMIC_ACQUIRE) != 2 ||
      !publishedOwner || !publishedOwner->healthy(disk)) return IOSW_BLOCK_NOT_READY;
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSString *wanted = OSString::withCString(volumeUUIDs[disk]);
  if (!matching || !properties || !wanted) {
    OSSafeReleaseNULL(wanted); OSSafeReleaseNULL(properties); OSSafeReleaseNULL(matching);
    return IOSW_BLOCK_NOMEM;
  }
  bool ready = properties->setObject("UUID", wanted) && matching->setObject("IOPropertyMatch", properties);
  wanted->release(); properties->release();
  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  matching->release();
  if (!iterator) return ready ? IOSW_BLOCK_NOT_READY : IOSW_BLOCK_NOMEM;
  unsigned matches = 0;
  bool selected = false;
  char candidate[64] = {};
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    if (++matches != 1) { selected = false; break; }
    // Matching and BSD property publication are asynchronous. Keep one retained
    // scalar-property snapshot, rather than borrowed values from separate reads.
    struct PropertySnapshot {
      OSDictionary *value;
      ~PropertySnapshot() { if (value) value->release(); }
    } snapshot = {media->dictionaryWithProperties()};
    if (!snapshot.value) continue;
    OSString *uuid = OSDynamicCast(OSString, snapshot.value->getObject("UUID"));
    OSString *name = OSDynamicCast(OSString, snapshot.value->getObject(kIOBSDNameKey));
    OSNumber *major = OSDynamicCast(OSNumber, snapshot.value->getObject(kIOBSDMajorKey));
    OSNumber *minor = OSDynamicCast(OSNumber, snapshot.value->getObject(kIOBSDMinorKey));
    if (media->isInactive() || !uuid || uuid->getLength() != 36 ||
        strcmp(uuid->getCStringNoCopy(), volumeUUIDs[disk]) || !name || !major || !minor ||
        !name->getLength() || name->getLength() > sizeof(candidate) - 6 ||
        name->getLength() > size - 6 || major->unsigned64BitValue() > 255 ||
        minor->unsigned64BitValue() > 0xffffff) continue;
    const char *bsd = name->getCStringNoCopy();
    bool validName = true;
    for (unsigned at = 0; at < name->getLength(); ++at) {
      char c = bsd[at];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-')) validName = false;
    }
    if (!validName) continue;
    IORegistryEntry *entry = media;
    entry->retain();
    for (unsigned depth = 0; entry && depth < 64; ++depth) {
      NativeIOSwitchBlockDevice *child = OSDynamicCast(NativeIOSwitchBlockDevice, entry);
      if (child && !child->isInactive() && child->belongsTo(publishedOwner, disk)) {
        snprintf(candidate, sizeof(candidate), "/dev/%s", bsd);
        selected = true;
        break;
      }
      IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
      entry->release();
      entry = parent;
    }
    if (entry) entry->release();
  }
  iterator->release();
  __atomic_store_n(&ioswitch_block_volume_matches[disk], matches, __ATOMIC_RELAXED);
  if (matches > 1) return IOSW_BLOCK_CORRUPT;
  if (!selected || !publishedOwner->healthy(disk)) return IOSW_BLOCK_NOT_READY;
  strlcpy(path, candidate, size);
  return IOSW_BLOCK_OK;
}
