#ifndef IOSWITCH_LOADER_COPY_H
#define IOSWITCH_LOADER_COPY_H

#define LOADER_COPY_TABLE_PAGES 7
#define LOADER_COPY_TABLE_ENTRIES 512
#define LOADER_COPY_TCR 0x80a03520
#define LOADER_COPY_MAIR 0xff00

#ifndef __ASSEMBLER__
#include <stdint.h>

/* The physical table address is explicit so the page-table builder can be
 * checked on the host without executing privileged instructions. */
struct loader_copy_bounds {
    uint64_t source, destination, bytes;
    uint64_t rx_end, ro_end, image_end;
};

int loader_copy_build_tables(uint64_t tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES],
                            uint64_t table_address, const struct loader_copy_bounds *bounds);
int loader_copy_prepare(void);
uint64_t loader_copy_run(void (*progress)(uint64_t, uint64_t, uint64_t), uint64_t started);
#endif
#endif
