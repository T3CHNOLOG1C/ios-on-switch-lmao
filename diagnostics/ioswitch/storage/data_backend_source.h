#ifndef IOSWITCH_DATA_BACKEND_SOURCE_H
#define IOSWITCH_DATA_BACKEND_SOURCE_H
#include "block_backend_source.h"
/* Data allocation is discovered from the current FAT filesystem on each boot.
 * Qualify enables writes only after range and overlap checks. Caller serializes
 * all calls; no host allocation table or card identity is compiled in. */
struct iosw_data_source {
    struct iosw_fat_volume *volume;
    void *context;
    int (*ready)(void *);
    int (*begin)(void *, uint32_t sector_budget, uint32_t seconds_budget);
    int (*begin_metadata)(void *, uint32_t, uint32_t);
    int (*end)(void *);
    void (*offline)(void *);
    int (*qualify)(void *, int (*permit)(void *, uint64_t, uint32_t), void *);
    void (*unbind)(void *, void *);
    int (*write)(void *, uint64_t, uint32_t, const uint8_t *);
    int (*sync)(void *);
};
int ioswitch_files_data_source(struct iosw_data_source *);
#endif
