#ifndef IOSWITCH_SPLIT_DISK_H
#define IOSWITCH_SPLIT_DISK_H
#include "fat32.h"
#define IOSW_SPLIT_MAX_PARTS 16u
struct iosw_split_part {
    const char *path;
    uint64_t offset;
    uint32_t size;
};
struct iosw_split_disk {
    struct iosw_fat_volume *volume;
    uint64_t size;
    uint32_t count;
    struct iosw_fat_file files[IOSW_SPLIT_MAX_PARTS];
    uint64_t offsets[IOSW_SPLIT_MAX_PARTS];
};
struct iosw_split_indexed_disk {
    struct iosw_fat_volume *volume;
    uint64_t size;
    uint32_t count;
    struct iosw_fat_indexed_file files[IOSW_SPLIT_MAX_PARTS];
    uint64_t offsets[IOSW_SPLIT_MAX_PARTS];
};
/* Trusted, static descriptors replace an on-card JSON parser. Sizes, offsets
 * and total must be nonzero multiples of 512; files must match sizes exactly.
 * Mapping must be contiguous from zero. Failure invalidates disk. Descriptors
 * need only remain alive during open; no path/descriptor pointers are retained.
 * This validates allocation/mapping, not content identity: authenticate bytes
 * separately against an independently trusted digest. */
int iosw_split_open(struct iosw_split_disk *, struct iosw_fat_volume *,
                    const struct iosw_split_part *, uint32_t count,
                    uint64_t total_size);
/* Byte reads may be unaligned and cross any part/4GiB boundary. On error,
 * discard the entire requested output range (a valid prefix may be present). */
int iosw_split_read(struct iosw_split_disk *, uint64_t offset,
                    void *output, uint32_t length);
/* Validate descriptors and return their exact aggregate uint32_t index-entry
 * count without I/O or allocation. Sets *entries=0 on failure. The caller must
 * enforce its aggregate memory budget before multiplying by sizeof(uint32_t).
 * Actual file sizes/chains are checked by open_indexed, not this sizing call. */
int iosw_split_index_count(const struct iosw_fat_volume *,
                           const struct iosw_split_part *, uint32_t count,
                           uint64_t total_size, uint32_t *entries);
/* One caller-owned pool is divided among parts. Insufficient capacity returns
 * LIMIT before I/O. On any failure disk is invalid and the partial pool MUST
 * NOT be used. The pool, volume and unchanged medium must outlive disk; the
 * caller must keep the pool immutable and serialize access. No allocation or
 * unindexed fallback. Descriptors/paths are not retained after this call. */
int iosw_split_open_indexed(struct iosw_split_indexed_disk *,
                            struct iosw_fat_volume *,
                            const struct iosw_split_part *, uint32_t count,
                            uint64_t total_size, uint32_t *map,
                            uint32_t capacity);
/* Same byte ranges and partial-output-on-failure contract as split_read, with
 * direct indexed cluster lookup and no FAT I/O during data reads. */
int iosw_split_read_indexed(struct iosw_split_indexed_disk *, uint64_t offset,
                            void *output, uint32_t length);
#endif
