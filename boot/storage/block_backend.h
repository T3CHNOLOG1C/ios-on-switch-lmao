#ifndef SWITCH_BLOCK_BACKEND_H
#define SWITCH_BLOCK_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWITCH_BLOCK_DISK_COUNT 2u
#define SWITCH_BLOCK_SECTOR_BYTES 512u
#define SWITCH_BLOCK_MAX_TRANSFER_BYTES 65536u
#define SWITCH_BLOCK_MAX_INDEX_BYTES (16u * 1024u * 1024u)

enum switch_block_status {
    SWITCH_BLOCK_OK = 0,
    SWITCH_BLOCK_NOT_READY = 1,
    SWITCH_BLOCK_INVALID = 2,
    SWITCH_BLOCK_IO = 3,
    SWITCH_BLOCK_TIMEOUT = 4,
    SWITCH_BLOCK_NOMEM = 5,
    SWITCH_BLOCK_CORRUPT = 6
};

struct switch_block_backend;

struct switch_block_allocator {
    void *context;
    void *(*allocate)(void *context, size_t bytes);
    void (*release)(void *context, void *allocation, size_t bytes);
};

struct switch_block_info {
    uint64_t size_bytes;
    char label[16];
};

/* Caller serializes ALL calls across both disks with one sleepable lock.
 * Creation is permitted only after the native card/GPT/FAT/sample qualification
 * has succeeded. The backend retains no stack-owned host or callback context.
 * Index allocations are bounded in aggregate by SWITCH_BLOCK_MAX_INDEX_BYTES.
 * On failure *out is NULL and every newly owned allocation is released. */
int switch_block_create(const struct switch_block_allocator *allocator,
                     struct switch_block_backend **out);

/* Stable indexes: 0 = OS, 1 = SystemOS. Capacities are immutable, nonzero and
 * sector aligned. Labels are NUL terminated. No fabricated fallback sizes. */
int switch_block_get_info(struct switch_block_backend *backend, unsigned index,
                       struct switch_block_info *info);

/* Read only, offset and nonzero length aligned to512 bytes, length<=64KiB.
 * OK means every requested byte is valid. On any error the caller discards
 * the entire output range, even if a prefix was filled. A fatal controller
 * failure latches the backend unavailable; no automatic reset or retry.
 * This function never calls IOKit, invokes completion, or changes its lock. */
int switch_block_read(struct switch_block_backend *backend, unsigned index,
                   uint64_t byte_offset, void *destination, uint32_t length);

/* No published child or in-flight read may retain the backend when destroyed.
 * Releases only backend-owned allocations, never firmware/reference memory. */
void switch_block_destroy(struct switch_block_backend *backend);

/* Built-in IOKit bridge. Called after the diagnostic qualification succeeds.
 * Returns switch_block_status; repeated calls never publish duplicate devices. */
int switch_block_publish(void);

/* One non-waiting registry snapshot: exact expected APFS volume UUID plus
 * provider ancestry to the selected published child. OK returns observed
 * /dev/<BSDName>; NOT_READY means the fixture may retry within its own bound.
 * Duplicate UUID matches are CORRUPT. Clears path on error. This establishes
 * media identity only and does not authorize root or authenticate contents. */
int switch_block_volume_path(unsigned index, char *path, unsigned capacity);

#ifdef __cplusplus
}
#endif
#endif
