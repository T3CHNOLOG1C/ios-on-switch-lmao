#ifndef SWITCH_PARTITION_H
#define SWITCH_PARTITION_H
#include "fat32.h"

#define IOSW_GPT_MAX_ENTRIES 128u
enum iosw_gpt_error {
    IOSW_GPT_OK=0, IOSW_GPT_ARGUMENT=1, IOSW_GPT_IO=2, IOSW_GPT_MBR=3,
    IOSW_GPT_HEADER=4, IOSW_GPT_HEADER_CRC=5, IOSW_GPT_GEOMETRY=6,
    IOSW_GPT_ENTRIES=7, IOSW_GPT_ARRAY_CRC=8, IOSW_GPT_MISMATCH=9,
    IOSW_GPT_NOT_FOUND=10, IOSW_GPT_OVERLAP=11, IOSW_GPT_DUPLICATE=12
};
struct iosw_gpt_slice {
    uint64_t first_sector, sector_count;
    uint32_t partition_index; /* Zero-based GPT array index. */
    uint8_t unique_guid[16];  /* Exact on-disk GUID byte order. */
};
struct iosw_gpt_header {
    uint64_t first_usable, last_usable, entry_lba;
    uint32_t entry_count, entry_size, array_crc;
    uint8_t disk_guid[16];
};
struct iosw_gpt_extent {
    uint64_t first, last;
    uint8_t guid[16];
};
/* Caller-owned scratch, normally static for kernel callers. No initialization
 * needed, no heap, and no pointers into scratch are retained by the result. */
struct iosw_gpt_workspace {
    uint8_t sectors[2][512];
    struct iosw_gpt_header headers[2];
    struct iosw_gpt_extent used[IOSW_GPT_MAX_ENTRIES];
    struct iosw_gpt_slice candidate;
};

/* Read-only, strict GPT v1 profile: 512-byte sectors, 92-byte headers,
 * 1..128 entries of 128/256/512 bytes, at most 64KiB of declared entry data.
 * At least 16KiB must be reserved at each array location. Hekate may declare
 * only created entries; CRCs and copy comparisons cover exactly that count,
 * excluding final-sector padding and undeclared reserved entries. Validates both
 * headers at LBA1/capacity-1, both array CRCs and their exact covered bytes,
 * all used extents, unique GUIDs, and no overlaps. No repair or fallback.
 * MBR must have its signature and a protective type0xEE entry; additional
 * hybrid MBR entries are permitted and never used for partition selection.
 * Finds the Microsoft Basic Data partition with the exact expected GUID.
 * GUID arguments are in on-disk byte order (UUID.bytes_le), not display order.
 * Capacity must be independently trusted. Serial immutable-media access and
 * callback semantics are as in fat32.h. At most 259 sector callbacks occur.
 * Failure leaves *output entirely unchanged. */
int iosw_gpt_find(iosw_fat_read512, void *context, uint64_t capacity_sectors,
                  const uint8_t expected_unique_guid[16],
                  struct iosw_gpt_workspace *, struct iosw_gpt_slice *output);
#endif
