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

/* 64-byte blocks; bytes is a nonzero multiple of 64 (callers pass 4 KiB multiples). Inline loops, so the compiler
 * cannot turn them into memcpy/memset calls the freestanding image does not have. */
static void copy_blocks(uint64_t to, uint64_t from, uint64_t bytes) {
    __asm__ volatile(
        "1:\n"
        "ldp x9, x10, [%1]\n"
        "ldp x11, x12, [%1, #16]\n"
        "ldp x13, x14, [%1, #32]\n"
        "ldp x15, x16, [%1, #48]\n"
        "stp x9, x10, [%0]\n"
        "stp x11, x12, [%0, #16]\n"
        "stp x13, x14, [%0, #32]\n"
        "stp x15, x16, [%0, #48]\n"
        "add %1, %1, #64\n"
        "add %0, %0, #64\n"
        "subs %2, %2, #64\n"
        "b.ne 1b\n"
        : "+r"(to), "+r"(from), "+r"(bytes)
        :
        : "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "cc", "memory");
}

static void zero_blocks(uint64_t to, uint64_t bytes) {
    __asm__ volatile(
        "1:\n"
        "stp xzr, xzr, [%0]\n"
        "stp xzr, xzr, [%0, #16]\n"
        "stp xzr, xzr, [%0, #32]\n"
        "stp xzr, xzr, [%0, #48]\n"
        "add %0, %0, #64\n"
        "subs %1, %1, #64\n"
        "b.ne 1b\n"
        : "+r"(to), "+r"(bytes)
        :
        : "cc", "memory");
}

int loader_unpack(uint64_t destination, uint64_t source, uint64_t bytes,
                  const uint64_t (*runs)[2], unsigned count,
                  void (*progress)(uint64_t, uint64_t, uint64_t), uint64_t started) {
    uint64_t at = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint64_t offset = runs[i][0], length = runs[i][1];
        if (offset < at || offset > bytes || !length || length > bytes - offset ||
            ((offset | length) & (PAGE - 1))) return 1;
        if (offset > at) zero_blocks(destination + at, offset - at);
        copy_blocks(destination + offset, source, length);
        source += length;
        at = offset + length;
        if (progress) progress(at, bytes, started);
    }
    if (bytes & (PAGE - 1)) return 1;
    if (bytes > at) zero_blocks(destination + at, bytes - at);
    if (progress) progress(bytes, bytes, started);
    return 0;
}

#ifndef LOADER_COPY_HOST_TEST
#include "runs.h"

/* Under loader_copy_run's temporary cacheable regime: the packed staging copy at LOAD_SOURCE (the window's nonzero
 * extents back to back) expanded into the window at LOAD_DEST, zeros everywhere else. The builder checked that the
 * table expands to the exact layout image. */
int loader_copy_payload(void (*progress)(uint64_t, uint64_t, uint64_t), uint64_t started) {
    return loader_unpack(LOAD_DEST, LOAD_SOURCE, LOAD_BYTES, loader_runs, LOAD_RUN_COUNT, progress, started);
}

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
