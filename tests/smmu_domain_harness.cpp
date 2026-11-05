// drivers/TegraSMMU/TegraSMMUDomain.h on the host: entry encodings, table hand-out, identity maps, page maps, translation,
// limits and dirty tracking; and the tables TegraVIC builds with it, byte for byte against the algorithm TegraVIC used
// before it moved onto the shared domain (r14-pacing), so VIC's address space is unchanged.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../out/native-a57/root-v1/tree/drivers/TegraSMMU/TegraSMMUDomain.h"

namespace {
int failures = 0;
#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            if (failures++ < 20) {                               \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                std::printf(__VA_ARGS__);                        \
                std::printf("\n");                               \
            }                                                    \
        }                                                        \
    } while (0)

constexpr uint32_t R = 1u << 31, W = 1u << 30, NS = 1u << 29, NEXT = 1u << 28;

struct Buffer {
    std::vector<uint32_t> words;
    uint64_t pa;
    Buffer(uint32_t pages, uint64_t at) : words((size_t)pages * 1024, 0xdeadbeef), pa(at) {}
    uint32_t *va() { return words.data(); }
};

// TegraVIC's tables before r14-pacing (TegraVIC.cpp at eae4371: smmuSetUp, pte, identity), on its own buffer.
struct OldVic {
    static constexpr uint64_t kWindowBase = 0x10000000ull, kWindowBytes = 128ull << 20;
    static constexpr uint32_t kWindows = 2, kWindowTables = (uint32_t)(kWindowBytes >> 22), kIdentityTables = 4;
    Buffer b;
    uint32_t identityUsed = 0;
    explicit OldVic(uint64_t pa) : b(1 + kWindows * kWindowTables + kIdentityTables, pa)
    {
        std::fill(b.words.begin(), b.words.end(), 0);  // contiguous() zeroes the buffer
    }
    uint32_t *table(uint32_t i) { return b.va() + (size_t)i * 1024; }
    uint64_t tablePa(uint32_t i) { return b.pa + (uint64_t)i * 4096; }
    uint32_t *pte(uint64_t iova)
    {
        uint32_t *pd = table(0);
        const uint32_t pdi = (uint32_t)(iova >> 22);
        if (!(pd[pdi] & NEXT)) {
            if (identityUsed == kIdentityTables) return nullptr;
            const uint32_t index = 1 + kWindows * kWindowTables + identityUsed++;
            pd[pdi] = (uint32_t)(tablePa(index) >> 12) | R | W | NS | NEXT;
        }
        const uint64_t index = (((uint64_t)(pd[pdi] & 0x3fffff) << 12) - b.pa) / 4096;
        return &table((uint32_t)index)[(iova >> 12) & 1023];
    }
    bool identity(uint64_t pa, uint64_t size, bool writable)
    {
        for (uint64_t at = pa; at < pa + size; at += 4096) {
            uint32_t *p = pte(at);
            if (!p) return false;
            *p = (uint32_t)(at >> 12) | R | NS | (writable ? W : 0);
        }
        return true;
    }
    bool setUp(uint64_t fwPa, uint64_t fwSize, uint64_t arenaPa, uint64_t arenaSize)
    {
        uint32_t *pd = table(0);
        for (uint32_t w = 0; w < kWindows; w++)
            for (uint32_t t = 0; t < kWindowTables; t++) {
                const uint64_t iova = kWindowBase + w * kWindowBytes + ((uint64_t)t << 22);
                pd[iova >> 22] = (uint32_t)(tablePa(1 + w * kWindowTables + t) >> 12) | R | W | NS | NEXT;
            }
        return identity(fwPa, fwSize, false) && identity(arenaPa, arenaSize, true);
    }
};

// TegraVIC's tables now (TegraVIC.cpp smmuSetUp).
bool newVic(Buffer &b, uint64_t fwPa, uint64_t fwSize, uint64_t arenaPa, uint64_t arenaSize)
{
    TegraSMMUDomain d;
    if (!d.init(b.va(), b.pa, (uint32_t)(b.words.size() / 1024))) return false;
    for (uint32_t w = 0; w < OldVic::kWindows; w++)
        if (!d.reserve(OldVic::kWindowBase + w * OldVic::kWindowBytes, OldVic::kWindowBytes)) return false;
    return d.identity(fwPa, fwSize, false) && d.identity(arenaPa, arenaSize, true);
}

void vicTablesUnchanged()
{
    struct Case {
        uint64_t tables, fw, fwSize, arena, arenaSize;
    } cases[] = {
        {0xe1000000, 0xe1200000, 0x14000, 0xe1300000, 1 << 20},   // all in one 4 MiB section
        {0xf0000000, 0xf03fe000, 0x14000, 0xf07ff000, 1 << 20},   // firmware and arena across section boundaries
        {0x90000000, 0xa0000000, 0x40000, 0xb0000000, 1 << 20},   // far apart
    };
    for (const Case &c : cases) {
        OldVic old(c.tables);
        Buffer now(1 + OldVic::kWindows * OldVic::kWindowTables + OldVic::kIdentityTables, c.tables);
        std::fill(now.words.begin(), now.words.end(), 0);  // contiguous() zeroes VIC's buffer too
        const bool a = old.setUp(c.fw, c.fwSize, c.arena, c.arenaSize), b = newVic(now, c.fw, c.fwSize, c.arena, c.arenaSize);
        CHECK(a && b, "VIC set-up failed (old %d new %d) for tables %#llx", a, b, (unsigned long long)c.tables);
        CHECK(old.b.words == now.words, "VIC's tables differ from the old algorithm's for tables at %#llx",
              (unsigned long long)c.tables);
    }
    // Out of identity tables: both refuse.
    OldVic old(0xe1000000);
    Buffer now(1 + OldVic::kWindows * OldVic::kWindowTables + OldVic::kIdentityTables, 0xe1000000);
    const bool a = old.setUp(0x90000000, 0x14000, 0xa0000000, 20ull << 20), b = newVic(now, 0x90000000, 0x14000, 0xa0000000, 20ull << 20);
    CHECK(!a && !b, "a set-up past the identity tables must fail both ways (old %d new %d)", a, b);
}

