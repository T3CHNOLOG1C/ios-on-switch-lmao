// SPDX-License-Identifier: BSD-2-Clause-Patent
// A DMA request's physical runs without IOMemoryDescriptor::prepare(), for memory that is wired for as long as it is
// allocated (zone memory: mbufs and their clusters). No IOKit: test/switch_dma_direct_test.cpp drives the same code on
// the host; SwitchDMADirectMemory.h binds it to IOMemoryDescriptor in the kernel. Bound for kits/SwitchDMA.
//
// prepare() on a kernel-virtual descriptor (IOMemoryDescriptor::withAddressRanges over mbufs, as AppleUSBDeviceMux
// queues them) creates a UPL per range through vm_map_create_upl and commits it in complete(): page lookups and wiring
// under the VM object and page-queue locks, for memory that is already wired. Here the runs come from the page tables
// (one translation per 4 KiB); they are the maximal physically contiguous runs in request order, which is what the
// prepared descriptor's getPhysicalSegment walk gives.
#pragma once
#include <stdint.h>

constexpr uint32_t kSwitchDmaTranslateBytes = 4096;

// The longest virtually contiguous part of the request from an offset (getPhysicalSegment with
// _kIOMemorySourceSegment).
struct switch_dma_span {
    uint64_t va;
    uint64_t length;
};

struct switch_dma_run {
    uint64_t pa;
    uint32_t length;
};

// The runs (any struct with .pa and .length) of a request of `total` bytes whose memory `next(offset, &span)` describes
// (false: no span there), when every span is wired zone memory (`wired(va, length)`) and every 4 KiB of it translates
// (`toPhys(va)`, 0: no mapping). Returns the run count, or 0 when the request does not qualify (a span that is not zone
// memory, a page with no translation, more than `maxRuns` runs, an empty request): it then needs prepare().
template <class Run, class Next, class Wired, class ToPhys>
inline uint32_t switch_dma_direct_runs(Next next, Wired wired, ToPhys toPhys, uint32_t total, Run *runs,
                                       uint32_t maxRuns)
{
    uint32_t count = 0;
    for (uint32_t offset = 0; offset < total;) {
        switch_dma_span span = {0, 0};
        if (!next(offset, &span) || !span.length)
            return 0;
        const uint32_t take = span.length < total - offset ? (uint32_t)span.length : total - offset;
        if (!wired(span.va, take))
            return 0;
        for (uint32_t done = 0; done < take;) {
            const uint64_t va = span.va + done;
            const uint32_t inPage = kSwitchDmaTranslateBytes - (uint32_t)(va & (kSwitchDmaTranslateBytes - 1));
            const uint32_t piece = inPage < take - done ? inPage : take - done;
            const uint64_t pa = toPhys(va);
            if (!pa)
                return 0;
            if (count && runs[count - 1].pa + runs[count - 1].length == pa) {
                runs[count - 1].length += piece;
            } else {
                if (count == maxRuns)
                    return 0;
                runs[count].pa = pa;
                runs[count].length = piece;
                ++count;
            }
            done += piece;
        }
        offset += take;
    }
    return count;
}

// fn(va, length) over the virtual ranges of request bytes [offset, offset + length), in order. False when `next` has
// no span where one is needed.
template <class Next, class Fn> inline bool switch_dma_for_each_span(Next next, uint32_t offset, uint32_t length, Fn fn)
{
    for (const uint32_t end = offset + length; offset < end;) {
        switch_dma_span span = {0, 0};
        if (!next(offset, &span) || !span.length)
            return false;
        const uint32_t take = span.length < end - offset ? (uint32_t)span.length : end - offset;
        fn(span.va, take);
        offset += take;
    }
    return true;
}
