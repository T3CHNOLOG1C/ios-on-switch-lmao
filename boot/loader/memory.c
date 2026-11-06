#include "memory.h"

/* The loader runs this with the MMU off: every load is a Device-nGnRnE access that waits for DRAM in turn. The FDT
 * is therefore read one aligned 32-bit word at a time (tokens, sizes and cells are 4-byte aligned in it; a byte is
 * taken from the word that holds it), never byte by byte: a quarter of the accesses. */
static uint32_t word_at(const uint8_t *p)
{
    return *(const volatile uint32_t *)(const void *)((uintptr_t)p & ~(uintptr_t)3);
}

static uint8_t byte_at(const uint8_t *p)
{
    return (uint8_t)(word_at(p) >> (8 * ((uintptr_t)p & 3)));
}

static uint32_t be32(const uint8_t *p)
{
    if (!((uintptr_t)p & 3))
        return __builtin_bswap32(word_at(p));
    return (uint32_t)byte_at(p) << 24 | (uint32_t)byte_at(p + 1) << 16 | (uint32_t)byte_at(p + 2) << 8 | byte_at(p + 3);
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

/* The first zero byte in [at, limit) of base, or limit: one word read per four bytes. */
static uint32_t find_nul(const uint8_t *base, uint32_t at, uint32_t limit)
{
    while (at < limit) {
        const uint32_t word = word_at(base + at);
        for (unsigned b = (unsigned)((uintptr_t)(base + at) & 3); b < 4 && at < limit; ++b, ++at)
            if (!((word >> (8 * b)) & 0xff))
                return at;
    }
    return limit;
}

static int name_is(const uint8_t *p, uint32_t length, const char *name)
{
    uint32_t i = 0, word = 0;
    uintptr_t loaded = 1; /* never a word address */
    while (i < length && name[i]) {
        const uintptr_t at = (uintptr_t)(p + i);
        if ((at & ~(uintptr_t)3) != loaded) {
            loaded = at & ~(uintptr_t)3;
            word = word_at(p + i);
        }
        if ((uint8_t)(word >> (8 * (at & 3))) != (uint8_t)name[i])
            break;
        ++i;
    }
    return i == length && name[i] == 0;
}

static int overlaps(uint64_t base, uint64_t size, uint64_t start, uint64_t end)
{
    if (size > UINT64_MAX - base)
        return -1;
    return size && base < end && start < base + size;
}

/* Overlap of [base, base + size) with any of the ranges (-1: the reservation wraps), recorded per range: the first
 * failure code each range would have stopped a walk of its own with. */
static int overlaps_any(uint64_t base, uint64_t size, const struct loader_memory_range *ranges, unsigned count,
                        int code, int *first)
{
    int any = 0;
    for (unsigned i = 0; i < count; ++i) {
        const int o = overlaps(base, size, ranges[i].start, ranges[i].end);
        if (o < 0)
            return -1;
        if (o) {
            any = 1;
            if (!first[i])
                first[i] = code;
        }
    }
    return any;
}

static int save_range(struct loader_memory_report *r, uint64_t base, uint64_t size)
{
    if (!size || size > UINT64_MAX - base || r->ledger.count == SWITCH_RAM_RANGES)
        return 1;
    r->ledger.reserved[r->ledger.count++] = (struct switch_ram_range){base, base + size};
    return 0;
}

// This deliberately accepts the standard pinned FDT layout, including its
// complete reserve table and explicit /reserved-memory children. No writes,
// allocations or arbitrary property-address dereferences occur here.
/* One walk for every range (structural failures do not depend on them). first[i] gets the overlap code (4 or 20) of
 * the first reservation that overlaps range i; the walk goes on past overlaps, so first[] holds every range's. */
static int walk(const uint8_t *fdt, uint32_t capacity, const struct loader_memory_range *ranges, unsigned count,
                int *first, struct loader_memory_report *r)
{
    r->reservations = r->reserved_nodes = r->required_reservations = r->ramoops_seen = 0;
    r->bank_count = 0;
    _Static_assert(sizeof(r->ledger) % 8 == 0, "ledger is cleared in 64-bit words");
    volatile uint64_t *clear = (volatile uint64_t *)(void *)&r->ledger;
    for (unsigned i = 0; i < sizeof(r->ledger) / 8; ++i)
        clear[i] = 0;
    r->ledger.magic = SWITCH_RAM_MAGIC;
    r->ledger.version = 1;
    if (capacity < 40 || capacity > 0x40000 || !count)
        return 1;
    for (unsigned i = 0; i < count; ++i)
        if (ranges[i].start >= ranges[i].end)
            return 1;
    if (be32(fdt) != 0xd00dfeed)
        return 1;
    uint32_t total = be32(fdt + 4), structure = be32(fdt + 8), strings = be32(fdt + 12), reserve = be32(fdt + 16);
    uint32_t ssize = be32(fdt + 32), tsize = be32(fdt + 36);
    if (total < 40 || total > capacity || be32(fdt + 20) != 17 || be32(fdt + 24) > 17 || reserve < 40 ||
        reserve > total || reserve % 8 || structure % 4 || structure > total || structure < reserve ||
        structure - reserve < 16 || tsize > total - structure || strings < structure + tsize || strings > total ||
        ssize > total - strings)
        return 2;
    uint32_t at = reserve;
    for (;;) {
        if (at > structure - 16 || r->reservations >= 64)
            return 3;
        uint64_t base = be64(fdt + at), size = be64(fdt + at + 8);
        at += 16;
        if (!base && !size)
            break;
        if (!size)
            return 4;
        if (overlaps_any(base, size, ranges, count, 4, first)) {
            if (overlaps(base, size, 0, UINT64_MAX) < 0)
                return 4; /* a wrapping reservation stops every walk */
        }
        if (save_range(r, base, size))
            return 24;
        if (base <= UINT64_C(0x80000000) && base + size >= UINT64_C(0x80020000))
            r->required_reservations |= 1;
        if (base <= UINT64_C(0xf5a00000) && base + size >= UINT64_C(0xf5e00000))
            r->required_reservations |= 2;
        if (base <= UINT64_C(0x8d000000) && base + size >= UINT64_C(0x8d000000) + total)
            r->required_reservations |= 4;
        ++r->reservations;
    }
    if (r->required_reservations != 7)
        return 5;
    uint32_t pos = structure, limit = structure + tsize, depth = 0;
    uint32_t reserved = 0, reserved_seen = 0, child_reg = 0, root_cells = 0, reserved_cells = 0, empty_ranges = 0;
    uint32_t root_seen = 0, root_ended = 0, memory_node = 0, memory_reg = 0;
    while (pos <= limit && limit - pos >= 4) {
        uint32_t token = be32(fdt + pos);
        pos += 4;
        if (token == 1) {
            uint32_t name = pos;
            pos = find_nul(fdt, pos, limit);
            if (pos == limit || depth >= 64 || root_ended)
                return 6;
            ++depth;
            if (depth == 1) {
                if (root_seen++ || pos != name)
                    return 7;
            }
            if (depth == 2 && name_is(fdt + name, pos - name, "reserved-memory")) {
                if (reserved_seen++)
                    return 8;
                reserved = depth;
            } else if (reserved && depth == 3) {
                child_reg = 0;
                if (++r->reserved_nodes > 32)
                    return 9;
            } else if (reserved && depth > 3)
                return 10;
            if (depth == 2 &&
                (name_is(fdt + name, pos - name, "memory") || (pos - name > 7 && name_is(fdt + name, 7, "memory@"))))
                memory_node = depth;
            pos = (pos + 4) & ~3u;
        } else if (token == 2) {
            if (!depth)
                return 11;
            if (reserved && depth == 3 && !child_reg)
                return 12;
            if (reserved && depth == 2)
                reserved = 0;
            if (memory_node && depth == 2)
                memory_node = 0;
            if (depth == 1)
                root_ended = 1;
            --depth;
        } else if (token == 3) {
            if (!depth || limit - pos < 8)
                return 13;
            uint32_t size = be32(fdt + pos), key = be32(fdt + pos + 4);
            pos += 8;
            if (size > limit - pos || key >= ssize)
                return 14;
            uint32_t k = key;
            k = find_nul(fdt + strings, k, ssize);
            if (k == ssize)
                return 15;
            const uint8_t *name = fdt + strings + key;
            uint32_t n = k - key;
            if (memory_node && depth == 2 && name_is(name, n, "reg")) {
                if (memory_reg++ || (size != 32 && size != 48))
                    return 25;
                static const uint64_t expected[4] = {0x80000000ULL, 0x7fd00000ULL, 0x100000000ULL, 0x80000000ULL};
                for (unsigned i = 0; i < 4; i++)
                    if (be64(fdt + pos + i * 8) != expected[i])
                        return 26;
                if (size == 48 && (be64(fdt + pos + 32) || be64(fdt + pos + 40)))
                    return 26;
                for (unsigned i = 0; i < 2; i++)
                    r->ledger.banks[i] =
                        (struct switch_ram_range){expected[i * 2], expected[i * 2] + expected[i * 2 + 1]};
                r->bank_count = 2;
            }
            if (depth == 1 || (reserved && depth == 2)) {
                uint32_t bit = 0;
                if (name_is(name, n, "#address-cells"))
                    bit = 1;
                if (name_is(name, n, "#size-cells"))
                    bit = 2;
                if (bit) {
                    uint32_t *flags = depth == 1 ? &root_cells : &reserved_cells;
                    if (size != 4 || be32(fdt + pos) != 2 || (*flags & bit))
                        return 16;
                    *flags |= bit;
                }
            }
            if (reserved && depth == 2 && name_is(name, n, "ranges")) {
                if (size || empty_ranges++)
                    return 17;
            }
            if (reserved && depth == 3) {
                if (name_is(name, n, "size") || name_is(name, n, "alloc-ranges"))
                    return 18;
                if (name_is(name, n, "reg")) {
                    if (child_reg++ || !size || size % 16 || size > 256)
                        return 19;
                    for (uint32_t i = 0; i < size; i += 16) {
                        uint64_t base = be64(fdt + pos + i), bytes = be64(fdt + pos + i + 8);
                        const int o = overlaps_any(base, bytes, ranges, count, 20, first);
                        if (o < 0)
                            return 20;
                        if (bytes && save_range(r, base, bytes))
                            return 24;
                        if (base == UINT64_C(0xb0000000) && bytes == UINT64_C(0x200000))
                            r->ramoops_seen = 1;
                    }
                }
            }
            pos = (pos + size + 3) & ~3u;
        } else if (token == 4) {
            continue;
        } else if (token == 9) {
            if (depth || !root_ended || root_cells != 3 || reserved_seen != 1 || reserved_cells != 3 ||
                empty_ranges != 1 || !r->ramoops_seen)
                return 21;
            return 0;
        } else
            return 22;
    }
    return 23;
}

int loader_check_fdt_memory_ranges(const uint8_t *fdt, uint32_t capacity, const struct loader_memory_range *ranges,
                                   unsigned count, struct loader_memory_report *r, unsigned *range)
{
    int first[LOADER_MEMORY_RANGES_MAX];
    *range = 0;
    if (count > LOADER_MEMORY_RANGES_MAX)
        count = 0; /* walk() refuses it as a bad argument */
    for (unsigned i = 0; i < LOADER_MEMORY_RANGES_MAX; ++i)
        first[i] = 0;
    const int failure = walk(fdt, capacity, ranges, count, first, r);
    /* What the calls one range at a time, in index order, would have stopped at: range 0's walk ends at the earlier
     * of its first overlap and the first structural failure, which (if any) every range's walk reaches. */
    if (failure) {
        if (count && first[0])
            return first[0];
        return failure;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (first[i]) {
            *range = i;
            return first[i];
        }
    }
    return 0;
}

int loader_check_fdt_memory(const uint8_t *fdt, uint32_t capacity, uint64_t start, uint64_t end,
                            struct loader_memory_report *r)
{
    const struct loader_memory_range range = {start, end};
    unsigned index;
    return loader_check_fdt_memory_ranges(fdt, capacity, &range, 1, r, &index);
}
