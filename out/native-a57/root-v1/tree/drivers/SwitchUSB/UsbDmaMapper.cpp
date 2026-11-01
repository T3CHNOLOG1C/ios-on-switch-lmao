// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDMAMapper: identity IOMapper with cache maintenance and bouncing; see UsbDmaMapper.h.
#include "UsbDmaMapper.h"
#include "XudcHardware.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <mach/vm_param.h>

extern "C" {
void pmap_set_cache_attributes(ppnum_t pn, unsigned int cacheattr);
ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
}
// osfmk/arm/memory_types.h (not exported to IOKit sources): copy-back, and normal non-cacheable.
constexpr unsigned VM_WIMG_DEFAULT = 0x2, VM_WIMG_WCOMB = 0x6;

namespace {
constexpr uint32_t kPoolBytes = 8u << 20;  // largest burst of in-flight client transfers we bounce
constexpr uint64_t kPage = 4096;           // bounce granule (the kernel's PAGE_SIZE is a variable)
constexpr uint32_t kPoolPages = (uint32_t)(kPoolBytes / kPage);
constexpr uint32_t kMaxMappings = 512;

struct Mapping {
    uint64_t address = 0, length = 0, offset = 0;
    IOMemoryDescriptor *memory = nullptr;
    uint32_t firstPage = 0, pages = 0;  // bounce pages (pages == 0: identity)
    bool copyBack = false, used = false;
};

// Kernel virtual range of an IOBufferMemoryDescriptor's [offset, offset + length): every managed page
// mapped there gets `attribute` (pmap updates all of the page's mappings and syncs the cache).
void setAttribute(IOBufferMemoryDescriptor *buffer, uint64_t offset, uint64_t length, unsigned attribute)
{
    uintptr_t va = reinterpret_cast<uintptr_t>(buffer->getBytesNoCopy()) + offset;
    uintptr_t start = va & ~(uintptr_t)PAGE_MASK, end = (va + length + PAGE_MASK) & ~(uintptr_t)PAGE_MASK;
    for (uintptr_t page = start; page < end; page += PAGE_SIZE) {
        ppnum_t pn = pmap_find_phys(kernel_pmap, page);
        if (pn) pmap_set_cache_attributes(pn, attribute);
    }
    // Lines the cacheable physical aperture may have fetched while the mapping was non-cacheable are
    // stale once it is cacheable again; dropping them is harmless the other way round.
    xudc::cacheFlush(reinterpret_cast<void *>(start), end - start);
}
}

class SwitchDMAMapper : public IOMapper {
    OSDeclareDefaultStructors(SwitchDMAMapper) IOLock *lock = nullptr;
    IOBufferMemoryDescriptor *pool = nullptr;
    uint8_t *poolVA = nullptr;
    uint64_t poolPA = 0;
    uint8_t *used = nullptr;      // kPoolPages entries
    Mapping *mappings = nullptr;  // kMaxMappings entries (the class must stay below 16 KiB)
    const OSMetaClass *controllerBuffer = nullptr;
    bool reportedFull = false;

    int allocatePages(uint32_t count, uint64_t alignment);
    Mapping *slot();
    Mapping *find(uint64_t address);

protected:
    bool initHardware(IOService *provider) override;

public:
    void free() override;
    uint64_t getPageSize(void) const override { return PAGE_SIZE; }
    IOReturn iovmMapMemory(IOMemoryDescriptor *memory, uint64_t descriptorOffset, uint64_t length, uint32_t mapOptions,
                           const IODMAMapSpecification *spec, IODMACommand *command, const IODMAMapPageList *pageList,
                           uint64_t *mapAddress, uint64_t *mapLength) override;
    IOReturn iovmUnmapMemory(IOMemoryDescriptor *memory, IODMACommand *command, uint64_t mapAddress,
                             uint64_t mapLength) override;
    IOReturn iovmInsert(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) override { return kIOReturnUnsupported; }
    uint64_t mapToPhysicalAddress(uint64_t mappedAddress) override { return mappedAddress; }
};
OSDefineMetaClassAndStructors(SwitchDMAMapper, IOMapper)

    bool SwitchDMAMapper::initHardware(IOService *)
{
    lock = IOLockAlloc();
    used = IONewZero(uint8_t, kPoolPages);
    mappings = IONewZero(Mapping, kMaxMappings);
    pool = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        kPoolBytes, 0x00000000fffff000ull);
    uint64_t physical = pool ? pool->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone) : 0;
    if (!lock || !used || !mappings || !pool || !physical || physical + kPoolBytes > (1ull << 32)) {
        IOLog("SwitchDMAMapper: no %u MiB bounce pool below 4 GiB\n", kPoolBytes >> 20);
        return false;
    }
    poolVA = static_cast<uint8_t *>(pool->getBytesNoCopy());
    poolPA = physical;
    setAttribute(pool, 0, kPoolBytes, VM_WIMG_WCOMB);
    const OSSymbol *name = OSSymbol::withCString("AppleUSBHostDMABufferMemoryDescriptor");
    controllerBuffer = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    return true;
}

void SwitchDMAMapper::free()
{
    OSSafeReleaseNULL(pool);  // never happens in practice: the mapper lives for the boot
    if (used) IODeleteData(used, uint8_t, kPoolPages);
    if (mappings) IODelete(mappings, Mapping, kMaxMappings);
    if (lock) IOLockFree(lock);
    IOMapper::free();
}

int SwitchDMAMapper::allocatePages(uint32_t count, uint64_t alignment)
{
    uint32_t step = alignment > kPage ? (uint32_t)(alignment / kPage) : 1;
    for (uint32_t first = 0; first + count <= kPoolPages; first += step) {
        uint32_t n = 0;
        while (n < count && !used[first + n]) ++n;
        if (n == count) {
            for (uint32_t i = 0; i < count; ++i) used[first + i] = 1;
            return (int)first;
        }
    }
    return -1;
}

