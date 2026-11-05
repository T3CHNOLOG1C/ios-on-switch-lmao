#ifndef IOSWITCH_RAM_TEST_IOKIT_STUBS_H
#define IOSWITCH_RAM_TEST_IOKIT_STUBS_H
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <CommonCrypto/CommonDigest.h>
#include <sys/types.h>
#include <sys/param.h>
#define SHA256_CTX    CC_SHA256_CTX
#define SHA256_Init   CC_SHA256_Init
#define SHA256_Update CC_SHA256_Update
#define SHA256_Final  CC_SHA256_Final
using UInt64 = uint64_t;
using UInt32 = uint32_t;
using IOOptionBits = uint32_t;
using IOStorageUnmapOptions = uint32_t;
using IOReturn = int;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnUnsupported = -1, kIOReturnNotWritable = -2,
                   kIOReturnBadArgument = -3, kIOReturnNotReady = -4, kIOReturnTimeout = -5, kIOReturnNoMemory = -6,
                   kIOReturnIOError = -7;
constexpr unsigned kIODirectionIn = 1, kIODirectionOut = 2, kIODirectionInOut = 3;
#define kIOBSDNameKey                "BSD Name"
#define kIOBSDMajorKey               "BSD Major"
#define kIOBSDMinorKey               "BSD Minor"
#define kIOMaximumBlockCountReadKey  "IOMaximumBlockCountRead"
#define kIOMaximumByteCountReadKey   "IOMaximumByteCountRead"
#define kIOMaximumBlockCountWriteKey "IOMaximumBlockCountWrite"
#define kIOMaximumByteCountWriteKey  "IOMaximumByteCountWrite"
#define OSDeclareDefaultStructors(cls)
#define OSDefineMetaClassAndStructors(cls, super)
#define OSDynamicCast(cls, obj) dynamic_cast<cls *>(obj)
#define OSSafeReleaseNULL(obj) \
    do {                       \
        if (obj) {             \
            obj->release();    \
            obj = nullptr;     \
        }                      \
    } while (0)
