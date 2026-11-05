// Host stand-ins for the IOKit/libkern interfaces native/storage/NativeIOSwitchBlock.cpp uses, for
// native_block_test.cpp (BUILD.bazel points <IOKit/...> and <libkern/c++/...> here): reference-counted objects that
// count what is live, a lock that checks its own discipline, a registry of published services, and failure injection
// for every allocation, attach, start, property and IOMalloc.
#ifndef IOSWITCH_TEST_IOKIT_STUBS_H
#define IOSWITCH_TEST_IOKIT_STUBS_H
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "bazel/testing/require.h"

// A stand-in's invariant (lock discipline, ownership): printed at once, since it may fail on a worker thread where
// nothing else would report it, then REQUIRE (never compiled out under NDEBUG).
#define STUB_REQUIRE(...)                                                                                              \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            std::fprintf(stderr, "%s:%d: stand-in invariant failed: %s\n", __FILE__, __LINE__, #__VA_ARGS__);         \
            REQUIRE(__VA_ARGS__);                                                                                      \
        }                                                                                                              \
    } while (0)

using UInt64 = uint64_t;
using UInt32 = uint32_t;
using IOOptionBits = uint32_t;
using IOStorageUnmapOptions = uint32_t;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnUnsupported = -1,
 kIOReturnNotWritable = -2, kIOReturnBadArgument = -3, kIOReturnNotReady = -4,
 kIOReturnTimeout = -5, kIOReturnNoMemory = -6, kIOReturnIOError = -7;
