// loader_check_fdt_memory (one walk for several ranges, word reads) against the single-range reference parser in
// memory_reference.c: on generated and randomly damaged FDTs (fdt_builder.h), every
// single-range status is unchanged, and so is the whole report whenever it passes (the only time the loader uses it),
// and the multi-range result is what the single-range calls in index order would have stopped at.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <random>

#include "fdt_builder.h"

extern "C" {
#include "../memory.h"
int reference_loader_check_fdt_memory(const uint8_t *fdt, uint32_t capacity, uint64_t window_start, uint64_t window_end,
                                      struct loader_memory_report *report);
}

using fdt::Bytes;
using fdt::Cells;
using fdt::Node;
using fdt::Reservation;
using fdt::Tree;

namespace {
constexpr size_t REPORT_BYTES = 4096;  // larger than struct loader_memory_report
static_assert(sizeof(loader_memory_report) <= REPORT_BYTES);

struct Rng {
    std::mt19937_64 engine{1104};
    uint64_t below(uint64_t n) { return std::uniform_int_distribution<uint64_t>(0, n - 1)(engine); }
    template <class T> const T &choice(const std::vector<T> &v) { return v[below(v.size())]; }
};

// The generated and damaged trees: the default one, 300 with shuffled extra reservations and carve-outs, and 300
// copies of the default with one to three random bytes overwritten.
std::vector<Bytes> Trees(Rng &rng)
{
    std::vector<Bytes> trees{Tree()};
    for (int t = 0; t < 300; ++t) {
        std::vector<Reservation> reservations = fdt::DefaultReservations();
        for (uint64_t n = rng.below(4); n; --n)
            reservations.push_back(
                {rng.choice(std::vector<uint64_t>{0xA0000000, 0xAA000000, 0xB1000000, 0xB8000000, 0xE0000000}) +
                     rng.below(0x2000000 / 0x1000) * 0x1000,
                 rng.choice(std::vector<uint64_t>{0x1000, 0x200000, 0x3000000})});
        std::shuffle(reservations.begin(), reservations.end(), rng.engine);
        std::vector<Node> children{{"ramoops", {{"reg", Cells({0xB0000000, 0x200000})}}, {}}};
        const uint64_t carves = rng.below(4);
        for (uint64_t n = 0; n < carves; ++n) {
            const uint64_t base = rng.choice(std::vector<uint64_t>{0xA001E000, 0xAA100000, 0xC0000000, 0xFFE00000});
            const uint64_t size = rng.choice(std::vector<uint64_t>{0x1000, 0x100000});
            children.push_back({"carve" + std::to_string(n), {{"reg", Cells({base, size})}}, {}});
        }
        trees.push_back(Tree({.reservations = reservations, .children = children}));
    }
    const Bytes base = Tree();
    for (int t = 0; t < 300; ++t) {
        Bytes damaged = base;
        for (uint64_t n = 1 + rng.below(3); n; --n) damaged[rng.below(damaged.size())] = (uint8_t)rng.below(256);
        trees.push_back(damaged);
    }
    return trees;
}
}  // namespace

TEST(LoaderMemoryRanges, SingleAndMultiRangeMatchThePreviousParser)
{
    Rng rng;
    const std::vector<loader_memory_range> candidates{{0xB2000000, 0xE2200000}, {0xA001F000, 0xA0020000},
                                                      {0xAA100000, 0xAAF43000}, {0xA0000000, 0xA001D000},
                                                      {0xA001EF00, 0xA001F000}, {0xB0000000, 0xB0001000}};
    unsigned checked = 0, multis = 0;
    for (const Bytes &data : Trees(rng)) {
        // The tree and 8 zero bytes after it, its own length declared.
        auto buffer = std::make_unique<uint8_t[]>(data.size() + 8);
        std::memset(buffer.get(), 0, data.size() + 8);
        std::memcpy(buffer.get(), data.data(), data.size());
        std::vector<loader_memory_range> picks = candidates;
        std::shuffle(picks.begin(), picks.end(), rng.engine);
        picks.resize(1 + rng.below(candidates.size()));
        std::optional<std::pair<int, unsigned>> expected;
        for (unsigned index = 0; index < picks.size(); ++index) {
            alignas(8) unsigned char current[REPORT_BYTES] = {}, previous[REPORT_BYTES] = {};
            const int now = loader_check_fdt_memory(buffer.get(), (uint32_t)data.size(), picks[index].start,
                                                    picks[index].end, (loader_memory_report *)current);
            const int before =
                reference_loader_check_fdt_memory(buffer.get(), (uint32_t)data.size(), picks[index].start,
                                                  picks[index].end, (loader_memory_report *)previous);
            EXPECT_EQ(now, before) << std::hex << "range " << picks[index].start << ", tree of " << std::dec
                                   << data.size() << " bytes";
            if (!before) EXPECT_EQ(std::memcmp(current, previous, REPORT_BYTES), 0) << "the passing walk's report";
            ++checked;
            if (!expected && before) expected = std::make_pair(before, index);
        }
        unsigned failed = 99;
        alignas(8) unsigned char report[REPORT_BYTES] = {};
        const int status =
            loader_check_fdt_memory_ranges(buffer.get(), (uint32_t)data.size(), picks.data(), (unsigned)picks.size(),
                                           (loader_memory_report *)report, &failed);
        if (expected) {
            EXPECT_EQ(status, expected->first);
            EXPECT_EQ(failed, expected->second);
        } else {
            EXPECT_EQ(status, 0);
        }
        ++multis;
    }
    EXPECT_GT(checked, 1000u);
    std::printf("%u single-range and %u multi-range checks\n", checked, multis);
}
