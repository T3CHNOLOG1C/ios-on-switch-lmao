#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "copy.h"

#define PAGE UINT64_C(0x1000)
#define BLOCK UINT64_C(0x200000)
#define ADDRESS UINT64_C(0x0000fffffffff000)
#define AF (UINT64_C(1) << 10)
#define RO (UINT64_C(2) << 6)
#define PXN (UINT64_C(1) << 53)
#define UXN (UINT64_C(1) << 54)
#define RAM ((UINT64_C(1) << 2) | (UINT64_C(3) << 8) | AF | UXN)

static uint64_t (*tables)[512];
static unsigned char *allocation;
static const uint64_t table_address = UINT64_C(0xa0015000);

static uint64_t *next_table(uint64_t descriptor) {
    assert((descriptor & 3) == 3);
    uint64_t pa = descriptor & ADDRESS;
    assert(pa >= table_address && pa < table_address + LOADER_COPY_TABLE_PAGES * PAGE);
    assert((pa - table_address) % PAGE == 0);
    return tables[(pa - table_address) / PAGE];
}

/* Decode the generated binary descriptors independently of map_ram. */
static uint64_t lookup(uint64_t va, unsigned *level) {
    uint64_t d = tables[0][va >> 30];
    if (!d) return 0;
    assert((d & ~ADDRESS) == 3);
    uint64_t *l2 = next_table(d);
    d = l2[(va >> 21) & 511];
    if (!d) return 0;
    if ((d & 3) == 1) {
        *level = 2;
        assert((d & ADDRESS & ~(BLOCK - 1)) == (va & ~(BLOCK - 1)));
        return d;
    }
    assert((d & ~ADDRESS) == 3);
    d = next_table(d)[(va >> 12) & 511];
    if (d) {
        *level = 3;
        assert((d & 3) == 3);
        assert((d & ADDRESS) == va);
    }
    return d;
}

static void guards(void) {
    for (unsigned i = 0; i < PAGE; ++i) {
        assert(allocation[i] == 0xa5);
        assert(allocation[(LOADER_COPY_TABLE_PAGES + 1) * PAGE + i] == 0xa5);
    }
}

static void positive(struct loader_copy_bounds b) {
    assert(loader_copy_build_tables(tables, table_address, &b) == 0);
    for (uint64_t va = 0; va < UINT64_C(0x100000000); va += PAGE) {
        uint64_t expected = 0;
        if (va >= UINT64_C(0xa0000000) && va < b.image_end)
            expected = va < b.rx_end ? RAM | RO :
                va < b.ro_end ? RAM | RO | PXN : RAM | PXN;
        else if (va >= b.source && va < b.source + b.source_bytes)
            expected = RAM | RO | PXN;
        else if (va >= b.destination && va < b.destination + b.bytes)
            expected = RAM | PXN;
        else if (va >= UINT64_C(0xf5a00000) && va < UINT64_C(0xf5e00000))
            expected = AF | PXN | UXN;
        unsigned level = 0;
        uint64_t actual = lookup(va, &level);
        if (!expected) assert(actual == 0);
        else {
            assert(actual);
            assert((actual & ~ADDRESS & ~UINT64_C(3)) == expected);
            if (level == 2) {
                uint64_t begin = va & ~(BLOCK - 1);
                if (va >= b.source && va < b.source + b.source_bytes)
                    assert(begin >= b.source && begin + BLOCK <= b.source + b.source_bytes);
                if (va >= b.destination && va < b.destination + b.bytes)
                    assert(begin >= b.destination && begin + BLOCK <= b.destination + b.bytes);
            }
        }
    }
    guards();
}

