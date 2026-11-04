// Host test of drivers/NativeIOSwitchBlock/IOSwitchAdma.h: the SD driver's ADMA2 descriptor tables, built from a
// request chain's physical segments (test_native_sd_adma.py compiles and runs this).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

typedef uint64_t IOByteCount;
constexpr uint32_t kIOMemoryMapperNone = 0x800;

#include "IOSwitchAdma.h"

// A memory descriptor as getPhysicalSegment sees it: physical runs in order (a UPL's pages, a contiguous buffer).
struct Run {
    uint64_t pa, bytes;
};
struct Memory {
    std::vector<Run> runs;
    int missingAt = -1;  // run index getPhysicalSegment pretends is not there
    uint64_t getPhysicalSegment(IOByteCount offset, IOByteCount *length, uint32_t options)
    {
        assert(options == kIOMemoryMapperNone);
        for (size_t i = 0; i < runs.size(); ++i) {
            if (offset < runs[i].bytes) {
                if ((int)i == missingAt) break;
                *length = runs[i].bytes - offset;
                return runs[i].pa + offset;
            }
            offset -= runs[i].bytes;
        }
        *length = 0;
        return 0;
    }
};

static bool anyPage(uint64_t) { return true; }

struct Table {
    std::vector<IOSwitchAdmaDescriptor> entries;
    IOSwitchAdmaTable t;
    explicit Table(uint32_t capacity = 1024, uint64_t limit = 1ull << 34) : entries(capacity + 1)
    {
        t = {entries.data(), capacity, limit, 0, 0};
        // A guard entry past the capacity: the builder must never write it.
        entries[capacity] = {0xdead, 0xbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef};
    }
    ~Table()
    {
        const IOSwitchAdmaDescriptor &guard = entries[t.capacity];
        assert(guard.attributes == 0xdead && guard.length == 0xbeef && guard.reserved == 0xdeadbeef);
    }
};

// Every descriptor of a finished table: a valid transfer of whole lines, 1..kAdmaMaxLength bytes (never 0: Tegra's
// SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC), reserved 0, End on the last one only; and the bytes it moves are exactly
// `expected` in order (the runs as given, in 64-byte lines).
static void checkTable(const IOSwitchAdmaTable &t, const std::vector<Run> &expected)
{
    std::vector<uint64_t> want, got;
    for (const Run &r : expected)
        for (uint64_t at = 0; at < r.bytes; at += kAdmaLine) want.push_back(r.pa + at);
    assert(t.count >= 1);
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < t.count; ++i) {
        const IOSwitchAdmaDescriptor &d = t.entries[i];
        assert(d.attributes == (i + 1 == t.count ? 0x23 : 0x21));
        assert(d.length >= 1 && d.length <= kAdmaMaxLength && d.length % kAdmaLine == 0);
        assert(d.reserved == 0);
        const uint64_t pa = ioswitchAdmaAddress(d);
        assert(pa % kAdmaLine == 0 && pa + d.length <= t.limit);
        for (uint64_t at = 0; at < d.length; at += kAdmaLine) got.push_back(pa + at);
        bytes += d.length;
    }
    assert(bytes == t.bytes);
    assert(got == want);
}

static void layout()
{
    // 128 bits, little-endian: attributes, length, address low, address high, reserved (L4T sdhci_adma2_64_desc + pad).
    IOSwitchAdmaDescriptor d = {0x23, 0xf000, 0x89abcdc0, 0x1, 0};
    uint8_t raw[16];
    memcpy(raw, &d, sizeof(raw));
    const uint8_t expect[16] = {0x23, 0, 0x00, 0xf0, 0xc0, 0xcd, 0xab, 0x89, 1, 0, 0, 0, 0, 0, 0, 0};
    assert(!memcmp(raw, expect, sizeof(raw)));
    assert(ioswitchAdmaAddress(d) == 0x189abcdc0ull);
}

static void singlePages()
{
    Table low;
    assert(low.t.add(0x80001000, 0x1000, anyPage) == kAdmaOk && low.t.finish());
    assert(low.t.count == 1 && low.t.entries[0].addressHigh == 0 && low.t.entries[0].addressLow == 0x80001000);
    checkTable(low.t, {{0x80001000, 0x1000}});

    // Above 4 GiB: the high word carries bit 32 (this board's RAM runs to 0x17fffffff).
    Table high;
    assert(high.t.add(0x123456000ull, 0x1000, anyPage) == kAdmaOk && high.t.finish());
    assert(high.t.entries[0].addressHigh == 1 && high.t.entries[0].addressLow == 0x23456000);
    checkTable(high.t, {{0x123456000ull, 0x1000}});

    // Sub-page, line-aligned pieces (a 512-byte sector at a 64-byte offset) are fine.
    Table piece;
    assert(piece.t.add(0x80001040, 0x200, anyPage) == kAdmaOk && piece.t.finish());
    checkTable(piece.t, {{0x80001040, 0x200}});
}

