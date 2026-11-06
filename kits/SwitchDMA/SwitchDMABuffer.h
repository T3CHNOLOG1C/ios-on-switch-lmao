// SPDX-License-Identifier: BSD-2-Clause-Patent
// One physically contiguous, wired, cacheable kernel buffer for a Tegra DMA master, below the limit its mask sets.
#ifndef SWITCH_DMA_BUFFER_H
#define SWITCH_DMA_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

// IOBufferMemoryDescriptor::inTaskWithPhysicalMask's mask: the highest set bit is the address ceiling, the clear low
// bits the alignment. Most Tegra masters take 32-bit addresses.
constexpr uint64_t kSwitchDMABelow4G = 0x00000000fffff000ULL;

struct switch_dma_buffer {
    IOBufferMemoryDescriptor *memory;
    uint8_t *va;
    uint64_t pa;
    size_t size;
};

// Zeroed and cleaned+invalidated on return, so the device sees zeros and no dirty line is evicted over its writes.
// extraOptions adds IOKit options (kIOMapWriteCombineCache, kIOMemoryKernelUserShared). Thread context.
bool switch_dma_alloc(switch_dma_buffer *out, size_t size, uint64_t physicalMask = kSwitchDMABelow4G,
                      IOOptionBits extraOptions = 0);
void switch_dma_free(switch_dma_buffer *buffer);

#endif
