// SPDX-License-Identifier: BSD-2-Clause-Patent
// ADMA2 descriptor tables for the SD driver: scatter-gather DMA straight between the card and a request's own pages.
//
// Format: SD Host Controller spec v4 ADMA2 with 64-bit addressing, the mode setup() leaves SDMMC1 in (HOST_CONTROL2 Host
// Version 4 Enable + 64-bit Addressing) and the one NVIDIA's L4T kernel uses on this controller (switchroot
// linux-4.9 sdhci-tegra.c soc_data_tegra210: SDHCI_QUIRK2_USE_64BIT_ADDR, so sdhci.c takes the 16-byte descriptor
// SDHCI_ADMA2_64_ADDR_DESC_SZ). Tegra210 quirks from the same table, both kept here:
// - SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC: a length of 0 does not mean 64 KiB, so no descriptor is longer than
//   kAdmaMaxLength (below 64 KiB, a whole number of 4 KiB pages so splits stay page- and line-aligned).
// - SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC: End goes on the last transfer descriptor, never on a separate NOP.
// Mainline Linux gives Tegra210's SDMMC a 34-bit DMA mask (sdhci-tegra.c soc_data_tegra210.dma_mask) with no SMMU in
// front of it, which covers this board's DRAM (0x80000000-0x17fffffff); setup() still checks addresses above 4 GiB on the
// hardware before it lets the data path use them (limit).
//
// What may go straight to the card (anything else takes the bounce buffer; the builder says why):
// - every segment starts and ends on a 64-byte cache line: the driver cleans or invalidates whole lines of the client's
//   pages, which is only correct when no other data shares one (a read's invalidate would drop a neighbour's writes);
// - every page is managed RAM with the default (cacheable) attributes, as `pageOk` reports: the lines are reached through
//   the kernel's physical aperture, and a write-combined or device page is staged by copypv with its own attributes;
// - everything below `limit`, and the table has room.
#pragma once

#include <stdint.h>

// One descriptor: 128 bits, little-endian (the driver and the controller are both little-endian).
struct IOSwitchAdmaDescriptor {
    uint16_t attributes;
    uint16_t length;
    uint32_t addressLow;
    uint32_t addressHigh;
    uint32_t reserved;  // always 0
};
static_assert(sizeof(IOSwitchAdmaDescriptor) == 16, "ADMA2 64-bit addressing descriptors are 128 bits in v4 mode");

// Attribute bits: Valid, End, and Act = transfer (sdhci.h ADMA2_TRAN_VALID 0x21, | End = 0x23). No Int: the driver waits
// for transfer complete, never for a descriptor.
constexpr uint16_t kAdmaValid = 0x01, kAdmaEnd = 0x02, kAdmaTransfer = 0x20;
constexpr uint32_t kAdmaMaxLength = 0xf000;
constexpr uint64_t kAdmaLine = 64, kAdmaPage = 4096;

// Why a request went through the bounce buffer instead (stats index; kAdmaOk is "ADMA2 is off").
enum IOSwitchAdmaRefusal : uint8_t {
    kAdmaOk = 0,
    kAdmaAlign,    // a segment does not start and end on a cache line
    kAdmaMemory,   // a page is not managed, cacheable RAM
    kAdmaHigh,     // a segment reaches the address limit
    kAdmaFull,     // more descriptors than the table holds
    kAdmaMissing,  // a segment the memory descriptor does not have (or an empty one)
    kAdmaRefusals
};

struct IOSwitchAdmaTable {
    IOSwitchAdmaDescriptor *entries;
    uint32_t capacity;
    uint64_t limit;  // the first physical address the engine may not reach
    uint32_t count;
    uint64_t bytes;

    void reset()
    {
        count = 0;
        bytes = 0;
    }

    // [pa, pa + length) as transfer descriptors, continuing the previous descriptor when the memory continues it.
    template <typename PageOk> IOSwitchAdmaRefusal add(uint64_t pa, uint64_t length, PageOk pageOk)
    {
        if (!length) return kAdmaMissing;
        if ((pa | length) & (kAdmaLine - 1)) return kAdmaAlign;
        if (pa >= limit || length > limit - pa) return kAdmaHigh;
        for (uint64_t page = pa & ~(kAdmaPage - 1); page < pa + length; page += kAdmaPage)
            if (!pageOk(page)) return kAdmaMemory;
        while (length) {
            IOSwitchAdmaDescriptor *last = count ? &entries[count - 1] : nullptr;
            uint64_t room = 0;
            if (last && (static_cast<uint64_t>(last->addressHigh) << 32 | last->addressLow) + last->length == pa)
                room = kAdmaMaxLength - last->length;
            if (!room) {
                if (count == capacity) return kAdmaFull;
                last = &entries[count++];
                *last = {kAdmaValid | kAdmaTransfer, 0, static_cast<uint32_t>(pa), static_cast<uint32_t>(pa >> 32), 0};
                room = kAdmaMaxLength;
            }
            const uint64_t n = length < room ? length : room;
            last->length = static_cast<uint16_t>(last->length + n);
            pa += n;
            length -= n;
            bytes += n;
        }
        return kAdmaOk;
    }

    // The first `length` bytes of `memory`, one physical segment at a time (getPhysicalSegment, CPU addresses).
    template <typename Memory, typename PageOk>
    IOSwitchAdmaRefusal addMemory(Memory *memory, uint64_t length, PageOk pageOk)
    {
        for (uint64_t done = 0; done < length;) {
            IOByteCount segment = 0;
            const uint64_t pa = memory->getPhysicalSegment(done, &segment, kIOMemoryMapperNone);
            if (!pa || !segment) return kAdmaMissing;
            if (segment > length - done) segment = length - done;
            if (const IOSwitchAdmaRefusal refusal = add(pa, segment, pageOk)) return refusal;
            done += segment;
        }
        return kAdmaOk;
    }

    // End on the last descriptor; false for an empty table.
    bool finish()
    {
        if (!count) return false;
        entries[count - 1].attributes |= kAdmaEnd;
        return true;
    }
};

inline uint64_t ioswitchAdmaAddress(const IOSwitchAdmaDescriptor &descriptor)
{
    return static_cast<uint64_t>(descriptor.addressHigh) << 32 | descriptor.addressLow;
}
