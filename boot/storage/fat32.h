#ifndef SWITCH_FAT32_H
#define SWITCH_FAT32_H
#include <stdint.h>

/* A deliberately small, read-only FAT32 profile: 512-byte sectors, one or two
 * FATs, a validated partition slice or primary MBR partition, ASCII 8.3 paths.
 * No LFN, partition-table writes, media writes or heap.
 * The caller serializes access and keeps the mounted medium immutable. Remount
 * after changing/replacing it; cached sectors and open files belong to a mount.
 */
#define IOSW_FAT_MAX_PATH 104u
#define IOSW_FAT_MAX_DEPTH 8u
#define IOSW_FAT_MAX_CHAIN 1048576u
#define IOSW_FAT_MAX_IO 32768u
#define IOSW_FAT_MAX_DIR_SECTORS 4096u

enum iosw_fat_error {
    IOSW_FAT_OK=0, IOSW_FAT_ARGUMENT=1, IOSW_FAT_IO=2,
    IOSW_FAT_FORMAT=3, IOSW_FAT_RANGE=4, IOSW_FAT_NOT_FOUND=5,
    IOSW_FAT_NOT_FILE=6, IOSW_FAT_CHAIN=7, IOSW_FAT_LIMIT=8,
    IOSW_FAT_MIRROR=9
};
/* Exactly one absolute 512-byte LBA. Zero means all 512 bytes are valid;
 * nonzero means failure, and the caller discards this sector's buffer. */
typedef int (*iosw_fat_read512)(void *, uint64_t, uint8_t [512]);

struct iosw_fat_volume {
    iosw_fat_read512 read512;
    void *context;
    uint64_t capacity_sectors, partition_start, partition_sectors;
    uint64_t fat_start, data_start;
    uint32_t total_sectors, fat_sectors, cluster_count, root_cluster;
    uint32_t sectors_per_cluster, fat_count, active_fat, mirrored, mounted;
    uint32_t generation, cache_valid;
    uint64_t cache_sector;
    uint8_t cache[2][512];
};
struct iosw_fat_file {
    const struct iosw_fat_volume *volume;
    uint32_t generation, first_cluster, size, clusters;
};
struct iosw_fat_indexed_file {
    struct iosw_fat_file file;
    const uint32_t *clusters;
    uint32_t count;
};

/* Initialize the volume to zero before its first mount. Capacity must come
 * from a trusted block-addressed device qualification, not the BPB or MBR.
 * Failure invalidates the volume; successful remount invalidates old files. */
int iosw_fat_mount(struct iosw_fat_volume *, iosw_fat_read512, void *,
                   uint64_t capacity_sectors, uint32_t partition_index);
/* For a caller-validated GPT or other partition table. The caller establishes
 * the slice's identity and integrity; this function bounds it against device
 * capacity and bounds every FAT32 read against the BPB within that slice.
 * BPB_HiddSec is informational and is not used for addressing. */
int iosw_fat_mount_slice(struct iosw_fat_volume *, iosw_fat_read512, void *,
                         uint64_t capacity_sectors, uint64_t first_sector,
                         uint64_t sector_count);
/* Accept /A/B.BIN or A/B.BIN; ASCII case is folded. Reject . and .., empty
 * components, long names and paths beyond the stated limits. Validates the
 * complete file chain and requires exactly ceil(size/cluster_bytes) clusters.
 * Empty files must have first_cluster=0. On failure file is invalidated. */
int iosw_fat_open(struct iosw_fat_volume *, const char *, struct iosw_fat_file *);
/* Exact bounded read; offset==size is allowed only for length=0. On error,
 * output may contain a prefix: the caller MUST discard the requested range.
 * Each operation is limited to MAX_CHAIN FAT links and MAX_IO sector reads;
 * each directory scanned is additionally capped at MAX_DIR_SECTORS. */
int iosw_fat_read(struct iosw_fat_volume *, const struct iosw_fat_file *,
                  uint64_t offset, void *output, uint32_t length);
/* Exact number of uint32_t cluster entries for a file of this size on a mounted
 * volume. No I/O or allocation. Sets *entries=0 on failure. The caller checks
 * its aggregate byte budget before allocating entries*sizeof(uint32_t). */
int iosw_fat_index_count(const struct iosw_fat_volume *, uint32_t size,
                         uint32_t *entries);
/* Builds an index during the same complete chain validation as open. Requires
 * at least index_count(size) entries; insufficient capacity returns LIMIT.
 * Empty files need no map. Failure invalidates file and may leave a partial map
 * which MUST NOT be used. The caller owns map and keeps its storage alive and
 * immutable, along with the mounted medium, for every indexed-file use. No
 * allocation or fallback to an unindexed scan occurs. */
int iosw_fat_open_indexed(struct iosw_fat_volume *, const char *,
                          struct iosw_fat_indexed_file *, uint32_t *map,
                          uint32_t capacity);
/* Same bounds, generation and partial-output contract as read. Cluster lookup
 * is direct; successful reads perform data-sector I/O only, without FAT reads.
 * Caller serialization and immutable map/medium ownership are required. */
int iosw_fat_read_indexed(struct iosw_fat_volume *,
                          const struct iosw_fat_indexed_file *, uint64_t offset,
                          void *output, uint32_t length);
#endif
