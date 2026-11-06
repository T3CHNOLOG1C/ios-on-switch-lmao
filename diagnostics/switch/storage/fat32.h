#ifndef SWITCH_FAT32_H
#define SWITCH_FAT32_H
#include <stdint.h>

/* A deliberately small, read-only FAT32 profile: 512-byte sectors, one or two
 * FATs, a validated partition slice or primary MBR partition, ASCII 8.3 paths.
 * No LFN, partition-table writes, media writes or heap.
 * The caller serializes access and keeps the mounted medium immutable. Remount
 * after changing/replacing it; cached sectors and open files belong to a mount.
 */
#define SWITCH_FAT_MAX_PATH         104u
#define SWITCH_FAT_MAX_DEPTH        8u
#define SWITCH_FAT_MAX_CHAIN        1048576u
#define SWITCH_FAT_MAX_IO           32768u
#define SWITCH_FAT_MAX_DIR_SECTORS  4096u
#define SWITCH_FAT_MAX_READ_SECTORS 128u

enum switch_fat_error {
    SWITCH_FAT_OK = 0,
    SWITCH_FAT_ARGUMENT = 1,
    SWITCH_FAT_IO = 2,
    SWITCH_FAT_FORMAT = 3,
    SWITCH_FAT_RANGE = 4,
    SWITCH_FAT_NOT_FOUND = 5,
    SWITCH_FAT_NOT_FILE = 6,
    SWITCH_FAT_CHAIN = 7,
    SWITCH_FAT_LIMIT = 8,
    SWITCH_FAT_MIRROR = 9
};

/* Exactly one absolute 512-byte LBA. Zero means all 512 bytes are valid;
 * nonzero means failure, and the caller discards this sector's buffer. */
typedef int (*switch_fat_read512)(void *, uint64_t, uint8_t[512]);
/* Optional exact contiguous read of 1..MAX_READ_SECTORS absolute 512-byte LBAs.
 * Zero means every requested byte is valid; on error the caller discards the
 * entire requested range, which may have been modified. No read-ahead. */
typedef int (*switch_fat_read_sectors)(void *, uint64_t, uint32_t, uint8_t *);

struct switch_fat_volume {
    switch_fat_read512 read512;
    void *context;
    uint64_t capacity_sectors, partition_start, partition_sectors;
    uint64_t fat_start, data_start;
    uint32_t total_sectors, fat_sectors, cluster_count, root_cluster;
    uint32_t sectors_per_cluster, fat_count, active_fat, mirrored, mounted;
    uint32_t generation, cache_valid;
    uint64_t cache_sector;
    uint8_t cache[2][512];
    /* Set after a successful mount, under caller serialization. Every mount
     * clears it; NULL retains the original single-sector callback path. */
    switch_fat_read_sectors read_sectors;
};

struct switch_fat_file {
    const struct switch_fat_volume *volume;
    uint32_t generation, first_cluster, size, clusters;
};

struct switch_fat_indexed_file {
    struct switch_fat_file file;
    const uint32_t *clusters;
    uint32_t count;
};

/* Initialize the volume to zero before its first mount. Capacity must come
 * from a trusted block-addressed device qualification, not the BPB or MBR.
 * Failure invalidates the volume; successful remount invalidates old files. */
int switch_fat_mount(struct switch_fat_volume *, switch_fat_read512, void *, uint64_t capacity_sectors,
                     uint32_t partition_index);
/* For a caller-validated GPT or other partition table. The caller establishes
 * the slice's identity and integrity; this function bounds it against device
 * capacity and bounds every FAT32 read against the BPB within that slice.
 * BPB_HiddSec is informational and is not used for addressing. */
int switch_fat_mount_slice(struct switch_fat_volume *, switch_fat_read512, void *, uint64_t capacity_sectors,
                           uint64_t first_sector, uint64_t sector_count);
/* Accept /A/B.BIN or A/B.BIN; ASCII case is folded. Reject . and .., empty
 * components, long names and paths beyond the stated limits. Validates the
 * complete file chain and requires exactly ceil(size/cluster_bytes) clusters.
 * Empty files must have first_cluster=0. On failure file is invalidated. */
int switch_fat_open(struct switch_fat_volume *, const char *, struct switch_fat_file *);
/* Cheap candidate selection: resolve a path and validate its directory-entry
 * metadata, returning the final regular file's size without walking its file
 * chain or reading file payload. *size is unchanged on failure. Ancestor lookup
 * has the normal bounded scan semantics; it does not validate directory tails.
 * This is NOT an open/authorization result: open_indexed and, before writes,
 * path_directories_exclude remain required against the selected mounted volume.
 */
int switch_fat_lookup_size(struct switch_fat_volume *, const char *, uint32_t *size);
/* Resolve a file path and require the COMPLETE FAT chains of its root and
 * ancestor directories to exclude every supplied cluster. This includes
 * allocated directory tails after an early name match or 0x00 end marker.
 * The exclusion list must contain valid, strictly increasing cluster numbers;
 * count=0 allows a NULL list. Directory/file payloads outside path lookup are
 * not read, and the final file chain is not revalidated (use open_indexed).
 * A cross-link returns CHAIN; malformed/cyclic chains, mirrors and I/O errors
 * retain their normal errors. Each directory's complete allocation is capped
 * at MAX_DIR_SECTORS and the operation shares MAX_IO/MAX_CHAIN budgets.
 * Call under the same serialized, immutable-metadata mount as the file index.
 * This protects only the named path's ancestors, not unrelated directory trees.
 */
int switch_fat_path_directories_exclude(struct switch_fat_volume *, const char *, const uint32_t *sorted_clusters,
                                        uint32_t count);
/* Exact bounded read; offset==size is allowed only for length=0. On error,
 * output may have been modified: the caller MUST discard the requested range.
 * Each operation is limited to MAX_CHAIN FAT links and MAX_IO sector reads;
 * each directory scanned is additionally capped at MAX_DIR_SECTORS. */
int switch_fat_read(struct switch_fat_volume *, const struct switch_fat_file *, uint64_t offset, void *output,
                    uint32_t length);
/* Exact number of uint32_t cluster entries for a file of this size on a mounted
 * volume. No I/O or allocation. Sets *entries=0 on failure. The caller checks
 * its aggregate byte budget before allocating entries*sizeof(uint32_t). */
int switch_fat_index_count(const struct switch_fat_volume *, uint32_t size, uint32_t *entries);
/* Builds an index during the same complete chain validation as open. Requires
 * at least index_count(size) entries; insufficient capacity returns LIMIT.
 * Empty files need no map. Failure invalidates file and may leave a partial map
 * which MUST NOT be used. The caller owns map and keeps its storage alive and
 * immutable, along with the mounted medium, for every indexed-file use. No
 * allocation or fallback to an unindexed scan occurs. */
int switch_fat_open_indexed(struct switch_fat_volume *, const char *, struct switch_fat_indexed_file *, uint32_t *map,
                            uint32_t capacity);
/* Same bounds, generation and partial-output contract as read. Cluster lookup
 * is direct; successful reads perform data-sector I/O only, without FAT reads.
 * An optional read_sectors callback combines requested whole sectors across
 * physically contiguous indexed clusters, at most MAX_READ_SECTORS per call.
 * Caller serialization and immutable map/medium ownership are required. */
int switch_fat_read_indexed(struct switch_fat_volume *, const struct switch_fat_indexed_file *, uint64_t offset,
                            void *output, uint32_t length);
#endif
