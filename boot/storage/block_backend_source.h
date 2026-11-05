#ifndef SWITCH_BLOCK_BACKEND_SOURCE_H
#define SWITCH_BLOCK_BACKEND_SOURCE_H

#include "block_backend.h"
#include "split_disk.h"

/* Trusted native glue supplies this only after card, GPT, FAT and sample
 * qualification. Every referenced object/path and callback context has static
 * or backend-long lifetime. The volume and medium stay immutable except for
 * the reader's serialized sector cache. This is not an on-disk descriptor. */
struct iosw_block_source_disk {
    uint64_t size_bytes;
    uint32_t part_count;
    const struct iosw_split_part *parts;
    const char *label;
};

struct iosw_block_source {
    struct iosw_fat_volume *volume;
    struct iosw_block_source_disk disks[IOSW_BLOCK_DISK_COUNT];
    void *context;
    int (*ready)(void *context);
    int (*begin)(void *context, uint32_t sector_budget, uint32_t seconds_budget);
    int (*end)(void *context);
};

/* Return an iosw_block_status. begin/end delimit one bounded read operation;
 * only one operation can be active, and an error never permits an SD retry. */
int switch_files_block_source(struct iosw_block_source *source);

#endif
