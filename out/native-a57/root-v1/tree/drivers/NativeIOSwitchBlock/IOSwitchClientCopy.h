// SPDX-License-Identifier: BSD-2-Clause-Patent
// A request's bytes between its IOMemoryDescriptor and the SD bounce buffer, without the cache flush that
// IOMemoryDescriptor::readBytes/writeBytes add on this kernel.
//
// readBytes/writeBytes copy page by page with copypv(..., cppvFsnk | ...). This XNU is built for a board that is not
// IO-coherent (DEVELOPMENT_ARM64_BCM2837: no __ARM_COHERENT_IO__), and there cppvFsnk makes copypv clean and
// invalidate every destination line to DRAM after the copy (flush_dcache64 -> flush_dcache -> FlushPoC_DcacheRegion,
// one dc civac per 64 bytes, by address). On a card read that is a second pass over every byte read: the lines the
// copy just filled are written back to DRAM and dropped, so APFS then reads them from DRAM again (cleaning dirty lines
// cost 0.34 ms per MiB in the GPU driver's r7l BoAlloc numbers). On a card write it flushes the bounce buffer, which
// transfer() cleans again before the command anyway.
//
// Nothing needs that flush. The CPU reads what it wrote. A device that DMAs out of such pages cleans them first (USB
// UsbDmaMapper kIOMemoryIncoherentIOStore, SwitchWLAN, TegraNVDEC and the GPU's BoImport clean by hand), and a page
// first mapped executable is synced at its fault (pmap_sync_page_data_phys -> InvalidatePoU_IcacheRegion, which
// cleans to PoU first). copypv still copies a page with other cache attributes (write-combined, device) through a
// window with that page's own attributes (bcopy_phys_internal), as readBytes/writeBytes did.
#pragma once

#include <stdint.h>

// Flags to copypv for each direction: readBytes'/writeBytes' own, less cppvFsnk. A card read fills the client's
// pages (physical sink) from the bounce buffer (kernel virtual source); a card write stages the other way round.
constexpr int kIOSwitchCopyToClient = cppvPsnk | cppvNoRefSrc | cppvNoModSnk | cppvKmap;
constexpr int kIOSwitchCopyFromClient = cppvPsrc | cppvNoRefSrc | cppvKmap;

// Copies `length` bytes between `memory` (from its offset `from`) and `bounce`, one physical segment at a time; `copy` is
// copypv. False when a segment is missing or a copy fails (the request then completes with an I/O error).
template <typename Memory, typename Copy>
inline bool ioswitchClientCopy(Memory *memory, uint8_t *bounce, uint32_t length, bool toClient, Copy copy,
                               uint32_t from = 0)
{
    for (uint32_t done = 0; done < length;) {
        IOByteCount segment = 0;
        const addr64_t pa = memory->getPhysicalSegment(from + done, &segment, kIOMemoryMapperNone);
        if (!pa || !segment) return false;
        if (segment > length - done) segment = length - done;
        const addr64_t va = reinterpret_cast<uintptr_t>(bounce + done);
        const auto unsignedSegment = static_cast<unsigned int>(segment);
        if (toClient ? copy(va, pa, unsignedSegment, kIOSwitchCopyToClient)
                     : copy(pa, va, unsignedSegment, kIOSwitchCopyFromClient))
            return false;
        done += unsignedSegment;
    }
    return true;
}

// The bytes [from, from + length) of a chain of requests that follow each other on the card (`next`; `sectors` of 512
// bytes each), against bounce[0, length): `stage(request, offset in its memory, bounce address, bytes)` once for each
// request the window touches, in chain order. False when a stage fails or the chain ends before the window does. The
// bounce path uses it to move a chain longer than one SDMA command in pieces.
template <typename Request, typename Stage>
inline bool ioswitchChainCopy(Request *first, uint32_t from, uint32_t length, uint8_t *bounce, Stage stage)
{
    for (Request *r = first; r && length; r = r->next) {
        const uint32_t size = r->sectors * 512;
        if (from >= size) {
            from -= size;
            continue;
        }
        const uint32_t n = size - from < length ? size - from : length;
        if (!stage(r, from, bounce, n)) return false;
        bounce += n;
        length -= n;
        from = 0;
    }
    return !length;
}