static void splitsAtTheLengthCap()
{
    // 1 MiB contiguous: 17 full 60 KiB descriptors and 4 KiB.
    Table one;
    assert(one.t.add(0x100000000ull, 0x100000, anyPage) == kAdmaOk && one.t.finish());
    assert(one.t.count == 18);
    for (uint32_t i = 0; i < 17; ++i) assert(one.t.entries[i].length == kAdmaMaxLength);
    assert(one.t.entries[17].length == 0x1000);
    checkTable(one.t, {{0x100000000ull, 0x100000}});

    // The same MiB as 256 physically contiguous 4 KiB segments (a UPL page by page): the same 18 descriptors.
    Memory pages;
    for (uint64_t i = 0; i < 256; ++i) pages.runs.push_back({0x100000000ull + i * 0x1000, 0x1000});
    Table merged;
    assert(merged.t.addMemory(&pages, 0x100000, anyPage) == kAdmaOk && merged.t.finish());
    assert(merged.t.count == 18);
    checkTable(merged.t, {{0x100000000ull, 0x100000}});

    // Exactly at the cap, one line past it, twice it.
    Table exact, past, twice;
    assert(exact.t.add(0x90000000, kAdmaMaxLength, anyPage) == kAdmaOk && exact.t.finish() && exact.t.count == 1);
    assert(past.t.add(0x90000000, kAdmaMaxLength + 64, anyPage) == kAdmaOk && past.t.finish() && past.t.count == 2);
    assert(past.t.entries[1].length == 64);
    assert(twice.t.add(0x90000000, 2 * kAdmaMaxLength, anyPage) == kAdmaOk && twice.t.finish() && twice.t.count == 2);
    checkTable(past.t, {{0x90000000, kAdmaMaxLength + 64}});

    // A run that continues the previous one tops it up to the cap, then starts a new descriptor.
    Table topUp;
    assert(topUp.t.add(0x90000000, kAdmaMaxLength - 64, anyPage) == kAdmaOk);
    assert(topUp.t.add(0x90000000 + kAdmaMaxLength - 64, 128, anyPage) == kAdmaOk && topUp.t.finish());
    assert(topUp.t.count == 2 && topUp.t.entries[0].length == kAdmaMaxLength && topUp.t.entries[1].length == 64);
    checkTable(topUp.t, {{0x90000000, kAdmaMaxLength + 64}});

    // Pages in falling physical order never merge.
    Memory falling;
    for (uint64_t i = 0; i < 8; ++i) falling.runs.push_back({0x90008000 - i * 0x1000, 0x1000});
    Table fall;
    assert(fall.t.addMemory(&falling, 0x8000, anyPage) == kAdmaOk && fall.t.finish() && fall.t.count == 8);
    checkTable(fall.t, falling.runs);
}

