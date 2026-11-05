#ifndef IOSWITCH_LOADER_MEMORY_H
#define IOSWITCH_LOADER_MEMORY_H
#include <stdint.h>
#include "ram.h"
struct loader_memory_report {
    uint32_t reservations;
    uint32_t reserved_nodes;
    uint32_t required_reservations;
    uint32_t ramoops_seen;
    uint32_t bank_count;
    struct iosw_ram_ledger ledger;
};
int loader_check_fdt_memory(const uint8_t *fdt, uint32_t capacity,
                           uint64_t window_start, uint64_t window_end,
                           struct loader_memory_report *report);
/* The same check for several ranges in one walk of the FDT (the loader reads it uncached, so each walk is costly).
 * Returns exactly what loader_check_fdt_memory for ranges[0], ranges[1], ... in turn would first have failed with,
 * and that range's index in *range; 0 when all pass. The report is the walk's own (it does not depend on ranges). */
#define LOADER_MEMORY_RANGES_MAX 8u
struct loader_memory_range { uint64_t start, end; };
int loader_check_fdt_memory_ranges(const uint8_t *fdt, uint32_t capacity,
                                   const struct loader_memory_range *ranges, unsigned count,
                                   struct loader_memory_report *report, unsigned *range);
#endif
