#include "copy.h"
#include "layout.h"

#define PAGE UINT64_C(0x1000)
#define BLOCK UINT64_C(0x200000)
#define ADAPTER UINT64_C(0xa0000000)
#define REFERENCE UINT64_C(0xa001f000)
#define NORMAL (UINT64_C(1) << 2)
#define INNER_SHAREABLE (UINT64_C(3) << 8)
#define ACCESSED (UINT64_C(1) << 10)
#define READONLY (UINT64_C(2) << 6)
#define PXN (UINT64_C(1) << 53)
#define UXN (UINT64_C(1) << 54)
#define RAM (NORMAL | INNER_SHAREABLE | ACCESSED | UXN)
#define DATA (RAM | PXN)

/* Image-backed, aligned storage: the loader does not depend on BSS clearing.
 * 0=L1, 1=L2 RAM, 2=L2 framebuffer, 3=adapter L3, 4=source tail L3,
 * 5=destination head L3, 6=destination tail L3. */
uint64_t loader_copy_tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES]
    __attribute__((section(".copy_tables"), aligned(4096))) = {{0}};

static void map_ram(uint64_t tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES],
                    uint64_t table_address, uint64_t start, uint64_t end,
                    uint64_t attributes, unsigned head_table, unsigned tail_table) {
    uint64_t first_block = start & ~(BLOCK - 1);
    for (uint64_t address = start; address < end;) {
        unsigned l2 = (unsigned)((address >> 21) & 511);
        if (!(address & (BLOCK - 1)) && end - address >= BLOCK) {
            tables[1][l2] = address | attributes | 1;
            address += BLOCK;
        } else {
            unsigned page_table = (address & ~(BLOCK - 1)) == first_block ? head_table : tail_table;
            tables[1][l2] = (table_address + page_table * PAGE) | 3;
            tables[page_table][(address >> 12) & 511] = address | attributes | 3;
            address += PAGE;
        }
    }
}

int loader_copy_build_tables(uint64_t tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES],
                            uint64_t table_address, const struct loader_copy_bounds *b) {
    if (!tables || !b || ((uintptr_t)tables & (PAGE - 1)) ||
        (table_address & (PAGE - 1)) || table_address < ADAPTER || table_address >= REFERENCE ||
        b->image_end != table_address + LOADER_COPY_TABLE_PAGES * PAGE || b->image_end > REFERENCE ||
        b->rx_end < ADAPTER + PAGE || (b->rx_end & (PAGE - 1)) ||
        b->ro_end < b->rx_end || (b->ro_end & (PAGE - 1)) || b->ro_end > table_address ||
        b->source != UINT64_C(0xa2000000) || !b->bytes || (b->bytes & (PAGE - 1)) ||
        b->bytes > UINT64_C(0x6000000) ||
        (b->destination & (PAGE - 1)) || b->destination < UINT64_C(0xb2000000) ||
        b->destination >= UINT64_C(0xc0000000) ||
        b->bytes > UINT64_C(0xc0000000) - b->destination) return 1;

    for (unsigned page = 0; page < LOADER_COPY_TABLE_PAGES; ++page)
        for (unsigned entry = 0; entry < LOADER_COPY_TABLE_ENTRIES; ++entry)
            tables[page][entry] = 0;

    /* All addresses are below 4 GiB. TTBR1 is disabled. Nothing else has a
     * valid descriptor, including the fixed SD reference at 0xa001f000. */
    tables[0][2] = (table_address + PAGE) | 3;
    tables[0][3] = (table_address + 2 * PAGE) | 3;
    tables[1][(ADAPTER >> 21) & 511] = (table_address + 3 * PAGE) | 3;
    for (uint64_t address = ADAPTER; address < b->image_end; address += PAGE) {
        uint64_t permissions = address < b->rx_end ? RAM | READONLY :
            address < b->ro_end ? DATA | READONLY : DATA;
        tables[3][(address >> 12) & 511] = address | permissions | 3;
    }
    map_ram(tables, table_address, b->source, b->source + b->bytes, DATA | READONLY, 4, 4);
    map_ram(tables, table_address, b->destination, b->destination + b->bytes, DATA, 5, 6);

    /* Firmware reserves these complete two blocks for the framebuffer. They
     * remain Device-nGnRnE, non-executable. Display helpers access only the
     * measured 0x384000-byte pixel span; the trailing reserved bytes are idle. */
    for (uint64_t address = UINT64_C(0xf5a00000); address < UINT64_C(0xf5e00000); address += BLOCK)
        tables[2][(address >> 21) & 511] = address | ACCESSED | PXN | UXN | 1;
    return 0;
}

#ifndef LOADER_COPY_HOST_TEST
extern char __loader_rx_end[], __loader_ro_end[], __image_end[];

int loader_copy_prepare(void) {
    const struct loader_copy_bounds bounds = {
        LOAD_SOURCE, LOAD_DEST, LOAD_BYTES,
        (uint64_t)(uintptr_t)__loader_rx_end,
        (uint64_t)(uintptr_t)__loader_ro_end,
        (uint64_t)(uintptr_t)__image_end
    };
    return loader_copy_build_tables(loader_copy_tables,
        (uint64_t)(uintptr_t)loader_copy_tables, &bounds);
}
#endif
