// drivers/SwitchUSB's device-mode data path (SwitchXUDC.cpp section "Data endpoints", XudcTransfer.h) on the host: the
// TD planner and the OUT residue reading, from the header the driver uses (r17-iodma).
//
// Planner, over random scatter-gather layouts (whole pages, mbuf-like ranges at odd offsets, pages above 4 GiB, runs
// across 64 KiB boundaries): the pieces cover the request once and in order; a direct piece is exactly the client's
// memory at its offset, below 4 GiB, within one 64 KiB window and at most 64 KiB; every piece but the last is whole
// packets; every cache line an OUT direct piece touches is inside its run; a bounce piece fits a 4 KiB page; the
// residue probe's first piece is one packet. Whole pages below 4 GiB, contiguous or not, are never bounced. OUT data
// through a model of the CPU cache (XudcOutCache): the client reads what the host sent, its neighbours their own.
//
// OUT residue: against a model controller of either kind (residue of the whole TD, as Linux tegra-xudc reads it, or of
// the event's TRB, as xHCI hosts report it), the driver's protocol (probe while unknown, then read each event the
// learned way) gets every transfer's length right, short or full, and learns which kind it is talking to.
//
// Throughput (a cost model, assumptions below): USB2 high speed bulk at ~40 MB/s takes under 10% of one core, against
// the serial bounce path it replaces; the same per-request costs at SuperSpeed rates.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../XudcTransfer.h"

namespace {

constexpr uint64_t kGiB4 = 1ull << 32;
constexpr uint32_t kPage = 4096;

using Runs = std::vector<xudc::Run>;

struct Plan {
    std::vector<xudc::Piece> pieces;
    xudc::PlanStats stats = {};
};

// The planner as the driver calls it (at most 1024 pieces, as the driver's rings and the old harness allowed).
Plan plan(const Runs &runs, uint32_t total, uint32_t mps, bool out = false, bool probe = false, uint64_t reach = kGiB4,
          uint32_t maximum = 1024)
{
    Plan p;
    p.pieces.resize(maximum > 1024 ? 1024 : maximum);
    const uint32_t n = xudc::plan(runs.data(), static_cast<uint32_t>(runs.size()), total, mps, out, probe, reach,
                                  p.pieces.data(), static_cast<uint32_t>(p.pieces.size()), &p.stats);
    p.pieces.resize(n);
    return p;
}

// The client's physical address at `offset` of the request and what is left of its run there.
std::optional<std::pair<uint64_t, uint32_t>> paAt(const Runs &runs, uint32_t offset)
{
    uint32_t start = 0;
    for (const xudc::Run &r : runs) {
        if (offset < start + r.length)
            return std::make_pair(r.pa + (offset - start), start + r.length - offset);
        start += r.length;
    }
    return std::nullopt;
}

// The request offset where the run holding `offset` starts.
uint32_t runOffset(const Runs &runs, uint32_t offset)
{
    uint32_t start = 0;
    for (const xudc::Run &r : runs) {
        if (offset < start + r.length)
            return start;
        start += r.length;
    }
    ADD_FAILURE() << "offset " << offset << " beyond the runs";
    return 0;
}

// The Python reference's random.Random calls, over a fixed-seed generator.
struct Rng {
    std::mt19937_64 engine;

    explicit Rng(uint64_t seed) : engine(seed)
    {}

    uint64_t randrange(uint64_t n)
    {
        return std::uniform_int_distribution<uint64_t>(0, n - 1)(engine);
    }

    uint64_t randint(uint64_t lo, uint64_t hi)
    {
        return std::uniform_int_distribution<uint64_t>(lo, hi)(engine);
    }

    double random()
    {
        return std::uniform_real_distribution<double>(0.0, 1.0)(engine);
    }

