// SPDX-License-Identifier: BSD-2-Clause-Patent
// Original OS/SystemOS stay read-only; persistent Data shares their SD owner lock.
#include <IOKit/IOBSD.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include "block_backend.h"
#include "raw_apfs.h"
#include "IOSwitchFiles.h"
#include "NativeIOSwitchBlock.h"

static constexpr unsigned dataDisk = IOSW_BLOCK_DISK_COUNT;
static constexpr unsigned childCount = IOSW_BLOCK_DISK_COUNT + 1;
static constexpr uint64_t dataBytes = UINT64_C(17179869184);
static const char dataUUID[] = "CCD5A1A2-2FDC-4B25-B322-AE1722871BC9";
#include <sys/types.h>
#include <sys/param.h>

extern "C" {
void ioswitch_root_io_progress(uint64_t read_bytes, unsigned error);
extern volatile uint64_t ioswitch_dma_record[13];
volatile uint32_t ioswitch_block_devices_published = 0;
// Legacy observation symbol; this development path performs no block hashing.
volatile uint64_t ioswitch_block_integrity_failures = 0;
int native_verified_block_volume_device(const char *, dev_t *, char *, size_t);
volatile uint64_t ioswitch_block_read_requests = 0;
volatile uint64_t ioswitch_block_read_bytes = 0;
volatile uint64_t ioswitch_block_read_failures = 0;
volatile uint64_t ioswitch_data_write_requests = 0, ioswitch_data_write_bytes = 0;
volatile uint64_t ioswitch_data_write_failures = 0, ioswitch_data_flush_requests = 0;
volatile uint32_t ioswitch_block_last_status = IOSW_BLOCK_NOT_READY;
volatile uint32_t ioswitch_block_volume_matches[IOSW_BLOCK_DISK_COUNT] = {};
}

class NativeIOSwitchBlock;
class NativeIOSwitchBlockDevice : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeIOSwitchBlockDevice)
  NativeIOSwitchBlock *owner = nullptr;
  unsigned disk = childCount;
  uint64_t blocks = 0;
  char label[16] = {};
  bool started = false, rawMode = false;
public:
  bool configure(NativeIOSwitchBlock *, unsigned, const iosw_block_info &);
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;
  bool belongsTo(NativeIOSwitchBlock *, unsigned) const;
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return disk == dataDisk ? kIOReturnUnsupported : kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  IOReturn doUnmap(IOBlockStorageDeviceExtent *, UInt32, IOStorageUnmapOptions) override {
    return disk == dataDisk ? kIOReturnUnsupported : kIOReturnNotWritable;
  }
  char *getVendorString() override { return const_cast<char *>("IOSwitch"); }
  char *getProductString() override { return label; }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override {
    return const_cast<char *>(rawMode ? "Native SD/APFS container" :
      disk == dataDisk ? "Native SD/FAT persistent Data" : "Native SD/FAT image, read-only");
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
    *value = disk != dataDisk; return kIOReturnSuccess;
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
  iosw_data_backend *data = nullptr;
  iosw_raw_apfs raw = {};
  bool rawProfile = false;
  uint8_t *scratch = nullptr;
  iosw_block_info info[childCount] = {};
  uint64_t readBytes = 0, reportedBytes = 0;
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
  bool matchesVolume(unsigned, const char *, uint8_t * = nullptr, bool rawVolume = false);
  bool isRawProfile() const { return rawProfile; }
  IOReturn transfer(unsigned, IOMemoryDescriptor *, UInt64, uint32_t, bool, IOStorageCompletion *);
  IOReturn synchronize(unsigned);
  bool dataCapacity(uint64_t *);
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
// Fixed development image identifiers preserve the existing boot metadata.
// They are not runtime-verified digests and do not authenticate current media.
constexpr size_t imageIdentifierBytes = 48;
const uint8_t developmentImageIdentifiers[IOSW_BLOCK_DISK_COUNT][imageIdentifierBytes] = {
  {
    0xfd, 0x86, 0xb9, 0x5b, 0x70, 0x33, 0xe3, 0x52, 0x84, 0x3f, 0x98, 0xd1,
    0x20, 0x4a, 0xde, 0x29, 0x45, 0x9b, 0xf6, 0x08, 0xa7, 0xe1, 0xcc, 0xb1,
    0xa9, 0xb0, 0x4a, 0xc9, 0x35, 0x33, 0x29, 0x2e, 0xb3, 0x9d, 0xe7, 0xc3,
    0x34, 0xeb, 0xe9, 0x8b, 0x43, 0xd0, 0xdb, 0x47, 0x0f, 0xa1, 0x6e, 0x77
  },
  {
    0x13, 0x14, 0x8b, 0xb4, 0x2c, 0x8c, 0xe4, 0xb5, 0x6d, 0x83, 0x5d, 0x89,
    0xf9, 0x1a, 0x4c, 0xea, 0x1a, 0xc0, 0x48, 0x21, 0x11, 0xdc, 0x34, 0xa0,
    0x0c, 0xfd, 0xdf, 0xee, 0x36, 0x03, 0x7c, 0xe5, 0x04, 0x92, 0x3b, 0xaa,
    0xe4, 0xba, 0xe2, 0xba, 0xe6, 0xda, 0x7d, 0x82, 0x29, 0xc5, 0x3e, 0x54
  }
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
  case IOSW_BLOCK_INTEGRITY: return kIOReturnNotPrivileged;
  default: return kIOReturnIOError;
  }
}
}