void basics()
{
    Buffer b(8, 0xe2000000);
    TegraSMMUDomain d;
    CHECK(!d.init(b.va(), 0xe2000800, 8), "unaligned table buffer accepted");
    CHECK(!d.init(b.va(), 0xfffff000, 8), "table buffer past 4 GiB accepted");
    CHECK(d.init(b.va(), b.pa, 8), "init");
    for (uint32_t i = 0; i < 1024; ++i) CHECK(b.words[i] == 0, "directory entry %u not cleared", i);
    CHECK(d.ptb() == ((uint32_t)(b.pa >> 12) | R | W | NS), "PTB %#x", d.ptb());
    CHECK(d.translate(0x10000000) == UINT64_MAX, "unmapped address translates");

    // identity, read-only and writable entries
    CHECK(d.identity(0xf5a00000, 0x3c0000, false), "identity of the boot framebuffer");
    CHECK(d.translate(0xf5a00000) == 0xf5a00000 && d.translate(0xf5dbfff8) == 0xf5dbfff8, "identity translation");
    CHECK(d.translate(0xf5dc0000) == UINT64_MAX, "identity ran past its end");
    CHECK(*d.pte(0xf5a00000, false) == (0xf5a00u | R | NS), "read-only entry %#x", *d.pte(0xf5a00000, false));
    CHECK(d.identity(0xe3000000, 4096, true) && *d.pte(0xe3000000, false) == (0xe3000u | R | W | NS), "writable entry");

    // a scattered surface into a slot, then out
    uint64_t pages[5] = {0x81234000, 0x9abc0000, 0x1fffff000ull & 0xfffff000, 0xc0001000, 0x80002000};
    CHECK(d.map(0x10000000, pages, 5, false), "map");
    for (uint32_t i = 0; i < 5; ++i)
        CHECK(d.translate(0x10000000 + i * 4096ull + 0x123) == pages[i] + 0x123, "page %u translates wrong", i);
    d.unmap(0x10000000, 5);
    CHECK(d.translate(0x10000000) == UINT64_MAX && d.translate(0x10004000) == UINT64_MAX, "unmap left entries");

    // limits: 4 GiB, and running out of tables
    CHECK(!d.pte(1ull << 32, true), "address past 4 GiB accepted");
    uint32_t got = 0;
    for (uint64_t at = 0x40000000; at < 0x80000000 && d.reserve(at, 4096); at += 4ull << 20) ++got;
    // directory + 2 (the framebuffer spans two 4 MiB sections) + 1 (0xe3000000) + 1 (the slot) = 5 used before, 3 left
    CHECK(d.used == d.pages && got == 3, "tables handed out %u (used %u of %u)", got, d.used, d.pages);
    CHECK(!d.identity(0x70000000, 4096, true), "a map with no table left succeeded");

    // dirty tracking covers what was written
    Buffer c(4, 0xe4000000);
    TegraSMMUDomain e;
    uintptr_t low, high;
    e.init(c.va(), c.pa, 4);
    e.takeDirty(&low, &high);
    CHECK(!e.takeDirty(&low, &high), "dirty after take");
    e.identity(0x88000000, 8192, true);
    CHECK(e.takeDirty(&low, &high), "identity not dirty");
    const uintptr_t pde = reinterpret_cast<uintptr_t>(&c.words[0x88000000u >> 22]);
    const uintptr_t pte = reinterpret_cast<uintptr_t>(e.pte(0x88001000, false));
    CHECK(low <= pde && high >= pde + 4 && low <= pte && high >= pte + 4, "dirty range misses the entries");

    // consecutive tables for a reserved range
    Buffer f(1 + 32 + 1, 0xe5000000);
    TegraSMMUDomain g;
    g.init(f.va(), f.pa, 1 + 32 + 1);
    CHECK(g.reserve(0x10000000, 128ull << 20), "reserve a window");
    for (uint32_t t = 0; t < 32; ++t)
        CHECK(f.words[(0x10000000u >> 22) + t] == (((uint32_t)(f.pa >> 12) + 1 + t) | R | W | NS | NEXT),
              "window table %u not consecutive", t);
}
}  // namespace

int main()
{
    basics();
    vicTablesUnchanged();
    if (failures) {
        std::printf("FAIL: smmu domain (%d failures)\n", failures);
        return 1;
    }
    std::printf("PASS: smmu domain\n");
    return 0;
}
