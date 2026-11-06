// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVENC's buffer arena. With the SMMU bypassed the engine addresses physical memory, so every buffer it touches must be
// physically contiguous and below 4 GiB. Unlike NVDEC's one region reserved at boot, this arena starts with one small
// chunk (E0: 2 MiB, which holds E3's buffers and then the setup slots and scratch) and grows by contiguous chunks
// when a session's buffers do not fit, up to kMaxChunks / kMaxBytes. Chunks are kept once added: a session that ends
// leaves its memory for the next, and nothing is re-fragmented. Allocation is first fit in 64 KiB granules across
// the chunks.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include "TegraNVENC.h"

namespace {
constexpr uint64_t kGranule = 64 * 1024, kGrowChunk = 8ull << 20, kMaxBytes = 96ull << 20;
constexpr uint32_t kMaxChunks = 16;

struct Chunk {
    IOBufferMemoryDescriptor *md;
    uint64_t pa;
    uint8_t *va;
    uint32_t granules, free;
    uint32_t *used;  // one bit per granule
};

Chunk gChunks[kMaxChunks];
uint32_t gCount;
uint64_t gTotal;
IOLock *gLock;

bool used(const Chunk &c, uint32_t i)
{
    return c.used[i / 32] & (1u << (i % 32));
}

void setUsed(Chunk &c, uint32_t first, uint32_t count, bool on)
{
    for (uint32_t i = first; i < first + count; i++) {
        if (on)
            c.used[i / 32] |= 1u << (i % 32);
        else
            c.used[i / 32] &= ~(1u << (i % 32));
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

// One more chunk (gLock held). Fails quietly; the caller reports.
bool addChunk(uint64_t bytes)
{
    bytes = (bytes + kGranule - 1) & ~(kGranule - 1);
    if (gCount == kMaxChunks || gTotal + bytes > kMaxBytes)
        return false;
    const IOOptionBits options = (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous |
                                 (IOOptionBits)kIOMemoryMapperNone | (IOOptionBits)kIOMemoryKernelUserShared;
    IOBufferMemoryDescriptor *md =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, bytes, 0xffff0000ULL);
    IOByteCount contiguous = 0;
    uint64_t pa = 0;
    if (md && md->prepare() == kIOReturnSuccess)
        pa = md->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
    if (!pa || contiguous < bytes || pa + bytes > (1ULL << 32)) {
        if (md)
            md->release();
        return false;
    }
    Chunk &c = gChunks[gCount];
    c.granules = (uint32_t)(bytes / kGranule);
    c.used = IONewZero(uint32_t, (c.granules + 31) / 32);
    if (!c.used) {
        md->release();
        return false;
    }
    c.md = md;
    c.pa = pa;
    c.va = (uint8_t *)md->getBytesNoCopy();
    c.free = c.granules;
    gTotal += bytes;
    gCount++;
    return true;
}

bool fit(uint32_t chunk, uint32_t count, uint32_t *first)
{
    const Chunk &c = gChunks[chunk];
    uint32_t run = 0;
    for (uint32_t i = 0; i < c.granules && run < count; i++) {
        if (used(c, i)) {
            run = 0;
        } else {
            if (!run)
                *first = i;
            run++;
        }
    }
    return run == count;
}
}  // namespace

extern "C" void tegra_nvenc_cache_clean(const void *va, size_t size)
{
    cacheOp<false>(va, size);
}

extern "C" void tegra_nvenc_cache_flush(const void *va, size_t size)
{
    cacheOp<true>(va, size);
}

extern "C" int tegra_nvenc_arena_init(uint64_t bytes)
{
    if (!gLock && !(gLock = IOLockAlloc()))
        return -12;
    IOLockLock(gLock);
    const bool ok = gCount || addChunk(bytes);
    IOLockUnlock(gLock);
    return ok ? 0 : -12;
}

extern "C" void tegra_nvenc_arena_release(void)
{
    if (!gLock)
        return;
    IOLockLock(gLock);
    for (uint32_t i = 0; i < gCount; i++) {
        gChunks[i].md->complete();
        gChunks[i].md->release();
        IODelete(gChunks[i].used, uint32_t, (gChunks[i].granules + 31) / 32);
        gChunks[i] = Chunk{};
    }
    gCount = 0;
    gTotal = 0;
    IOLockUnlock(gLock);
}

extern "C" int tegra_nvenc_arena_alloc(uint64_t size, TegraNVENCBlock *block)
{
    if (!gLock || !size || size > kMaxBytes)
        return -12;
    const uint32_t count = (uint32_t)((size + kGranule - 1) / kGranule);
    IOLockLock(gLock);
    uint32_t chunk = 0, first = 0;
    while (chunk < gCount && !fit(chunk, count, &first))
        chunk++;
    if (chunk == gCount) {  // a new chunk, at least the usual size and big enough for this buffer
        const uint64_t bytes = (uint64_t)count * kGranule > kGrowChunk ? (uint64_t)count * kGranule : kGrowChunk;
        if (!gCount || !addChunk(bytes) || !fit(chunk, count, &first)) {
            IOLockUnlock(gLock);
            return -12;
        }
    }
    Chunk &c = gChunks[chunk];
    setUsed(c, first, count, true);
    c.free -= count;
    IOLockUnlock(gLock);
    block->chunk = chunk;
    block->first = first;
    block->count = count;
    block->size = (uint64_t)count * kGranule;
    block->pa = c.pa + (uint64_t)first * kGranule;
    block->va = c.va + (uint64_t)first * kGranule;
    // Zero through the cacheable kernel mapping and push it out, so no dirty line can later land on engine output.
    bzero(block->va, block->size);
    tegra_nvenc_cache_flush(block->va, block->size);
    return 0;
}

extern "C" void tegra_nvenc_arena_free(const TegraNVENCBlock *block)
{
    if (!gLock || !block->count)
        return;
    IOLockLock(gLock);
    if (block->chunk < gCount) {
        setUsed(gChunks[block->chunk], block->first, block->count, false);
        gChunks[block->chunk].free += block->count;
    }
    IOLockUnlock(gLock);
}

// A physical-range descriptor, as TegraNVDEC hands out its arena; on RAM the default cache mode maps it cacheable.
extern "C" IOMemoryDescriptor *tegra_nvenc_arena_descriptor(const TegraNVENCBlock *block)
{
    IOAddressRange range = {block->pa, block->size};
    return gCount ? IOMemoryDescriptor::withAddressRanges(
                        &range, 1, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryTypePhysical64, TASK_NULL)
                  : nullptr;
}

extern "C" void tegra_nvenc_arena_usage(uint64_t *total, uint64_t *free)
{
    uint64_t f = 0;
    if (gLock)
        IOLockLock(gLock);
    for (uint32_t i = 0; i < gCount; i++)
        f += (uint64_t)gChunks[i].free * kGranule;
    *total = gTotal;
    if (gLock)
        IOLockUnlock(gLock);
    *free = f;
}
