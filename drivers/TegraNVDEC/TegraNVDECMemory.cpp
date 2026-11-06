// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVDEC's buffer arena. With the SMMU bypassed the engine addresses physical memory, so every buffer it touches
// (bitstream, surfaces, colocated motion, history, status) must be physically contiguous and below 4 GiB. A 3 MiB
// contiguous allocation late in boot fails intermittently once memory is fragmented, so one region is reserved when
// NVDEC comes up (stage 2 on) and handed out in 64 KiB granules, first fit.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include "TegraNVDEC.h"

namespace {
constexpr uint64_t kGranule = 64 * 1024;
IOBufferMemoryDescriptor *gArena;
uint64_t gArenaPA;
uint8_t *gArenaVA;
uint32_t gGranules, gFree;
uint32_t *gUsed;  // one bit per granule
IOLock *gLock;

bool used(uint32_t i)
{
    return gUsed[i / 32] & (1u << (i % 32));
}

void setUsed(uint32_t first, uint32_t count, bool on)
{
    for (uint32_t i = first; i < first + count; i++) {
        if (on)
            gUsed[i / 32] |= 1u << (i % 32);
        else
            gUsed[i / 32] &= ~(1u << (i % 32));
    }
}

template <bool Invalidate> void cacheOp(const void *va, size_t size)
{
    if (!size)
        return;
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 15);
    for (uintptr_t a = (uintptr_t)va & ~(line - 1), end = (uintptr_t)va + size; a < end; a += line) {
        if (Invalidate)
            __asm__ volatile("dc civac, %0" ::"r"(a) : "memory");
        else
            __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
}  // namespace

extern "C" void tegra_nvdec_cache_clean(const void *va, size_t size)
{
    cacheOp<false>(va, size);
}

extern "C" void tegra_nvdec_cache_flush(const void *va, size_t size)
{
    cacheOp<true>(va, size);
}

extern "C" int tegra_nvdec_arena_init(uint64_t bytes)
{
    if (gArena)
        return 0;
    bytes = (bytes + kGranule - 1) & ~(kGranule - 1);
    gLock = IOLockAlloc();
    const IOOptionBits options = (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous |
                                 (IOOptionBits)kIOMemoryMapperNone | (IOOptionBits)kIOMemoryKernelUserShared;
    IOBufferMemoryDescriptor *arena =
        gLock ? IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, bytes, 0xffff0000ULL) : nullptr;
    IOByteCount contiguous = 0;
    uint64_t pa = 0;
    if (arena && arena->prepare() == kIOReturnSuccess)
        pa = arena->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
    if (!pa || contiguous < bytes || pa + bytes > (1ULL << 32)) {
        if (arena)
            arena->release();
        return -12;
    }
    gGranules = (uint32_t)(bytes / kGranule);
    gUsed = IONewZero(uint32_t, (gGranules + 31) / 32);
    if (!gUsed) {
        arena->release();
        return -12;
    }
    gArenaPA = pa;
    gArenaVA = (uint8_t *)arena->getBytesNoCopy();
    gFree = gGranules;
    gArena = arena;
    return 0;
}

extern "C" void tegra_nvdec_arena_release(void)
{
    if (!gArena)
        return;
    gArena->complete();
    gArena->release();
    gArena = nullptr;
    IODelete(gUsed, uint32_t, (gGranules + 31) / 32);
    gUsed = nullptr;
    gGranules = gFree = 0;
}

extern "C" int tegra_nvdec_arena_alloc(uint64_t size, TegraNVDECBlock *block)
{
    if (!gArena || !size || size > (uint64_t)gGranules * kGranule)
        return -12;
    const uint32_t count = (uint32_t)((size + kGranule - 1) / kGranule);
    IOLockLock(gLock);
    uint32_t run = 0, first = 0;
    for (uint32_t i = 0; i < gGranules && run < count; i++) {
        if (used(i)) {
            run = 0;
        } else {
            if (!run)
                first = i;
            run++;
        }
    }
    if (run < count) {
        IOLockUnlock(gLock);
        return -12;
    }
    setUsed(first, count, true);
    gFree -= count;
    IOLockUnlock(gLock);
    block->first = first;
    block->count = count;
    block->size = (uint64_t)count * kGranule;
    block->pa = gArenaPA + (uint64_t)first * kGranule;
    block->va = gArenaVA + (uint64_t)first * kGranule;
    // Zero through the cacheable kernel mapping and push it out, so no dirty line can later land on engine output.
    bzero(block->va, block->size);
    tegra_nvdec_cache_flush(block->va, block->size);
    return 0;
}

extern "C" void tegra_nvdec_arena_free(const TegraNVDECBlock *block)
{
    if (!gArena || !block->count)
        return;
    IOLockLock(gLock);
    setUsed(block->first, block->count, false);
    gFree += block->count;
    IOLockUnlock(gLock);
}

// A physical-range descriptor, as TegraGM20B hands its pool memory to clients; on RAM the default cache mode maps it
// cacheable.
extern "C" IOMemoryDescriptor *tegra_nvdec_arena_descriptor(const TegraNVDECBlock *block)
{
    IOAddressRange range = {block->pa, block->size};
    return gArena ? IOMemoryDescriptor::withAddressRanges(
                        &range, 1, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryTypePhysical64, TASK_NULL)
                  : nullptr;
}

extern "C" void tegra_nvdec_arena_usage(uint64_t *total, uint64_t *free)
{
    *total = (uint64_t)gGranules * kGranule;
    *free = (uint64_t)gFree * kGranule;
}