constexpr unsigned kIODirectionIn = 1, kIODirectionOut = 2, kIODirectionInOut = 3;
#define kIOBSDNameKey "BSD Name"
#define kIOBSDMajorKey "BSD Major"
#define kIOBSDMinorKey "BSD Minor"
#define kIOMaximumBlockCountReadKey "IOMaximumBlockCountRead"
#define kIOMaximumByteCountReadKey "IOMaximumByteCountRead"
#define OSDeclareDefaultStructors(cls)
#define OSDefineMetaClassAndStructors(cls, super)
#define OSDynamicCast(cls, obj) dynamic_cast<cls *>(obj)
#define OSSafeReleaseNULL(obj) do { if (obj) { obj->release(); obj = nullptr; } } while (0)
inline unsigned allocationCalls = 0, failAllocationCall = 0;
inline unsigned attachCalls = 0, failAttachCall = 0, startCalls = 0, failStartCall = 0;
inline unsigned propertyCalls = 0, failPropertyCall = 0, superclassStops = 0;
inline unsigned mallocCalls = 0, failMallocCall = 0, mallocLive = 0;
template<class T> T *allocateType() {
 if (++allocationCalls == failAllocationCall) return nullptr;
 return new T;
}
#define OSTypeAlloc(cls) allocateType<cls>()
inline thread_local unsigned heldLocks = 0;
struct IOLock { std::mutex lock; };
inline IOLock *IOLockAlloc() { return new IOLock; }
inline void IOLockFree(IOLock *lock) { STUB_REQUIRE(!heldLocks); delete lock; }
// IOLockLock/IOLockUnlock take and drop the lock across calls, as IOKit's do (clang's -Wthread-safety, which the host
// toolchain turns on, cannot follow that).
inline void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis)) { STUB_REQUIRE(!heldLocks); lock->lock.lock(); ++heldLocks; }
inline void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis)) { STUB_REQUIRE(heldLocks == 1); --heldLocks; lock->lock.unlock(); }
inline void *IOMalloc(size_t bytes) {
 if (++mallocCalls == failMallocCall) return nullptr;
 void *p = std::malloc(bytes); if(p) ++mallocLive; return p;
}
inline void IOFree(void *p, size_t) { if(p) { STUB_REQUIRE(mallocLive); --mallocLive; std::free(p); } }
class OSObject {
 std::atomic<unsigned> refs{1};
public:
 inline static std::atomic<unsigned> live{0};
 OSObject() { ++live; }
 virtual ~OSObject() { --live; }
 virtual void free() {}
 void retain() { ++refs; }
 void release() { if(--refs == 0) { free(); delete this; } }
};
class OSString : public OSObject {
 std::string value;
public:
 explicit OSString(const char *s):value(s) {}
 static OSString *withCString(const char *s) { return new OSString(s); }
 const char *getCStringNoCopy() const { return value.c_str(); }
 unsigned getLength() const { return static_cast<unsigned>(value.size()); }
};
class OSNumber : public OSObject {
 uint64_t value;
public:
 explicit OSNumber(uint64_t n):value(n) {}
 static OSNumber *withNumber(uint64_t n,unsigned) { return new OSNumber(n); }
 uint64_t unsigned64BitValue() const { return value; }
};
class OSDictionary : public OSObject {
public:
 std::map<std::string,OSObject *> items;
 static OSDictionary *withCapacity(unsigned) { return new OSDictionary; }
 bool setObject(const char *key,OSObject *value) {
  if (!value) return false;
  value->retain(); auto &old=items[key]; if(old) old->release(); old=value; return true;
 }
 OSObject *getObject(const char *key) { auto it=items.find(key); return it==items.end()?nullptr:it->second; }
 void free() override { for(auto &item:items) item.second->release(); items.clear(); }
};
class OSIterator : public OSObject {
 std::vector<OSObject *> items; size_t index=0;
public:
 explicit OSIterator(const std::vector<OSObject *>&v):items(v) { for(auto *o:items)o->retain(); }
 OSObject *getNextObject() { return index<items.size()?items[index++]:nullptr; }
 void free() override { for(auto *o:items)o->release(); items.clear(); }
};
class IOService : public OSObject {
 IOService *provider=nullptr;
 OSDictionary *properties=nullptr;
public:
 inline static IOService *resource=nullptr;
 inline static std::vector<IOService *> registered;
 inline static std::vector<IOService *> candidates;
 bool inactive=false;
 virtual bool init(OSDictionary *) { return true; }
 virtual bool start(IOService *) { return ++startCalls != failStartCall; }
 virtual void stop(IOService *) { ++superclassStops; }
 virtual bool attach(IOService *parent) { if(++attachCalls==failAttachCall)return false; provider=parent;return true; }
 virtual void detach(IOService *parent) { STUB_REQUIRE(provider==parent); provider=nullptr; }
 virtual void registerService(IOOptionBits=0) { retain(); registered.push_back(this); }
 virtual IOService *getProvider() { return provider; }
 IOService *copyParentEntry(const void *) { if(provider)provider->retain(); return provider; }
 OSDictionary *dictionaryWithProperties() {
  auto *copy=OSDictionary::withCapacity(2);
  if(properties)for(auto &item:properties->items)copy->setObject(item.first.c_str(),item.second);
  return copy;
 }
 virtual bool isInactive() const { return inactive; }
 bool setProperty(const char *key, uint64_t value,unsigned bits) {
  if(++propertyCalls==failPropertyCall)return false;
  auto *n=OSNumber::withNumber(value,bits); bool result=setProperty(key,n); n->release(); return result;
 }
 bool setProperty(const char *key,OSObject *value) {
  if(!properties) properties=OSDictionary::withCapacity(2);
  return properties->setObject(key,value);
 }
 OSObject *getProperty(const char *key) { return properties?properties->getObject(key):nullptr; }
 void setName(const char *) {}
 static IOService *getResourceService() { return resource; }
 static OSDictionary *serviceMatching(const char *) { return OSDictionary::withCapacity(1); }
 static OSIterator *getMatchingServices(OSDictionary *dict) {
  auto *props=dynamic_cast<OSDictionary *>(dict->getObject("IOPropertyMatch"));
  auto *uuid=props?dynamic_cast<OSString *>(props->getObject("UUID")):nullptr;
  STUB_REQUIRE(uuid); std::vector<OSObject *> matches;
  for(auto *media:candidates) {
   auto *identity=dynamic_cast<OSString *>(media->getProperty("UUID"));
   if(identity && !std::strcmp(identity->getCStringNoCopy(),uuid->getCStringNoCopy())) matches.push_back(media);
  }
  return new OSIterator(matches);
 }
 void free() override { if(properties)properties->release(); properties=nullptr; OSObject::free(); }
};
using IORegistryEntry = IOService;
inline const void *gIOServicePlane = nullptr;
struct IOStorageAttributes {};
struct IOBlockStorageDeviceExtent {};
struct IOStorageCompletion {
 void *target;
 void (*action)(void *,void *,IOReturn,UInt64);
 void *parameter;
};
class IOMemoryDescriptor {
public:
 std::vector<uint8_t> bytes;
 unsigned direction;
 uint64_t copyLimit=UINT64_MAX;
 explicit IOMemoryDescriptor(size_t n,unsigned d=kIODirectionIn):bytes(n,0xa5),direction(d) {}
 uint64_t getLength() const { return bytes.size(); }
 unsigned getDirection() const { return direction; }
 uint64_t writeBytes(uint64_t off,const void *src,uint64_t count) {
  STUB_REQUIRE(heldLocks==1); uint64_t n=std::min(count,copyLimit);
  STUB_REQUIRE(off<=bytes.size() && n<=bytes.size()-off); std::memcpy(bytes.data()+off,src,n); return n;
 }
};
class IOBlockStorageDevice : public IOService {
public:
 virtual IOReturn doEjectMedia()=0;
 virtual IOReturn doFormatMedia(UInt64)=0;
 virtual UInt32 doGetFormatCapacities(UInt64 *,UInt32) const=0;
 virtual IOReturn doUnmap(IOBlockStorageDeviceExtent *,UInt32,IOStorageUnmapOptions) {return 0;}
 virtual char *getVendorString()=0;
 virtual char *getProductString()=0;
 virtual char *getRevisionString()=0;
 virtual char *getAdditionalDeviceInfoString()=0;
 virtual IOReturn reportBlockSize(UInt64 *)=0;
 virtual IOReturn reportMaxValidBlock(UInt64 *)=0;
 virtual IOReturn reportEjectability(bool *)=0;
 virtual IOReturn reportRemovability(bool *)=0;
 virtual IOReturn reportWriteProtection(bool *)=0;
 virtual IOReturn reportMediaState(bool *,bool *)=0;
 virtual IOReturn doSynchronizeCache() {return 0;}
 virtual IOReturn doSynchronize(UInt64,UInt64,IOOptionBits) {return 0;}
 virtual IOReturn getWriteCacheState(bool *) {return 0;}
 virtual IOReturn setWriteCacheState(bool) {return 0;}
 virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *,UInt64,UInt64,IOStorageAttributes *,IOStorageCompletion *)=0;
};
#endif