inline unsigned allocationCalls = 0, failAllocationCall = 0;
inline unsigned attachCalls = 0, failAttachCall = 0, startCalls = 0, failStartCall = 0;
inline unsigned propertyCalls = 0, failPropertyCall = 0, superclassStops = 0;
inline unsigned mallocCalls = 0, failMallocCall = 0, mallocLive = 0;
inline unsigned lockCalls = 0, failLockCall = 0, lockLive = 0;
inline std::map<void *, size_t> allocations;
template <class T> T *allocateType()
{
    if (++allocationCalls == failAllocationCall) return nullptr;
    return new T;
}
#define OSTypeAlloc(cls) allocateType<cls>()
inline thread_local unsigned heldLocks = 0;
struct IOLock {
    std::mutex lock;
};
inline IOLock *IOLockAlloc()
{
    if (++lockCalls == failLockCall) return nullptr;
    ++lockLive;
    return new IOLock;
}
inline void IOLockFree(IOLock *lock)
{
    assert(!heldLocks && lockLive);
    --lockLive;
    delete lock;
}
inline void IOLockLock(IOLock *lock)
{
    assert(!heldLocks);
    lock->lock.lock();
    ++heldLocks;
}
inline void IOLockUnlock(IOLock *lock)
{
    assert(heldLocks == 1);
    --heldLocks;
    lock->lock.unlock();
}
inline void *IOMalloc(size_t bytes)
{
    if (++mallocCalls == failMallocCall) return nullptr;
    void *p = std::malloc(bytes);
    if (p) {
        ++mallocLive;
        allocations[p] = bytes;
    }
    return p;
}
inline void *IOMallocZero(size_t bytes)
{
    void *p = IOMalloc(bytes);
    if (p) std::memset(p, 0, bytes);
    return p;
}
inline void IOFree(void *p, size_t bytes)
{
    if (p) {
        assert(mallocLive && allocations.at(p) == bytes);
        allocations.erase(p);
        --mallocLive;
        std::free(p);
    }
}
class OSObject {
    std::atomic<unsigned> refs{1};

public:
    inline static std::atomic<unsigned> live{0};
    OSObject() { ++live; }
    virtual ~OSObject() { --live; }
    virtual void free() {}
    void retain() { ++refs; }
    void release()
    {
        if (--refs == 0) {
            free();
            delete this;
        }
    }
};
class OSData : public OSObject {
    std::vector<uint8_t> bytes;

public:
    OSData(const void *p, size_t n) : bytes(static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n) {}
    static OSData *withBytes(const void *p, size_t n) { return new OSData(p, n); }
    unsigned getLength() const { return bytes.size(); }
    const void *getBytesNoCopy() const { return bytes.data(); }
};
class OSString : public OSObject {
    std::string value;

public:
    explicit OSString(const char *s) : value(s) {}
    static OSString *withCString(const char *s) { return new OSString(s); }
    const char *getCStringNoCopy() const { return value.c_str(); }
    unsigned getLength() const { return static_cast<unsigned>(value.size()); }
};
class OSNumber : public OSObject {
    uint64_t value;

public:
    explicit OSNumber(uint64_t n) : value(n) {}
    static OSNumber *withNumber(uint64_t n, unsigned) { return new OSNumber(n); }
    uint64_t unsigned64BitValue() const { return value; }
};
class OSDictionary : public OSObject {
public:
    std::map<std::string, OSObject *> items;
    static OSDictionary *withCapacity(unsigned) { return new OSDictionary; }
    bool setObject(const char *key, OSObject *value)
    {
        if (!value) return false;
        value->retain();
        auto &old = items[key];
        if (old) old->release();
        old = value;
        return true;
    }
    OSObject *getObject(const char *key)
    {
        auto it = items.find(key);
        return it == items.end() ? nullptr : it->second;
    }
    void free() override
    {
        for (auto &item : items) item.second->release();
        items.clear();
    }
};
class OSIterator : public OSObject {
    std::vector<OSObject *> items;
    size_t index = 0;

public:
    explicit OSIterator(const std::vector<OSObject *> &v) : items(v)
    {
        for (auto *o : items) o->retain();
    }
    OSObject *getNextObject() { return index < items.size() ? items[index++] : nullptr; }
    void free() override
    {
        for (auto *o : items) o->release();
        items.clear();
    }
};
class IOService : public OSObject {
    IOService *provider = nullptr;
    OSDictionary *properties = nullptr;

public:
    inline static IOService *resource = nullptr;
    inline static IOService *memoryMap = nullptr;
    inline static unsigned nativeSeedLookups = 0, legacySeedLookups = 0;
    static IOService *fromPath(const char *path, const void *)
    {
        assert(!strcmp(path, "/chosen/memory-map"));
        if (memoryMap) memoryMap->retain();
        return memoryMap;
    }
    inline static std::vector<IOService *> registered;
    inline static std::vector<IOService *> candidates;
    bool inactive = false;
    virtual bool init(OSDictionary *) { return true; }
    virtual bool start(IOService *) { return ++startCalls != failStartCall; }
    virtual void stop(IOService *) { ++superclassStops; }
    virtual bool attach(IOService *parent)
    {
        if (++attachCalls == failAttachCall) return false;
        provider = parent;
        return true;
    }
    virtual void detach(IOService *parent)
    {
        assert(provider == parent);
        provider = nullptr;
    }
    virtual void registerService(IOOptionBits = 0)
    {
        retain();
        registered.push_back(this);
    }
    virtual IOService *getProvider() { return provider; }
    IOService *copyParentEntry(const void *)
    {
        if (provider) provider->retain();
        return provider;
    }
    OSDictionary *dictionaryWithProperties()
    {
        auto *copy = OSDictionary::withCapacity(2);
        if (properties)
            for (auto &item : properties->items) copy->setObject(item.first.c_str(), item.second);
        return copy;
    }
    virtual bool isInactive() const { return inactive; }
    bool setProperty(const char *key, uint64_t value, unsigned bits)
    {
        if (++propertyCalls == failPropertyCall) return false;
        auto *n = OSNumber::withNumber(value, bits);
        bool result = setProperty(key, n);
        n->release();
        return result;
    }
    bool setProperty(const char *key, OSObject *value)
    {
        if (!properties) properties = OSDictionary::withCapacity(2);
        return properties->setObject(key, value);
    }
    OSObject *getProperty(const char *key)
    {
        if (!strcmp(key, "NativeDataSeed")) ++nativeSeedLookups;
        if (!strcmp(key, "RAMDisk") || !strcmp(key, "PackedRAMDisk")) ++legacySeedLookups;
        return properties ? properties->getObject(key) : nullptr;
    }
    void setName(const char *) {}
    static IOService *getResourceService() { return resource; }
    static OSDictionary *serviceMatching(const char *) { return OSDictionary::withCapacity(1); }
    static OSIterator *getMatchingServices(OSDictionary *dict)
    {
        auto *props = dynamic_cast<OSDictionary *>(dict->getObject("IOPropertyMatch"));
        auto *uuid = props ? dynamic_cast<OSString *>(props->getObject("UUID")) : nullptr;
        assert(uuid);
        std::vector<OSObject *> matches;
        for (auto *media : candidates) {
            auto *identity = dynamic_cast<OSString *>(media->getProperty("UUID"));
            if (identity && !std::strcmp(identity->getCStringNoCopy(), uuid->getCStringNoCopy()))
                matches.push_back(media);
        }
        return new OSIterator(matches);
    }
    void free() override
    {
        if (properties) properties->release();
        properties = nullptr;
        OSObject::free();
    }
};
using IORegistryEntry = IOService;
inline const void *gIOServicePlane = nullptr;
inline const void *gIODTPlane = nullptr;
struct boot_args {
    uint64_t topOfKernelData;
};
struct {
    void *bootArgs;
} inline PE_state;
inline std::vector<uint8_t> mappedSeed;
inline uintptr_t ml_static_ptovirt(uint64_t physical)
{
    return physical == 0x80020000ULL ? reinterpret_cast<uintptr_t>(mappedSeed.data()) : 0;
}
struct IOStorageAttributes {};
struct IOBlockStorageDeviceExtent {};
struct IOStorageCompletion {
    void *target;
    void (*action)(void *, void *, IOReturn, UInt64);
    void *parameter;
};
class IOMemoryDescriptor {
public:
    std::vector<uint8_t> bytes;
    unsigned direction;
    uint64_t copyLimit = UINT64_MAX;
    explicit IOMemoryDescriptor(size_t n, unsigned d = kIODirectionIn) : bytes(n, 0xa5), direction(d) {}
    uint64_t getLength() const { return bytes.size(); }
    unsigned getDirection() const { return direction; }
    uint64_t writeBytes(uint64_t off, const void *src, uint64_t count)
    {
        assert(heldLocks == 1);
        uint64_t n = off < copyLimit ? std::min(count, copyLimit - off) : 0;
        assert(off <= bytes.size() && n <= bytes.size() - off);
        std::memcpy(bytes.data() + off, src, n);
        return n;
    }
    uint64_t readBytes(uint64_t off, void *dst, uint64_t count)
    {
        assert(heldLocks == 1);
        uint64_t n = off < copyLimit ? std::min(count, copyLimit - off) : 0;
        assert(off <= bytes.size() && n <= bytes.size() - off);
        std::memcpy(dst, bytes.data() + off, n);
        return n;
    }
};
class IOBlockStorageDevice : public IOService {
public:
    virtual IOReturn doEjectMedia() = 0;
    virtual IOReturn doFormatMedia(UInt64) = 0;
    virtual UInt32 doGetFormatCapacities(UInt64 *, UInt32) const = 0;
    virtual IOReturn doUnmap(IOBlockStorageDeviceExtent *, UInt32, IOStorageUnmapOptions) { return 0; }
    virtual char *getVendorString() = 0;
    virtual char *getProductString() = 0;
    virtual char *getRevisionString() = 0;
    virtual char *getAdditionalDeviceInfoString() = 0;
    virtual IOReturn reportBlockSize(UInt64 *) = 0;
    virtual IOReturn reportMaxValidBlock(UInt64 *) = 0;
    virtual IOReturn reportEjectability(bool *) = 0;
    virtual IOReturn reportRemovability(bool *) = 0;
    virtual IOReturn reportWriteProtection(bool *) = 0;
    virtual IOReturn reportMediaState(bool *, bool *) = 0;
    virtual IOReturn doSynchronizeCache() { return 0; }
    virtual IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) { return 0; }
    virtual IOReturn getWriteCacheState(bool *) { return 0; }
    virtual IOReturn setWriteCacheState(bool) { return 0; }
    virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *,
                                      IOStorageCompletion *) = 0;
};
#endif