    template <class T> T choice(const std::vector<T> &options)
    {
        return options[randrange(options.size())];
    }
};

enum class Kind {
    Pages,
    Mbufs,
    Buffer
};

// Physically contiguous runs (merged as the driver merges them) and their total.
std::pair<Runs, uint32_t> randomLayout(Rng &rng, Kind kind)
{
    Runs runs;
    if (kind == Kind::Pages) {
        for (uint64_t n = rng.randint(1, 24); n; --n) {
            const uint64_t base = rng.choice<uint64_t>({0x8000'0000, 0x1'2000'0000});
            runs.push_back({base + rng.randrange(1u << 18) * kPage, kPage});
        }
    } else if (kind == Kind::Mbufs) {
        for (uint64_t n = rng.randint(1, 12); n; --n) {
            const uint64_t any = rng.randint(1, 16384);
            const auto size = static_cast<uint32_t>(rng.choice<uint64_t>({20, 256, 2048, 4096, 16384, any}));
            const uint64_t base = rng.choice<uint64_t>({0x8000'0000, 0x1'0000'0000});
            runs.push_back({base + rng.randrange(1u << 20) * 64 + rng.randrange(64), size});
        }
    } else {  // one contiguous buffer, anywhere, across 64 KiB boundaries and maybe 4 GiB
        const auto size = static_cast<uint32_t>(rng.randint(1, 300'000));
        const uint64_t below4G = 0xFFFF'0000 - rng.randrange(1u << 16), low = 0x9000'0000 + rng.randrange(1u << 20);
        runs.push_back({rng.choice<uint64_t>({below4G, low}), size});
    }
    Runs merged;
    uint32_t total = 0;
    for (const xudc::Run &r : runs) {
        if (!merged.empty() && merged.back().pa + merged.back().length == r.pa)
            merged.back().length += r.length;
        else
            merged.push_back(r);
        total += r.length;
    }
    return {merged, total};
}

// Plans `runs` and checks every planner invariant on the result (into *result).
void checkPlan(const Runs &runs, uint32_t total, uint32_t mps, bool out, bool probe, Plan *result)
{
    const Plan p = plan(runs, total, mps, out, probe);
    ASSERT_FALSE(p.pieces.empty()) << "planned";
    uint32_t offset = 0, bounced = 0;
    for (size_t k = 0; k < p.pieces.size(); ++k) {
        const xudc::Piece &piece = p.pieces[k];
        SCOPED_TRACE("piece " + std::to_string(k));
        ASSERT_EQ(piece.offset, offset);
        if (k + 1 < p.pieces.size())
            ASSERT_EQ(piece.length % mps, 0u) << "whole packets but in the last TRB";
        if (piece.bounce) {
            ASSERT_LE(piece.length, kPage);
            bounced += piece.length;
        } else {
            const auto at = paAt(runs, piece.offset);
            ASSERT_TRUE(at) << "offset beyond the runs";
            const auto [want, runLeft] = *at;
            ASSERT_EQ(piece.pa, want) << "direct: the client memory itself";
            ASSERT_LE(piece.length, runLeft) << "within one run";
            ASSERT_LE(piece.pa + piece.length, kGiB4);
            ASSERT_LE(piece.length, 65536u);
            ASSERT_EQ(piece.pa >> 16, (piece.pa + piece.length - 1) >> 16) << "no 64 KiB boundary inside a TRB";
            if (out) {  // every cache line it touches inside its run (inside a run, lines are all this transfer's)
                const uint64_t runStart = want - (piece.offset - runOffset(runs, piece.offset)),
                               runEnd = want + runLeft;
                ASSERT_GE(piece.pa & ~63ull, runStart) << "its first line holds memory before the run";
                ASSERT_LE((piece.pa + piece.length + 63) & ~63ull, runEnd)
                    << "its last line holds memory after the run";
            }
        }
        offset += piece.length;
    }
    ASSERT_EQ(offset, total);
    if (probe && total >= 2 * mps)
        ASSERT_EQ(p.pieces[0].length, mps) << "the probe packet alone";
    ASSERT_EQ(p.stats.bounceBytes, bounced);
    if (result)
        *result = p;
}

std::vector<bool> bounces(const Plan &p)
{
    std::vector<bool> b;
    for (const xudc::Piece &piece : p.pieces)
        b.push_back(piece.bounce);
    return b;
}

}  // namespace

TEST(XudcPlanner, RandomLayouts)
{
    Rng rng(7);
    for (unsigned c = 0; c < 3000; ++c) {
        const Kind kind = c % 3 == 0 ? Kind::Pages : c % 3 == 1 ? Kind::Mbufs : Kind::Buffer;
        const auto [runs, total] = randomLayout(rng, kind);
        const auto mps = static_cast<uint32_t>(rng.choice<uint64_t>({512, 512, 64, 1024}));
        const bool out = rng.random() < 0.5;
        const bool probe = out && rng.random() < 0.5;
        SCOPED_TRACE("case " + std::to_string(c) + ": " + std::to_string(runs.size()) + " runs, " +
                     std::to_string(total) + " bytes, mps " + std::to_string(mps) + (out ? " OUT" : " IN") +
                     (probe ? " probe" : ""));
        ASSERT_NO_FATAL_FAILURE(checkPlan(runs, total, mps, out, probe, nullptr));
    }
}

TEST(XudcPlanner, WholePagesGoDirect)
{
    Rng rng(3);
    for (unsigned i = 0; i < 200; ++i) {
        Runs pages;
        for (unsigned k = 0; k < 16; ++k)
            pages.push_back({0x8000'0000 + rng.randrange(1u << 18) * kPage, kPage});
        for (const bool out : {false, true}) {
            Plan p;
            ASSERT_NO_FATAL_FAILURE(checkPlan(pages, 16 * kPage, 512, out, false, &p));
            EXPECT_EQ(p.stats.bounceBytes, 0u);
        }
    }
    Plan p;
    ASSERT_NO_FATAL_FAILURE(checkPlan({{0x9000'0000, 65536}}, 65536, 512, true, false, &p));
    EXPECT_EQ(p.pieces.size(), 1u);  // 64 KiB aligned contiguous: one TRB
    ASSERT_NO_FATAL_FAILURE(checkPlan({{0x9000'8000, 65536}}, 65536, 512, false, false, &p));
    EXPECT_EQ(p.pieces.size(), 2u);  // across a 64 KiB boundary: split there, both direct
    EXPECT_EQ(bounces(p), (std::vector<bool>{false, false}));
}

TEST(XudcPlanner, BouncedOnlyWhatMustBe)
{
    // A page above 4 GiB: that page alone through the pool.
    Plan p;
    ASSERT_NO_FATAL_FAILURE(checkPlan({{0x8000'0000, kPage}, {0x1'0000'0000, kPage}, {0x8000'2000, kPage}}, 3 * kPage,
                                      512, false, false, &p));
    EXPECT_EQ(bounces(p), (std::vector<bool>{false, true, false}));
    EXPECT_EQ(p.stats.high, kPage / 512);
    // An mbuf chain: a 20-byte header then a payload at an odd offset: the packet across the break is copied, the rest
    // goes direct.
    ASSERT_NO_FATAL_FAILURE(checkPlan({{0x8000'0010, 20}, {0x8100'0000, 8192}}, 8212, 512, false, false, &p));
    EXPECT_EQ(p.stats.bounceBytes, 512u);
    EXPECT_EQ(p.stats.discontinuity, 1u);
    // OUT at an odd start: the first packet (cache-line edge) and the short last one are copied.
    ASSERT_NO_FATAL_FAILURE(checkPlan({{0x8000'0020, 4096}}, 4096, 512, true, false, &p));
    EXPECT_EQ(p.stats.edge, 2u);
    // OUT, a 500-byte mbuf then a cluster starting 16 bytes into a line: the packet across the break is copied (12 of
    // its bytes in the cluster), and so is the next one, whose first line holds the 16 bytes before the cluster, and
    // the last, off the cluster's end; the two between go direct.
    ASSERT_NO_FATAL_FAILURE(checkPlan({{0x8000'0000, 500}, {0x8100'0010, 2048}}, 2548, 512, true, false, &p));
    EXPECT_EQ(bounces(p), (std::vector<bool>{true, false, true}));
    EXPECT_EQ(p.stats.discontinuity, 1u);
    EXPECT_EQ(p.stats.edge, 2u) << "the packet beside the break and the last one";
}

TEST(XudcPlanner, TooManyPiecesAndZeroLength)
{
    Runs pages;
    for (uint64_t k = 0; k < 40; ++k)
        pages.push_back({0x8000'0000 + k * 2 * kPage, kPage});
    EXPECT_TRUE(plan(pages, 40 * kPage, 512, false, false, kGiB4, 39).pieces.empty());
    const Plan empty = plan({}, 0, 512);
    ASSERT_EQ(empty.pieces.size(), 1u);
    EXPECT_EQ(empty.pieces[0].pa, 0u);
    EXPECT_EQ(empty.pieces[0].offset, 0u);
    EXPECT_EQ(empty.pieces[0].length, 0u);
    EXPECT_FALSE(empty.pieces[0].bounce);
}

namespace {

// An XUDC receiving `sent` bytes into an OUT TD of TRBs `lengths` (ISP on all, IOC on the last and, when probing, the
// first): the transfer events it raises (TRB index, residue, short packet), residue reported per TD or per TRB.
struct Controller {
    bool perTd;

    // The transfer ends short (a short packet, or a zero-length one on a TRB's end) unless all of it came.
    std::vector<std::tuple<uint32_t, uint32_t, bool>> events(const std::vector<uint32_t> &lengths, uint32_t sent,
                                                             bool probe) const
    {
        uint32_t total = 0, filled = 0;
        for (uint32_t length : lengths)
            total += length;
        std::vector<std::tuple<uint32_t, uint32_t, bool>> out;
        for (uint32_t k = 0; k < lengths.size(); ++k) {
            const uint32_t length = lengths[k], got = std::min(length, sent - filled);
            if (got == length) {  // this TRB full; a zero-length packet then lands in the next one
                filled += length;
                if (k == lengths.size() - 1 || (probe && k == 0))
                    out.emplace_back(k, perTd ? total - filled : 0, false);
                if (filled == total)
                    return out;
                continue;
            }
            out.emplace_back(k, perTd ? total - sent : length - got, true);
            return out;
        }
        return out;
    }
};

// The driver's protocol over 400 random OUT requests; returns what it learned.
uint8_t runProtocol(bool perTd, uint64_t seed)
{
    Rng rng(seed);
    const Controller hw{perTd};
    uint8_t known = xudc::kResidueUnknown;
    for (unsigned i = 0; i < 400; ++i) {
        const uint32_t mps = 512;
        const auto [runs, total] = randomLayout(rng, rng.choice<Kind>({Kind::Pages, Kind::Mbufs}));
        if (total < 2)
            continue;
        const bool probe = known == xudc::kResidueUnknown && total >= 2 * mps;
        const Plan p = plan(runs, total, mps, true, probe);
        std::vector<uint32_t> lengths;
        for (const xudc::Piece &piece : p.pieces)
            lengths.push_back(piece.length);
        if (lengths.size() > 1 && (known == xudc::kResidueBroken || (known == xudc::kResidueUnknown && !probe)))
            continue;  // the serial path: one TRB at a time, nothing to read
        const uint32_t sent = rng.random() < 0.3 ? total : static_cast<uint32_t>(rng.randint(0, total));
        std::optional<uint32_t> result;
        for (const auto &[index, residue, shortPacket] : hw.events(lengths, sent, probe && lengths.size() > 1)) {
            const xudc::OutResult r = xudc::outEvent(known, lengths.data(), static_cast<uint32_t>(lengths.size()),
                                                     index, residue, shortPacket);
            if (r.learned != xudc::kResidueUnknown && r.learned != known && known != xudc::kResidueBroken)
                known = r.learned;
            if (r.done) {
                EXPECT_FALSE(r.failed) << lengths.size() << " TRBs, sent " << sent;
                result = r.actual;
                break;
            }
        }
        EXPECT_EQ(result, std::optional<uint32_t>(sent))
            << lengths.size() << " TRBs of " << total << " bytes, sent " << sent << ", per TD " << perTd;
    }
    return known;
}

}  // namespace

TEST(XudcOutResidue, WholeTdResidue)
{
    EXPECT_EQ(runProtocol(true, 1), xudc::kResiduePerTd);
}

TEST(XudcOutResidue, PerTrbResidue)
{
    EXPECT_EQ(runProtocol(false, 2), xudc::kResiduePerTrb);
}

TEST(XudcOutResidue, NonsenseFailsTheRequest)
{
    const uint32_t three[] = {512, 4096, 4096};
    xudc::OutResult r = xudc::outEvent(xudc::kResiduePerTd, three, 3, 1, 3, true);  // 3 <= rest (4096): not per TD
    EXPECT_TRUE(r.failed);
    EXPECT_EQ(r.learned, xudc::kResidueBroken);
    const uint32_t two[] = {512, 4096};
    r = xudc::outEvent(xudc::kResidueUnknown, two, 2, 0, 4096, false);  // the probe, per TD
    EXPECT_FALSE(r.done);
    EXPECT_EQ(r.learned, xudc::kResiduePerTd);
}

// OUT data through the A57's data cache, which the controller does not snoop. A model of it over the request's memory
// and whatever shares its cache lines: 64-byte lines cached clean or dirty or not at all; the CPU reads and writes
// through it (a read may also be a speculative fill), the controller reads and writes memory only, any line may be
// evicted (written back if dirty) at any moment. The driver's sequence (SwitchXUDC.cpp admit and releaseTrbs): bounce
// pages and the request cleaned and invalidated before the doorbell (performOperation kIOMemoryIncoherentIOFlush), the
// controller writes what the host sent, then the request's first `actual` bytes cleaned and invalidated again and the
// bounced bytes copied in by the CPU (writeBytes). Meanwhile the owners of the memory around the request's runs write
// theirs. After that the client must read exactly what the host sent and every neighbour its own bytes: no stale or
// partial OUT data, whatever the layout. A direct piece whose cache line holds a neighbour's bytes fails it: the
// neighbour's dirty line, written back by the second clean, carries the line's bytes from before the transfer over the
// data.
namespace {
struct CacheModel {
    struct Line {
        std::array<uint8_t, 64> bytes;
        bool dirty;
    };

    std::unordered_map<uint64_t, std::array<uint8_t, 64>> memory;  // by line address; untouched memory reads 0xC1
    std::map<uint64_t, Line> cached;

    std::array<uint8_t, 64> &mem(uint64_t line)
    {
        auto it = memory.find(line);
        if (it == memory.end()) {
            std::array<uint8_t, 64> fresh;
            fresh.fill(0xC1);
            it = memory.emplace(line, fresh).first;
        }
        return it->second;
    }

    Line &fill(uint64_t line)
    {
        auto it = cached.find(line);
        if (it == cached.end())
            it = cached.emplace(line, Line{mem(line), false}).first;
        return it->second;
    }

    void cpuWrite(uint64_t pa, uint8_t v)
    {
        Line &l = fill(pa & ~63ull);
        l.bytes[pa & 63] = v;
        l.dirty = true;
    }

    uint8_t cpuRead(uint64_t pa)
    {
        return fill(pa & ~63ull).bytes[pa & 63];
    }

    void dmaWrite(uint64_t pa, uint8_t v)
    {
        mem(pa & ~63ull)[pa & 63] = v;
    }

    void evict(uint64_t line)
    {
        const auto it = cached.find(line);
        if (it == cached.end())
            return;
        if (it->second.dirty)
            mem(line) = it->second.bytes;
        cached.erase(it);
    }

    void civac(uint64_t pa, uint64_t length)  // dc civac over [pa, pa + length)
    {
        for (uint64_t line = pa & ~63ull; line < pa + length; line += 64)
            evict(line);
    }

    void evictAny(Rng &rng)
    {
        if (cached.empty())
            return;
        evict(std::next(cached.begin(), static_cast<long>(rng.randrange(cached.size())))->first);
    }
};

// The physical ranges of request bytes [from, to).
template <class F> void forRange(const Runs &runs, uint32_t from, uint32_t to, F f)
{
    uint32_t start = 0;
    for (const xudc::Run &r : runs) {
        const uint32_t a = std::max(from, start), b = std::min(to, start + r.length);
        if (a < b)
            f(r.pa + (a - start), b - a);
        start += r.length;
    }
}

uint64_t paOf(const Runs &runs, uint32_t offset)
{
    return paAt(runs, offset)->first;
}

// One OUT request through the model; returns false (with ADD_FAILUREs) when the client or a neighbour reads wrong.
bool outThroughCache(Rng &rng, const Runs &runs, uint32_t total, uint32_t mps)
{
    std::map<uint64_t, uint8_t> owner;  // every byte of every line the runs touch: 0 the request's, 1 a neighbour's
    forRange(runs, 0, total, [&](uint64_t pa, uint32_t length) {
        for (uint64_t line = pa & ~63ull; line < pa + length; line += 64)
            for (uint64_t b = line; b < line + 64; ++b)
                owner.emplace(b, 1);
    });
    forRange(runs, 0, total, [&](uint64_t pa, uint32_t length) {
        for (uint64_t b = pa; b < pa + length; ++b)
            owner[b] = 0;
    });
    CacheModel cache;
    std::map<uint64_t, uint8_t> neighbour;  // what each neighbour byte should read
    std::vector<uint64_t> neighbours, lines;
    for (const auto &[b, who] : owner) {
        if (who)
            neighbours.push_back(b);
        if ((b & 63) == 0)
            lines.push_back(b);
    }
    auto neighbourWrites = [&](unsigned n) {
        for (unsigned i = 0; i < n && !neighbours.empty(); ++i) {
            const uint64_t b = neighbours[rng.randrange(neighbours.size())];
            const auto v = static_cast<uint8_t>(rng.randrange(256));
            cache.cpuWrite(b, v);
            neighbour[b] = v;
        }
    };
    auto background = [&]() {  // neighbours at work, speculative fills, evictions
        neighbourWrites(static_cast<unsigned>(rng.randrange(3)));
        if (!lines.empty() && rng.random() < 0.5)
            cache.fill(lines[rng.randrange(lines.size())]);
        if (rng.random() < 0.3)
            cache.evictAny(rng);
    };
    neighbourWrites(64);
    for (int i = 0; i < 8; ++i)
        background();

    Plan p = plan(runs, total, mps, true, false);
    if (p.pieces.empty())
        return true;  // the serial path
    for (size_t k = 0; k < p.pieces.size(); ++k)
        if (p.pieces[k].bounce)
            p.pieces[k].pa = 0x7000'0000 + k * kPage;
    // admit: bounce pages and the request cleaned and invalidated before the doorbell.
    for (const xudc::Piece &piece : p.pieces)
        if (piece.bounce)
            cache.civac(piece.pa, piece.length ? piece.length : 1);
    forRange(runs, 0, total, [&](uint64_t pa, uint32_t length) {
        cache.civac(pa, length);
    });
    // The host sends `actual` bytes (a short transfer, or all of it); the controller writes them, packet by packet.
    const uint32_t actual = rng.random() < 0.3 ? total : static_cast<uint32_t>(rng.randint(0, total));
    std::vector<uint8_t> host(actual);
    for (uint8_t &b : host)
        b = static_cast<uint8_t>(rng.randrange(256));
    for (const xudc::Piece &piece : p.pieces) {
        for (uint32_t i = 0; i < piece.length && piece.offset + i < actual; ++i) {
            cache.dmaWrite(piece.pa + i, host[piece.offset + i]);
            if (i % mps == 0)
                background();
        }
    }
    background();
    // releaseTrbs: the request's first `actual` bytes cleaned and invalidated, then the bounced bytes copied in.
    forRange(runs, 0, actual, [&](uint64_t pa, uint32_t length) {
        cache.civac(pa, length);
    });
    for (const xudc::Piece &piece : p.pieces) {
        if (!piece.bounce || piece.offset >= actual)
            continue;
        const uint32_t got = std::min(actual - piece.offset, piece.length);
        cache.civac(piece.pa, got);
        for (uint32_t i = 0; i < got; ++i)
            cache.cpuWrite(paOf(runs, piece.offset + i), cache.cpuRead(piece.pa + i));
    }
    for (int i = 0; i < 16; ++i)
        background();
    bool ok = true;
    for (uint32_t i = 0; i < actual && ok; ++i) {
        const uint8_t got = cache.cpuRead(paOf(runs, i));
        if (got != host[i]) {
            ADD_FAILURE() << "byte " << i << " of " << actual << " reads " << int(got) << ", the host sent "
                          << int(host[i]);
            ok = false;
        }
    }
    for (const auto &[b, v] : neighbour) {
        if (cache.cpuRead(b) != v) {
            ADD_FAILURE() << "a neighbour's byte at " << std::hex << b << " changed";
            ok = false;
            break;
        }
    }
    return ok;
}
}  // namespace

TEST(XudcOutCache, NoStaleOrPartialData)
{
    Rng rng(19);
    unsigned direct = 0;
    for (unsigned c = 0; c < 700; ++c) {
        const auto [runs, total] = randomLayout(rng, c % 4 ? Kind::Mbufs : Kind::Pages);
        bool overlap = false;  // two runs of a layout on the same bytes: not a buffer
        for (size_t i = 0; i < runs.size() && !overlap; ++i)
            for (size_t j = i + 1; j < runs.size(); ++j)
                if (runs[i].pa < runs[j].pa + runs[j].length && runs[j].pa < runs[i].pa + runs[i].length)
                    overlap = true;
        if (overlap || total > 32768)
            continue;
        SCOPED_TRACE("case " + std::to_string(c) + ": " + std::to_string(runs.size()) + " runs, " +
                     std::to_string(total) + " bytes");
        ASSERT_TRUE(outThroughCache(rng, runs, total, 512));
        ++direct;
    }
    EXPECT_GT(direct, 300u);
}

// A cost model, the same costs for both paths (microseconds; assumptions, for the next boot's stats line to replace):
// an interrupt with its event and the completion 6; prepare() + complete() of a kernel range 6 plus 0.3 a page;
// getPhysicalSegment 0.3 a page; a cache line cleaned or invalidated 0.006; a copy through bcopy_phys 0.5 a KiB
// (2 GB/s); planning 0.02 a packet and a TRB 0.1 (new path only); the bus at 42.5 MB/s (USB2 high speed bulk) or
// 400 MB/s (SuperSpeed). The serial path (before r17) moves one 64 KiB piece at a time: prepare, copy and flush each
// piece, and the bus waits for the CPU between pieces (20 us of interrupt and work-loop latency plus that work). The
// new path keeps requests queued, so the bus never waits, and copies only what it bounces.
namespace {
constexpr double kBusGap = 20.0;

double perRequest(double request, double bounced, bool fresh, bool out)
{
    const double pages = request / kPage;
    double cost = 6 + 6 + 0.3 * pages + 0.3 * pages + 0.006 * request / 64 * (out ? 2 : 1);
    if (fresh)
        cost += 0.02 * request / 512 + 0.1 * pages;
    cost += (fresh ? bounced : 1.0) * request / 1024 * 0.5;
    return cost;
}

// MB/s and the fraction of a core.
std::pair<double, double> rateAndCpu(double request, double bounced, bool fresh, double bus, bool out = false)
{
    const double cost = perRequest(request, bounced, fresh, out);
    const double wire = request / bus;  // us on the bus (MB/s = bytes/us)
    const double perRequestUs = fresh ? wire : wire + kBusGap + cost;
    return {request / perRequestUs, cost / perRequestUs};
}
}  // namespace

TEST(XudcThroughput, Usb2HighSpeedUnderATenthOfACore)
{
    std::string lines;
    for (const double request : {16384.0, 65536.0}) {
        for (const bool out : {false, true}) {
            const auto [rate, cpu] = rateAndCpu(request, 0.5, true, 42.5, out);
            const double lowCpu = rateAndCpu(request, 0.0, true, 42.5, out).second;
            const auto [old, oldCpu] = rateAndCpu(request, 1.0, false, 42.5, out);
            char line[200];
            std::snprintf(line, sizeof(line),
                          "%s %d KiB: %.1f MB/s at %.1f%% of a core (half bounced), %.1f%% (none), serial path %.1f "
                          "MB/s at %.1f%%; ",
                          out ? "OUT" : "IN", static_cast<int>(request) / 1024, rate, cpu * 100, lowCpu * 100, old,
                          oldCpu * 100);
            lines += line;
            EXPECT_LT(cpu, 0.10) << line;
            EXPECT_GT(rate, old) << line;
            EXPECT_LT(lowCpu, oldCpu) << line;
        }
    }
    const double ss = rateAndCpu(65536, 0.0, true, 400).second, ssHalf = rateAndCpu(65536, 0.5, true, 400).second;
    std::printf(
        "XUDC cost model: %sSuperSpeed IN 64 KiB at 400 MB/s: %.0f%% of a core with nothing bounced, %.0f%% with "
        "half the pages above 4 GiB\n",
        lines.c_str(), ss * 100, ssHalf * 100);
}
