// The bounded FDT reservation parser (boot/loader/memory.c, loader_check_fdt_memory) the loader runs before its
// native writes, on generated trees (fdt_builder.h): the complete map, every required reservation, any reservation or
// /reserved-memory node in the kernel's window, overflow, malformed and dynamic nodes, address formats, and a
// truncated or lying header.
#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "fdt_builder.h"

extern "C" {
#include "../memory.h"
}

using fdt::Bytes;
using fdt::Cells;
using fdt::Node;
using fdt::Options;
using fdt::Reservation;
using fdt::Tree;

namespace {
constexpr uint64_t START = 0xB2000000, END = 0xE2200000;

struct Result {
    int status;
    loader_memory_report report;
};

// The parser over the tree, `capacity` (default: the tree's length) bytes of it declared. The buffer holds the tree, a
// NUL (as the Python test's C string did) and the rest of that last word: memory.c reads aligned 32-bit words only
// (uncached RAM, one read per four bytes), so the word holding the tree's last byte is read whole, by design.
Result Check(const Bytes &data, std::optional<uint32_t> capacity = std::nullopt)
{
    const size_t size = (data.size() + 1 + 3) & ~size_t{3};
    auto buffer = std::make_unique<uint8_t[]>(size);
    std::memset(buffer.get(), 0, size);
    if (!data.empty()) std::memcpy(buffer.get(), data.data(), data.size());
    Result r{};
    r.status =
        loader_check_fdt_memory(buffer.get(), capacity ? *capacity : (uint32_t)data.size(), START, END, &r.report);
    return r;
}

std::vector<Reservation> Good()
{
    return fdt::DefaultReservations();
}
}  // namespace

TEST(LoaderMemoryGate, CompleteMapAndZeroSizedPlaceholders)
{
    const Result r = Check(Tree());
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.report.reservations, 3u);
    EXPECT_EQ(r.report.reserved_nodes, 2u);
    EXPECT_EQ(r.report.required_reservations, 7u);
    EXPECT_EQ(r.report.ramoops_seen, 1u);
}

TEST(LoaderMemoryGate, MissingRequiredReservations)
{
    for (size_t i = 0; i < 3; ++i) {
        std::vector<Reservation> some = Good();
        some.erase(some.begin() + (long)i);
        EXPECT_EQ(Check(Tree({.reservations = some})).status, 5) << "without reservation " << i;
    }
}

TEST(LoaderMemoryGate, ReservationAnywhereInTheRoundedWindowIsRejected)
{
    for (uint64_t start : {START, START + 0x3FFF, START + 0x20000000, END - 1}) {
        std::vector<Reservation> r = Good();
        r.push_back({start, 1});
        EXPECT_EQ(Check(Tree({.reservations = r})).status, 4) << std::hex << start;
    }
    std::vector<Reservation> r = Good();
    r.push_back({START - 1, 1});
    r.push_back({END, 1});
    EXPECT_EQ(Check(Tree({.reservations = r})).status, 0);
}

TEST(LoaderMemoryGate, ReservedNodeOverlapAndOverflowAreRejected)
{
    const Node ramoops{"ramoops", {{"reg", Cells({0xB0000000, 0x200000})}}, {}};
    for (auto [start, size] : {std::pair<uint64_t, uint64_t>{START, 1}, {END - 1, 1}, {UINT64_MAX, 2}}) {
        const std::vector<Node> children{ramoops, {"other", {{"reg", Cells({start, size})}}, {}}};
        EXPECT_EQ(Check(Tree({.children = children})).status, 20) << std::hex << start;
    }
    // An empty interval is not an allocation, regardless of its address.
    const std::vector<Node> empty{ramoops, {"empty", {{"reg", Cells({START, 0})}}, {}}};
    EXPECT_EQ(Check(Tree({.children = empty})).status, 0);
}

TEST(LoaderMemoryGate, ReservationOverflowAndZeroSizeAreRejected)
{
    EXPECT_EQ(Check(Tree({.reservations = std::vector<Reservation>{{UINT64_MAX, 2}}})).status, 4);
    EXPECT_EQ(Check(Tree({.reservations = std::vector<Reservation>{{START, 0}}})).status, 4);
}

TEST(LoaderMemoryGate, DynamicMissingNestedAndMalformedReservedNodes)
{
    const std::vector<Node> children{
        {"dynamic", {{"size", Cells({0x1000})}}, {}},
        {"dynamic", {{"reg", Cells({0, 0})}, {"alloc-ranges", Cells({0, 0})}}, {}},
        {"missing", {}, {}},
        {"bad-length", {{"reg", Bytes(15)}}, {}},
        {"duplicate", {{"reg", Cells({0, 0})}, {"reg", Cells({0, 0})}}, {}},
        {"nested", {{"reg", Cells({0, 0})}}, {{"deeper", {}, {}}}},
    };
    for (const Node &child : children)
        EXPECT_NE(Check(Tree({.children = std::vector<Node>{child}})).status, 0) << child.name;
}

TEST(LoaderMemoryGate, AddressFormatsRangesAndRequiredRamoops)
{
    EXPECT_NE(Check(Tree({.root_cells = 1})).status, 0) << "root #address-cells 1";
    EXPECT_NE(Check(Tree({.reserved_cells = 1})).status, 0) << "reserved-memory #address-cells 1";
    EXPECT_NE(Check(Tree({.ranges = Bytes(16)})).status, 0) << "non-empty ranges";
    EXPECT_NE(Check(Tree({.children = std::vector<Node>{}})).status, 0) << "no ramoops";
    EXPECT_NE(Check(Tree({.include_reserved = false})).status, 0) << "no /reserved-memory";
}

TEST(LoaderMemoryGate, HeaderBoundsAndTruncation)
{
    const Bytes original = Tree();
    for (size_t n : {size_t{0}, size_t{1}, size_t{39}, size_t{40}, original.size() - 1})
        EXPECT_NE(Check(Bytes(original.begin(), original.begin() + (long)n)).status, 0) << n << " bytes";
    for (auto [offset, value] : {std::pair<size_t, uint32_t>{0, 0},
                                 {4, 0x40001},
                                 {8, 0},
                                 {12, 40},
                                 {16, 41},
                                 {20, 16},
                                 {32, 0xFFFFFFFF},
                                 {36, 0xFFFFFFFF}}) {
        Bytes modified = original;
        fdt::PutU32(modified, offset, value);
        EXPECT_NE(Check(modified).status, 0) << "header word at " << offset << " = " << value;
    }
}

TEST(LoaderMemoryGate, ReserveOffsetArithmeticCannotWrap)
{
    for (auto [reserve, structure] :
         {std::pair<uint32_t, uint32_t>{0xfffffff8, 8}, {0xfffffff0, 0}, {0x40000, 0x40010}, {40, 40}, {48, 40}}) {
        Bytes modified = Tree();
        fdt::PutU32(modified, 8, structure);
        fdt::PutU32(modified, 16, reserve);
        EXPECT_EQ(Check(modified).status, 2) << "reserve " << reserve << ", structure " << structure;
    }
}