int main(void) {
    assert(posix_memalign((void **)&allocation, PAGE, (LOADER_COPY_TABLE_PAGES + 2) * PAGE) == 0);
    memset(allocation, 0xa5, (LOADER_COPY_TABLE_PAGES + 2) * PAGE);
    tables = (uint64_t (*)[512])(allocation + PAGE);
    struct loader_copy_bounds b = {
        UINT64_C(0xaa101000), UINT64_C(0xb207c000), UINT64_C(0x4308000),
        UINT64_C(0xa0006000), UINT64_C(0xa0007000), UINT64_C(0xa001d000),
        UINT64_C(0xe3c000)
    };
    const uint64_t sizes[] = {PAGE, BLOCK - PAGE, BLOCK, BLOCK + PAGE,
        2 * BLOCK - PAGE, 2 * BLOCK + PAGE, UINT64_C(0x4308000), UINT64_C(0x6000000)};
    const uint64_t offsets[] = {0, PAGE, UINT64_C(0x7c000), BLOCK - PAGE};
    unsigned positives = 0, negatives = 0;
    for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); ++n)
        for (unsigned o = 0; o < sizeof(offsets) / sizeof(offsets[0]); ++o) {
            b.bytes = sizes[n]; b.destination = UINT64_C(0xb2000000) + offsets[o];
            /* The staged file (in bl33.bin, from 0xaa101000: a partial first block) is smaller than the window it
             * decodes into, or as large, up to the page before ramoops. */
            static const uint64_t staged[] = {PAGE, BLOCK - PAGE, UINT64_C(0xff000), UINT64_C(0x100000),
                UINT64_C(0x100000) + PAGE, UINT64_C(0xe3f000), UINT64_C(0x4308000), UINT64_C(0x5efe000)};
            b.source_bytes = staged[(n + o) % (sizeof(staged) / sizeof(staged[0]))];
            positive(b); ++positives;
        }
    uint64_t before[LOADER_COPY_TABLE_PAGES][512];
    memcpy(before, tables, sizeof(before));
    struct loader_copy_bounds good = b;
    for (unsigned kind = 0; kind < 23; ++kind) {
        b = good;
        uint64_t pa = table_address;
        switch (kind) {
            case 0: b.bytes = 0; break;
            case 1: b.bytes = 1; break;
            case 2: b.bytes = UINT64_MAX; break;
            case 3: b.bytes = UINT64_C(0x6001000); break;
            case 4: b.source += PAGE; break;
            case 5: b.destination++; break;
            case 6: b.destination = UINT64_C(0xb1fff000); break;
            case 7: b.destination = UINT64_C(0xc0000000); break;
            case 8: b.destination = UINT64_C(0xbffff000); break;
            case 9: b.rx_end = UINT64_C(0xa0000000); break;
            case 10: b.rx_end++; break;
            case 11: b.ro_end = b.rx_end - PAGE; break;
            case 12: b.ro_end++; break;
            case 13: b.ro_end = pa + PAGE; break;
            case 14: b.image_end++; break;
            case 15: pa++; break;
            case 16: pa = UINT64_C(0x9ffff000); break;
            case 17: pa = UINT64_C(0xa0018000); b.image_end = pa + LOADER_COPY_TABLE_PAGES * PAGE; break;
            case 18: b.source_bytes = 0; break;
            case 19: b.source_bytes = 1; break;
            case 20: b.source_bytes = UINT64_C(0x6001000); break;
            case 21: b.source_bytes = UINT64_MAX; break;
            case 22: b.source_bytes = UINT64_C(0x5eff000); break; /* would reach ramoops */
        }
        assert(loader_copy_build_tables(tables, pa, &b) != 0);
        assert(memcmp(before, tables, sizeof(before)) == 0);
        guards(); ++negatives;
    }
    assert(loader_copy_build_tables(NULL, table_address, &good) != 0); ++negatives;
    assert(loader_copy_build_tables(tables, table_address, NULL) != 0); ++negatives;
    assert(loader_copy_build_tables((uint64_t (*)[512])((unsigned char *)tables + 8), table_address, &good) != 0); ++negatives;
    guards();
    free(allocation);
    printf("PASS %u exact 4GiB map checks and %u malformed input controls\n", positives, negatives);
    return 0;
}