static void refusals()
{
    // Not on a cache line, at either end: a read's invalidate would hit a neighbour.
    Table a, b, c;
    assert(a.t.add(0x80001020, 0x200, anyPage) == kAdmaAlign);
    assert(b.t.add(0x80001000, 0x1f0, anyPage) == kAdmaAlign);
    assert(c.t.add(0x80001000, 0, anyPage) == kAdmaMissing);

    // The address limit: up to it is fine, past it not (a run is never split across it).
    Table edge(1024, 1ull << 32), over(1024, 1ull << 32), above(1024, 1ull << 32), wide(1024, 1ull << 34);
    assert(edge.t.add(0xfffff000ull, 0x1000, anyPage) == kAdmaOk);
    assert(over.t.add(0xfffff000ull, 0x2000, anyPage) == kAdmaHigh && over.t.count == 0);
    assert(above.t.add(0x100000000ull, 0x1000, anyPage) == kAdmaHigh);
    assert(wide.t.add(0x17ffff000ull, 0x1000, anyPage) == kAdmaOk);

    // A page that is not cacheable RAM, anywhere in a run (also a run that only touches its first or last line).
    auto notPage3 = [](uint64_t page) { return page != 0x100003000ull; };
    Table run, firstLine, lastLine, before;
    assert(run.t.add(0x100000000ull, 0x8000, notPage3) == kAdmaMemory);
    assert(firstLine.t.add(0x100002fc0ull, 0x80, notPage3) == kAdmaMemory);
    assert(lastLine.t.add(0x100003fc0ull, 0x40, notPage3) == kAdmaMemory);
    assert(before.t.add(0x100002000ull, 0x1000, notPage3) == kAdmaOk);
    std::vector<uint64_t> asked;
    Table pages;
    assert(pages.t.add(0x100000040ull, 0x2000, [&](uint64_t page) {
        asked.push_back(page);
        return true;
    }) == kAdmaOk);
    assert((asked == std::vector<uint64_t>{0x100000000ull, 0x100001000ull, 0x100002000ull}));

    // The table's capacity: exactly full is fine, one more is not (and the guard entry stays untouched).
    Memory five;
    for (uint64_t i = 0; i < 5; ++i) five.runs.push_back({0x90000000 + i * 0x2000, 0x1000});
    Table four(4), fits(5);
    assert(four.t.addMemory(&five, 0x5000, anyPage) == kAdmaFull && four.t.count == 4);
    assert(fits.t.addMemory(&five, 0x5000, anyPage) == kAdmaOk && fits.t.finish() && fits.t.count == 5);
    // Splitting counts too: 2 x 60 KiB + 64 bytes needs 3.
    Table two(2);
    assert(two.t.add(0x90000000, 2 * kAdmaMaxLength + 64, anyPage) == kAdmaFull && two.t.count == 2);

    // A segment the descriptor cannot produce, or a descriptor shorter than the request.
    Memory hole = five;
    hole.missingAt = 2;
    Table h, s;
    assert(h.t.addMemory(&hole, 0x5000, anyPage) == kAdmaMissing);
    Memory shortMemory;
    shortMemory.runs = {{0x90000000, 0x1000}};
    assert(s.t.addMemory(&shortMemory, 0x2000, anyPage) == kAdmaMissing);

    // Nothing added: no table to finish.
    Table empty;
    assert(!empty.t.finish());
}

static void chains()
{
    // A merged command: three requests' memory back to back. The last request's run is longer than it needs (the
    // descriptor stops at the request's length). Only the final descriptor carries End.
    Memory first, second, third;
    first.runs = {{0x123400000ull, 0x1000}, {0x80020000, 0x1000}};
    second.runs = {{0x80021000, 0x200}};  // continues the first request's last page physically: merged
    third.runs = {{0x80050000, 0x10000}};
    Table t;
    assert(t.t.addMemory(&first, 0x2000, anyPage) == kAdmaOk);
    assert(t.t.addMemory(&second, 0x200, anyPage) == kAdmaOk);
    assert(t.t.addMemory(&third, 0x1000, anyPage) == kAdmaOk && t.t.finish());
    assert(t.t.count == 3 && t.t.bytes == 0x3200);
    checkTable(t.t, {{0x123400000ull, 0x1000}, {0x80020000, 0x1200}, {0x80050000, 0x1000}});

    // A table is reused: reset() starts the next command afresh.
    t.t.reset();
    assert(t.t.count == 0 && t.t.bytes == 0);
    assert(t.t.addMemory(&third, 0x200, anyPage) == kAdmaOk && t.t.finish());
    checkTable(t.t, {{0x80050000, 0x200}});
}

static void randomRuns()
{
    // Many random requests: line-aligned runs of random lengths anywhere below the limit, often physically continuing
    // the previous one. Every table must move exactly the runs' bytes in order, within every descriptor rule.
    std::mt19937_64 random(0x5d0adaa2);
    for (unsigned round = 0; round < 3000; ++round) {
        // Starts at least 16 MiB below the limit; 40 runs of at most 256 KiB stay below it even when they all continue.
        auto anywhere = [&] { return (random() % ((1ull << 34) / kAdmaLine - 0x40000)) * kAdmaLine; };
        std::vector<Run> runs;
        uint64_t pa = anywhere();
        const unsigned count = 1 + random() % 40;
        for (unsigned i = 0; i < count; ++i) {
            if (random() % 3) pa = anywhere();
            const uint64_t bytes = (1 + random() % (random() % 2 ? 64 : 4096)) * kAdmaLine;
            runs.push_back({pa, bytes});
            pa += bytes;
        }
        Memory memory;
        memory.runs = runs;
        uint64_t total = 0;
        for (const Run &r : runs) total += r.bytes;
        Table t(4096);
        assert(t.t.addMemory(&memory, total, anyPage) == kAdmaOk && t.t.finish());
        checkTable(t.t, runs);
    }
}

int main()
{
    layout();
    singlePages();
    splitsAtTheLengthCap();
    refusals();
    chains();
    randomRuns();
    puts("PASS: sd adma2 tables");
    return 0;
}