bool NativeIOSwitchBlockDevice::configure(NativeIOSwitchBlock *provider,
                                         unsigned index, const iosw_block_info &metadata)
{
  if (owner || !provider || index >= childCount || !metadata.size_bytes ||
      metadata.size_bytes % IOSW_BLOCK_SECTOR_BYTES ||
      !metadata.label[0] || !memchr(metadata.label, 0, sizeof(metadata.label))) return false;
  if (!setProperty(kIOMaximumBlockCountReadKey,
      static_cast<UInt64>(IOSW_BLOCK_MAX_TRANSFER_BYTES / IOSW_BLOCK_SECTOR_BYTES), 64) ||
      !setProperty(kIOMaximumByteCountReadKey,
      static_cast<UInt64>(IOSW_BLOCK_MAX_TRANSFER_BYTES), 64)) return false;
  if (index == dataDisk &&
      (!setProperty(kIOMaximumBlockCountWriteKey,
        static_cast<UInt64>(IOSW_BLOCK_MAX_TRANSFER_BYTES / IOSW_BLOCK_SECTOR_BYTES), 64) ||
       !setProperty(kIOMaximumByteCountWriteKey,
        static_cast<UInt64>(IOSW_BLOCK_MAX_TRANSFER_BYTES), 64))) return false;
  owner = provider;
  owner->retain(); // Survives detach until the child's last client releases it.
  disk = index;
  rawMode = provider->isRawProfile();
  blocks = metadata.size_bytes / IOSW_BLOCK_SECTOR_BYTES;
  strlcpy(label, metadata.label, sizeof(label));
  setName(label);
  return true;
}

bool NativeIOSwitchBlockDevice::start(IOService *provider)
{
  if (provider != owner || !owner || !blocks || !IOBlockStorageDevice::start(provider)) return false;
  __atomic_store_n(&started, true, __ATOMIC_RELEASE);
  // The owner registers selected children only after every start succeeds.
  return true;
}

void NativeIOSwitchBlockDevice::stop(IOService *provider)
{
  // All children share one physical transport. Stopping any child drains
  // the current request and prevents further reads, writes, and flushes.
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
  return owner && __atomic_load_n(&started, __ATOMIC_ACQUIRE)
    ? owner->synchronize(disk) : kIOReturnNotReady;
}

IOReturn NativeIOSwitchBlockDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer,
    UInt64 block, UInt64 count, IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= blocks ||
      count > blocks - block || count > IOSW_BLOCK_MAX_TRANSFER_BYTES / IOSW_BLOCK_SECTOR_BYTES ||
      buffer->getLength() < count * IOSW_BLOCK_SECTOR_BYTES) return kIOReturnBadArgument;
  IODirection direction = buffer->getDirection();
  if (direction != kIODirectionIn && direction != kIODirectionOut) return kIOReturnBadArgument;
  bool writing = direction == kIODirectionOut;
  if (writing && disk != dataDisk) return kIOReturnNotWritable;
  if (!owner || !__atomic_load_n(&started, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
  return owner->transfer(disk, buffer, block * IOSW_BLOCK_SECTOR_BYTES,
      static_cast<uint32_t>(count * IOSW_BLOCK_SECTOR_BYTES), writing, completion);
}

bool NativeIOSwitchBlock::start(IOService *provider)
{
  if (!IOService::start(provider)) return false;
  NativeIOSwitchBlockDevice *pending[childCount] = {};
  bool attached[childCount] = {}, started[childCount] = {};
  gate = IOLockAlloc();
  scratch = static_cast<uint8_t *>(IOMalloc(IOSW_BLOCK_MAX_TRANSFER_BYTES));
  startStatus = IOSW_BLOCK_NOMEM;
  if (gate && scratch) {
    iosw_block_allocator allocator = {nullptr, backendAllocate, backendRelease};
    IOLockLock(gate);
    int profile = ioswitch_files_storage_profile();
    rawProfile = profile == IOSW_STORAGE_RAW;
    if (profile != IOSW_STORAGE_FAT && profile != IOSW_STORAGE_RAW) startStatus = IOSW_BLOCK_INVALID;
    else if (rawProfile) {
      startStatus = ioswitch_files_raw_open(&raw, scratch, IOSW_BLOCK_MAX_TRANSFER_BYTES);
      if (!startStatus) startStatus = iosw_raw_apfs_get_info(&raw, &info[dataDisk]);
    } else {
      startStatus = iosw_block_create(&allocator, &backend);
      if (!startStatus && !backend) startStatus = IOSW_BLOCK_CORRUPT;
      if (!startStatus) startStatus = iosw_data_create(&allocator, backend, &data);
      if (!startStatus && !data) startStatus = IOSW_BLOCK_CORRUPT;
    }
    for (unsigned i = 0; i < childCount && !startStatus; ++i) {
      if (rawProfile && i != dataDisk) continue;
      if (!rawProfile) startStatus = i == dataDisk ? iosw_data_get_info(data, &info[i])
                                 : iosw_block_get_info(backend, i, &info[i]);
      if (!startStatus && !rawProfile && i == dataDisk && info[i].size_bytes != dataBytes) startStatus = IOSW_BLOCK_CORRUPT;
      if (!startStatus && (!info[i].size_bytes || info[i].size_bytes % IOSW_BLOCK_SECTOR_BYTES ||
          !info[i].label[0] || !memchr(info[i].label, 0, sizeof(info[i].label)))) startStatus = IOSW_BLOCK_CORRUPT;
    }
    IOLockUnlock(gate);
    for (unsigned i = 0; i < childCount && !startStatus; ++i) {
      if (rawProfile && i != dataDisk) continue;
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
    for (unsigned i = 0; i < childCount; ++i) {
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
  for (unsigned i = 0; i < childCount; ++i) {
    if (!pending[i]) continue;
    pending[i]->registerService();
    __atomic_fetch_add(&ioswitch_block_devices_published, 1, __ATOMIC_RELAXED);
    pending[i]->release();
  }
  return true;
}

bool NativeIOSwitchBlock::healthyLocked(unsigned disk)
{
  if (!online || disk >= childCount || (rawProfile ? disk != dataDisk : !backend || !data)) return false;
  iosw_block_info current = {};
  int result = rawProfile ? iosw_raw_apfs_get_info(&raw, &current)
    : disk == dataDisk ? iosw_data_get_info(data, &current) : iosw_block_get_info(backend, disk, &current);
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

IOReturn NativeIOSwitchBlock::transfer(unsigned disk, IOMemoryDescriptor *buffer,
    UInt64 offset, uint32_t bytes, bool writing, IOStorageCompletion *completion)
{
  // The child has validated bounds, alignment, direction and the 64 KiB limit.
  IOStorageCompletion done = *completion;
  IOLockLock(gate);
  if (!healthyLocked(disk)) { IOLockUnlock(gate); return kIOReturnNotReady; }
  int result = IOSW_BLOCK_OK;
  UInt64 actual = 0;
  bool progress = false;
  IOReturn status = kIOReturnSuccess;
  if (writing) {
    __atomic_fetch_add(&ioswitch_data_write_requests, 1, __ATOMIC_RELAXED);
    // A descriptor fault or short copy must never submit even one media sector.
    if (disk != dataDisk || buffer->readBytes(0, scratch, bytes) != bytes) {
      status = kIOReturnIOError;
    } else {
      result = rawProfile ? iosw_raw_apfs_write(&raw, offset, scratch, bytes)
                          : iosw_data_write(data, offset, scratch, bytes);
      status = ioStatus(result);
      if (!result) actual = bytes;
      else online = false; // Failed media writes may already have changed a prefix.
    }
    __atomic_fetch_add(&ioswitch_data_write_bytes, actual, __ATOMIC_RELAXED);
    if (status) __atomic_fetch_add(&ioswitch_data_write_failures, 1, __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_add(&ioswitch_block_read_requests, 1, __ATOMIC_RELAXED);
    result = rawProfile ? iosw_raw_apfs_read(&raw, offset, scratch, bytes)
      : disk == dataDisk ? iosw_data_read(data, offset, scratch, bytes)
                             : iosw_block_read(backend, disk, offset, scratch, bytes);
    status = ioStatus(result);
    if (result) {
      online = false; // Never expose an incomplete transport read to the client.
    } else {
      readBytes += bytes;
      progress = readBytes - reportedBytes >= UINT64_C(1048576);
      actual = buffer->writeBytes(0, scratch, bytes);
      if (actual > bytes) actual = 0;
      if (actual != bytes) status = kIOReturnIOError;
      __atomic_fetch_add(&ioswitch_block_read_bytes, actual, __ATOMIC_RELAXED);
    }
    if (status) __atomic_fetch_add(&ioswitch_block_read_failures, 1, __ATOMIC_RELAXED);
  }
  unsigned progressError = result ? static_cast<unsigned>(result) : status ? IOSW_BLOCK_IO : IOSW_BLOCK_OK;
  __atomic_store_n(&ioswitch_block_last_status, progressError, __ATOMIC_RELAXED);
  if (status) progress = true;
  if (progress) reportedBytes = readBytes;
  uint64_t completedReadBytes = readBytes;
  IOLockUnlock(gate);
  if (progress) ioswitch_root_io_progress(completedReadBytes, progressError);
  // Exactly one completion, outside the owner lock; callbacks may submit more I/O.
  done.action(done.target, done.parameter, status, actual);
  return kIOReturnSuccess;
}

IOReturn NativeIOSwitchBlock::synchronize(unsigned disk)
{
  if (!gate) return kIOReturnNotReady;
  IOLockLock(gate);
  int result = healthyLocked(disk) ? IOSW_BLOCK_OK : IOSW_BLOCK_NOT_READY;
  if (!result && disk == dataDisk) {
    __atomic_fetch_add(&ioswitch_data_flush_requests, 1, __ATOMIC_RELAXED);
    result = rawProfile ? iosw_raw_apfs_sync(&raw) : iosw_data_sync(data); // Real CMD13 barrier.
    if (result) online = false;
  }
  __atomic_store_n(&ioswitch_block_last_status, static_cast<uint32_t>(result), __ATOMIC_RELAXED);
  IOLockUnlock(gate);
  return ioStatus(result);
}

bool NativeIOSwitchBlock::dataCapacity(uint64_t *bytes)
{
  if (!bytes || !gate) return false;
  *bytes = 0;
  IOLockLock(gate);
  bool valid = healthyLocked(dataDisk);
  if (valid) *bytes = info[dataDisk].size_bytes;
  IOLockUnlock(gate);
  return valid;
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
    if (rawProfile) iosw_raw_apfs_close(&raw);
    if (data) { iosw_data_destroy(data); data = nullptr; }
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
    if (expected == 2) return publishedOwner && publishedOwner->healthy(publishedOwner->isRawProfile() ? dataDisk : 0)
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

bool NativeIOSwitchBlock::matchesVolume(unsigned disk, const char *uuid, uint8_t *digest, bool rawVolume)
{
  if (digest) bzero(digest, imageIdentifierBytes);
  if (!gate || !uuid || disk >= childCount || (!rawProfile && disk == dataDisk && digest)) return false;
  IOLockLock(gate);
  // Raw Data/Preboot lookup proves only exact media identity under this one
  // container child. Legacy OS/root queries still accept only original OS UUID.
  bool valid = healthyLocked(disk) && (rawProfile
    ? disk == dataDisk && (rawVolume || !strcmp(uuid, volumeUUIDs[0])) && (!digest || !strcmp(uuid, volumeUUIDs[0]))
    : !strcmp(uuid, disk == dataDisk ? dataUUID : volumeUUIDs[disk]));
  if (valid && digest) memcpy(digest, developmentImageIdentifiers[rawProfile ? 0 : disk], imageIdentifierBytes);
  IOLockUnlock(gate);
  return valid;
}

namespace {
struct PropertySnapshot {
  OSDictionary *value;
  ~PropertySnapshot() { if (value) value->release(); }
};
// Registry references remain retained across each parent traversal. Only the
// actual concrete child, with this boot's owner/index, can establish media identity.
bool matchesMedia(IOService *media, const char *uuid, uint8_t *digest = nullptr, bool persistent = false, bool rawVolume = false) {
  if (!media || media->isInactive() || !uuid || strlen(uuid) != 36 ||
      __atomic_load_n(&publicationState, __ATOMIC_ACQUIRE) != 2 || !publishedOwner) return false;
  unsigned disk = 0;
  if (publishedOwner->isRawProfile()) {
    if (persistent || (!rawVolume && strcmp(uuid, volumeUUIDs[0]))) return false;
    disk = dataDisk;
  } else if (rawVolume) return false;
  else if (persistent) {
    if (strcmp(uuid, dataUUID) || digest) return false;
    disk = dataDisk;
  } else {
    while (disk < IOSW_BLOCK_DISK_COUNT && strcmp(uuid, volumeUUIDs[disk])) ++disk;
    if (disk == IOSW_BLOCK_DISK_COUNT) return false;
  }
  IORegistryEntry *entry = media;
  entry->retain();
  bool result = false;
  for (unsigned depth = 0; entry && depth < 64; ++depth) {
    auto *child = OSDynamicCast(NativeIOSwitchBlockDevice, entry);
    if (child && !child->isInactive() && child->belongsTo(publishedOwner, disk)) {
      result = publishedOwner->matchesVolume(disk, uuid, digest, rawVolume);
      break;
    }
    IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
    entry->release(); entry = parent;
  }
  if (entry) entry->release();
  return result;
}
OSIterator *matchingMedia(OSDictionary *properties) {
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  if (!matching) return nullptr;
  bool ready = matching->setObject("IOPropertyMatch", properties);
  OSIterator *result = ready ? IOService::getMatchingServices(matching) : nullptr;
  matching->release();
  return result;
}
bool actualDevice(OSDictionary *properties, dev_t *device) {
  auto *mjr = OSDynamicCast(OSNumber, properties->getObject(kIOBSDMajorKey));
  auto *mnr = OSDynamicCast(OSNumber, properties->getObject(kIOBSDMinorKey));
  if (!mjr || !mnr || mjr->unsigned64BitValue() > 255 || mnr->unsigned64BitValue() > 0xffffff) return false;
  *device = static_cast<dev_t>(makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
  return *device != NODEV;
}
int volumeIdentityQuery(dev_t device, const char *expectedUUID, uint8_t *digest, bool persistent = false, bool rawVolume = false) {
  if (digest) bzero(digest, imageIdentifierBytes);
  if (device == NODEV) return 0;
  auto *properties = OSDictionary::withCapacity(2);
  auto *mjr = OSNumber::withNumber(static_cast<unsigned>(major(device)), 32);
  auto *mnr = OSNumber::withNumber(static_cast<unsigned>(minor(device)), 32);
  if (!properties || !mjr || !mnr) {
    OSSafeReleaseNULL(mnr); OSSafeReleaseNULL(mjr); OSSafeReleaseNULL(properties); return 0;
  }
  bool ready = properties->setObject(kIOBSDMajorKey, mjr) && properties->setObject(kIOBSDMinorKey, mnr);
  OSIterator *iterator = ready ? matchingMedia(properties) : nullptr;
  mjr->release(); mnr->release(); properties->release();
  if (!iterator) return 0;
  unsigned matches = 0; bool valid = false;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    if (++matches != 1) { valid = false; break; }
    PropertySnapshot snapshot = {media->dictionaryWithProperties()};
    if (!snapshot.value) continue;
    auto *uuid = OSDynamicCast(OSString, snapshot.value->getObject("UUID"));
    dev_t observed = NODEV;
    if (!actualDevice(snapshot.value, &observed) || observed != device || !uuid || uuid->getLength() != 36 ||
        (expectedUUID && strcmp(uuid->getCStringNoCopy(), expectedUUID))) continue;
    valid = matchesMedia(media, uuid->getCStringNoCopy(), digest, persistent, rawVolume);
  }
  iterator->release();
  if (!valid || matches != 1) { if (digest) bzero(digest, imageIdentifierBytes); return 0; }
  return 1;
}
}

// Legacy kernel ABI names retained: these now establish device/UUID identity only.
extern "C" int native_verified_block_root_authorized(dev_t device) {
  return volumeIdentityQuery(device, nullptr, nullptr);
}
// Returns a fixed development identifier, without policy I/O or content hashing.
extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]) {
  if (!digest) return 0;
  bzero(digest, 48);
  return uuid && strlen(uuid) == 36 ? volumeIdentityQuery(device, uuid, digest) : 0;
}
static int volumeDeviceStatus(const char *uuid, dev_t *device, char *path, size_t capacity, bool persistent, bool rawVolume = false) {
  if (device) *device = NODEV;
  if (path && capacity) path[0] = 0;
  if (!device || !path || capacity < 6 || !uuid || strlen(uuid) != 36) return IOSW_BLOCK_INVALID;
  auto *properties = OSDictionary::withCapacity(1);
  auto *wanted = OSString::withCString(uuid);
  if (!properties || !wanted) { OSSafeReleaseNULL(wanted); OSSafeReleaseNULL(properties); return IOSW_BLOCK_NOMEM; }
  bool ready = properties->setObject("UUID", wanted);
  OSIterator *iterator = ready ? matchingMedia(properties) : nullptr;
  wanted->release(); properties->release();
  if (!ready) return IOSW_BLOCK_NOMEM;
  if (!iterator) return IOSW_BLOCK_NOT_READY;
  unsigned matches = 0; int status = IOSW_BLOCK_NOT_READY;
  dev_t candidate = NODEV; char candidatePath[64] = {};
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    if (++matches != 1) { status = IOSW_BLOCK_CORRUPT; break; }
    PropertySnapshot snapshot = {media->dictionaryWithProperties()};
    if (!snapshot.value) { status = IOSW_BLOCK_NOMEM; continue; }
    auto *identity = OSDynamicCast(OSString, snapshot.value->getObject("UUID"));
    auto *name = OSDynamicCast(OSString, snapshot.value->getObject(kIOBSDNameKey));
    status = IOSW_BLOCK_CORRUPT;
    if (!identity || identity->getLength() != 36 || strcmp(identity->getCStringNoCopy(), uuid) ||
        !matchesMedia(media, uuid, nullptr, persistent, rawVolume)) continue;
    // IOMedia can publish its UUID before IOBSD adds the device properties.
    if (!snapshot.value->getObject(kIOBSDNameKey) || !snapshot.value->getObject(kIOBSDMajorKey) ||
        !snapshot.value->getObject(kIOBSDMinorKey)) { status = IOSW_BLOCK_NOT_READY; continue; }
    if (!name || !name->getLength() || name->getLength() > sizeof(candidatePath) - 6 ||
        !actualDevice(snapshot.value, &candidate)) continue;
    if (name->getLength() > capacity - 6) { status = IOSW_BLOCK_INVALID; continue; }
    const char *bsd = name->getCStringNoCopy(); bool safe = true;
    for (unsigned i = 0; i < name->getLength(); ++i) {
      char c = bsd[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-')) safe = false;
    }
    if (!safe) continue;
    snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", bsd);
    status = IOSW_BLOCK_OK;
  }
  iterator->release();
  if (status) return status;
  if (matches != 1 || !volumeIdentityQuery(candidate, uuid, nullptr, persistent, rawVolume)) return IOSW_BLOCK_CORRUPT;
  *device = candidate; strlcpy(path, candidatePath, capacity); return IOSW_BLOCK_OK;
}
static int volumeDeviceQuery(const char *uuid, dev_t *device, char *path, size_t capacity, bool persistent) {
  return volumeDeviceStatus(uuid, device, path, capacity, persistent) == IOSW_BLOCK_OK;
}
extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t capacity) {
  return volumeDeviceQuery(uuid, device, path, capacity, false);
}
extern "C" int ioswitch_data_volume_status(const char *uuid, dev_t *device,
    char *path, size_t capacity, uint64_t *bytes) {
  if (device) *device = NODEV;
  if (path && capacity) path[0] = 0;
  if (bytes) *bytes = 0;
  if (!uuid || strcmp(uuid, dataUUID) || !device || !path || !bytes || capacity < 6)
    return IOSW_BLOCK_INVALID;
  uint32_t state = __atomic_load_n(&publicationState, __ATOMIC_ACQUIRE);
  if (state < 2) return IOSW_BLOCK_NOT_READY;
  if (state != 2 || !publishedOwner || !publishedOwner->healthy(dataDisk)) return IOSW_BLOCK_IO;
  int status = volumeDeviceStatus(uuid, device, path, capacity, true);
  uint64_t logicalBytes = 0;
  if (!publishedOwner->dataCapacity(&logicalBytes)) status = IOSW_BLOCK_IO;
  else if (logicalBytes != dataBytes) status = IOSW_BLOCK_CORRUPT;
  if (status) { *device = NODEV; path[0] = 0; return status; }
  *bytes = logicalBytes;
  return IOSW_BLOCK_OK;
}
extern "C" int ioswitch_raw_volume_status(const char *uuid, dev_t *device,
    char *path, size_t capacity, uint64_t *bytes) {
  if (device) *device = NODEV;
  if (path && capacity) path[0] = 0;
  if (bytes) *bytes = 0;
  if (!uuid || strlen(uuid) != 36 || !device || !path || !bytes || capacity < 6)
    return IOSW_BLOCK_INVALID;
  uint32_t state = __atomic_load_n(&publicationState, __ATOMIC_ACQUIRE);
  if (state < 2) return IOSW_BLOCK_NOT_READY;
  if (state != 2 || !publishedOwner) return IOSW_BLOCK_IO;
  if (!publishedOwner->isRawProfile()) return IOSW_BLOCK_INVALID;
  if (!publishedOwner->healthy(dataDisk)) return IOSW_BLOCK_IO;
  int status = volumeDeviceStatus(uuid, device, path, capacity, false, true);
  uint64_t logicalBytes = 0;
  if (!publishedOwner->dataCapacity(&logicalBytes)) status = IOSW_BLOCK_IO;
  if (status) { *device = NODEV; path[0] = 0; return status; }
  *bytes = logicalBytes;
  return IOSW_BLOCK_OK;
}
extern "C" int ioswitch_data_volume_path(const char *uuid, dev_t *device,
    char *path, size_t capacity, uint64_t *bytes) {
  return ioswitch_data_volume_status(uuid, device, path, capacity, bytes) == IOSW_BLOCK_OK;
}
extern "C" int native_verified_block_os_root_authorized(dev_t actual, dev_t selected) {
  if (actual == NODEV || actual != selected) return 0;
  dev_t expected = NODEV; char path[64];
  return native_verified_block_volume_device(volumeUUIDs[0], &expected, path, sizeof(path)) &&
    actual == expected && volumeIdentityQuery(actual, volumeUUIDs[0], nullptr);
}
extern "C" int ioswitch_block_volume_path(unsigned disk, char *path, unsigned capacity) {
  if (!path || !capacity) return IOSW_BLOCK_INVALID;
  path[0] = 0;
  if (disk >= IOSW_BLOCK_DISK_COUNT) return IOSW_BLOCK_INVALID;
  dev_t device;
  return native_verified_block_volume_device(volumeUUIDs[disk], &device, path, capacity)
    ? IOSW_BLOCK_OK : IOSW_BLOCK_NOT_READY;
}

namespace {
void rawRootRegistrySnapshot(const char *className, unsigned limit)
{
  OSDictionary *matching = IOService::serviceMatching(className);
  OSIterator *iterator = matching ? IOService::getMatchingServices(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (!iterator) { IOLog("raw root: %s snapshot unavailable\n", className); return; }
  unsigned rows = 0;
  while (OSObject *object = iterator->getNextObject()) {
    if (rows == limit) { IOLog("raw root: %s truncated at %u\n", className, limit); break; }
    IOService *service = OSDynamicCast(IOService, object);
    if (!service) continue;
    ++rows;
    PropertySnapshot snapshot = {service->dictionaryWithProperties()};
    IORegistryEntry *provider = service->copyParentEntry(gIOServicePlane);
    OSString *uuid = snapshot.value ? OSDynamicCast(OSString, snapshot.value->getObject("UUID")) : nullptr;
    OSString *bsd = snapshot.value ? OSDynamicCast(OSString, snapshot.value->getObject(kIOBSDNameKey)) : nullptr;
    OSNumber *size = snapshot.value ? OSDynamicCast(OSNumber, snapshot.value->getObject("Size")) : nullptr;
    OSBoolean *whole = snapshot.value ? OSDynamicCast(OSBoolean, snapshot.value->getObject("Whole")) : nullptr;
    OSBoolean *writable = snapshot.value ? OSDynamicCast(OSBoolean, snapshot.value->getObject("Writable")) : nullptr;
    IOLog("raw root: %.31s name=%.31s provider=%.31s active=%u busy=%u UUID=%.36s BSD=%.31s size=%llu whole=%d writable=%d props=%u\n",
      service->getMetaClass()->getClassName(), service->getName(),
      provider ? provider->getMetaClass()->getClassName() : "-", !service->isInactive(),
      service->getBusyState(), uuid ? uuid->getCStringNoCopy() : "-",
      bsd ? bsd->getCStringNoCopy() : "-", size ? size->unsigned64BitValue() : 0,
      whole ? static_cast<int>(whole->isTrue()) : -1,
      writable ? static_cast<int>(writable->isTrue()) : -1, snapshot.value != nullptr);
    OSSafeReleaseNULL(provider);
  }
  IOLog("raw root: %s rows=%u\n", className, rows);
  iterator->release();
}
}

extern "C" void ioswitch_raw_root_wait_diagnostic(void)
{
  static unsigned reported = 0;
  unsigned expected = 0;
  if (ioswitch_files_storage_profile() != IOSW_STORAGE_RAW ||
      !__atomic_compare_exchange_n(&reported, &expected, 1, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
  IOLog("raw root: expected UUID=%s publication=%u children=%u last=%u reads=%llu bytes=%llu failures=%llu writes=%llu writefailures=%llu flushes=%llu\n",
    volumeUUIDs[0], __atomic_load_n(&publicationState, __ATOMIC_ACQUIRE),
    ioswitch_block_devices_published, ioswitch_block_last_status,
    ioswitch_block_read_requests, ioswitch_block_read_bytes, ioswitch_block_read_failures,
    ioswitch_data_write_requests, ioswitch_data_write_failures, ioswitch_data_flush_requests);
  IOLog("raw root: SD error=%llu stage=%llu status=%llx blocked=%llu sectors=%llu lastLBA=%llu ready=%llx DMAstage=%llu DMAerror=%llu\n",
    ioswitch_files_record[5], ioswitch_files_record[13], ioswitch_files_record[12],
    ioswitch_files_record[15], ioswitch_files_record[10], ioswitch_files_record[11],
    ioswitch_files_record[14], ioswitch_dma_record[1], ioswitch_dma_record[2]);
  rawRootRegistrySnapshot("IOMedia", 8);
  rawRootRegistrySnapshot("AppleAPFSContainer", 4);
}
