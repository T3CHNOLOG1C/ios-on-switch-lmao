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
#endif