Mapping *SwitchDMAMapper::slot()
{
    for (uint32_t i = 0; i < kMaxMappings; ++i)
        if (!mappings[i].used) return &mappings[i];
    return nullptr;
}

Mapping *SwitchDMAMapper::find(uint64_t address)
{
    for (uint32_t i = 0; i < kMaxMappings; ++i)
        if (mappings[i].used && mappings[i].address == address) return &mappings[i];
    return nullptr;
}

IOReturn SwitchDMAMapper::iovmMapMemory(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length,
                                        uint32_t mapOptions, const IODMAMapSpecification *spec, IODMACommand *,
                                        const IODMAMapPageList *, uint64_t *mapAddress, uint64_t *mapLength)
{
    if (!memory || !length || !mapAddress || !mapLength) return kIOReturnBadArgument;
    const uint64_t alignment = spec && spec->alignment > 1 ? spec->alignment : 1;
    IOLockLock(lock);
    Mapping *m = slot();
    if (!m) {
        IOLockUnlock(lock);
        IOLog("SwitchDMAMapper: more than %u DMA mappings\n", kMaxMappings);
        return kIOReturnNoResources;
    }

    // Controller structure: physically contiguous by construction (kIOMemoryPhysicallyContiguous without
    // a system mapper); identity, non-cacheable while mapped. One that is not contiguous (or misaligned)
    // must not be bounced: the controller and the CPU share it for its lifetime, so a copy would silently
    // split them. Refused loudly instead (AppleUSBXHCI then fails the allocation and logs its own line).
    IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
    if (buffer && controllerBuffer && memory->getMetaClass() == controllerBuffer) {
        IOByteCount segment = 0;
        uint64_t physical = memory->getPhysicalSegment(offset, &segment, kIOMemoryMapperNone);
        if (!physical || segment < length || (physical & (alignment - 1))) {
            IOLockUnlock(lock);
            IOLog("SwitchDMAMapper: controller buffer not contiguous (%#llx: %llu of %llu bytes, alignment %llu)\n",
                  physical, (uint64_t)segment, length, alignment);
            return kIOReturnNoResources;
        }
        setAttribute(buffer, offset, length, VM_WIMG_WCOMB);
        *m = Mapping{physical, length, offset, memory, 0, 0, false, true};
        memory->retain();
        IOLockUnlock(lock);
        *mapAddress = physical;
        *mapLength = length;
        return kIOReturnSuccess;
    }

    // Anything else: bounce.
    uint32_t pages = (uint32_t)((length + kPage - 1) / kPage);
    int first = pages <= kPoolPages ? allocatePages(pages, alignment) : -1;
    if (first < 0) {
        if (!reportedFull) IOLog("SwitchDMAMapper: bounce pool full (%llu bytes asked)\n", length);
        reportedFull = true;
        IOLockUnlock(lock);
        return kIOReturnNoResources;
    }
    *m = Mapping{poolPA + (uint64_t)first * kPage,         length, offset, memory, (uint32_t)first, pages,
                 (mapOptions & kIODMAMapWriteAccess) != 0, true};
    memory->retain();
    IOLockUnlock(lock);
    uint8_t *bounce = poolVA + (uint64_t)first * kPage;
    if (memory->readBytes(offset, bounce, length) != length) bzero(bounce, length);
    __asm__ volatile("dsb sy" ::: "memory");  // non-cacheable stores reach memory before the doorbell
    *mapAddress = m->address;
    *mapLength = length;
    return kIOReturnSuccess;
}

IOReturn SwitchDMAMapper::iovmUnmapMemory(IOMemoryDescriptor *memory, IODMACommand *, uint64_t mapAddress, uint64_t)
{
    IOLockLock(lock);
    Mapping *m = find(mapAddress);
    if (!m || m->memory != memory) {
        IOLockUnlock(lock);
        return kIOReturnNotFound;
    }
    Mapping done = *m;
    m->used = false;
    IOLockUnlock(lock);

    if (!done.pages) {
        IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, done.memory);
        if (buffer) setAttribute(buffer, done.offset, done.length, VM_WIMG_DEFAULT);
    } else {
        __asm__ volatile("dsb sy" ::: "memory");
        if (done.copyBack) done.memory->writeBytes(done.offset, poolVA + (uint64_t)done.firstPage * kPage, done.length);
        IOLockLock(lock);
        for (uint32_t i = 0; i < done.pages; ++i) used[done.firstPage + i] = 0;
        reportedFull = false;
        IOLockUnlock(lock);
    }
    done.memory->release();
    return kIOReturnSuccess;
}

IOMapper *switch_usb_dma_mapper(void)
{
    static SwitchDMAMapper *mapper;
    static uint32_t starting;
    if (__atomic_load_n(&mapper, __ATOMIC_ACQUIRE)) return mapper;
    if (__atomic_exchange_n(&starting, 1, __ATOMIC_ACQ_REL)) {
        while (!__atomic_load_n(&mapper, __ATOMIC_ACQUIRE)) IOSleep(1);
        return mapper;
    }
    IOService *resources = IOService::getResourceService();
    SwitchDMAMapper *candidate = OSTypeAlloc(SwitchDMAMapper);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources) ||
        !candidate->start(resources)) {
        IOLog("SwitchDMAMapper: setup failed\n");
        if (candidate) {
            candidate->detach(resources);
            candidate->release();
        }
        __atomic_store_n(&starting, 0, __ATOMIC_RELEASE);
        return nullptr;
    }
    candidate->setName("SwitchDMAMapper");
    __atomic_store_n(&mapper, candidate, __ATOMIC_RELEASE);
    return candidate;
}
