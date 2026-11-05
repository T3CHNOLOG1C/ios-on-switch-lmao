// IOSwitchCryptexCache.h, the cryptex direct read map's block cache, against a model: every request the
// cache takes completes exactly once, with the image's bytes unless a read it waited for failed; a block is never
// read twice at once nor read while held; a window is never renamed or retired while a request holds it or a read fills
// it; and the cache's own invariants hold after every step. What IOSwitchCryptex.cpp does with it is guarded by
// cryptex_source_test.cpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

#include "../IOSwitchCryptexCache.h"

namespace {

uint64_t content(uint8_t image, uint64_t block)
{
    uint64_t z = (uint64_t(image) << 48) + block + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}
constexpr uint64_t kPoison = 0xdeadbeefdeadbeefull;

// The cache with its windows, each window's "bytes" one uint64_t per block (the block's content id).
struct Rig {
    std::vector<IOSwitchWindow> windows;
    std::vector<std::vector<uint64_t>> slots;
    std::vector<IOSwitchWindow *> buckets;
    IOSwitchCache cache;
    uint64_t imageBlocks[2] = {};

    Rig(uint32_t count, uint32_t bucketCount, uint64_t image0, uint64_t image1)
        : windows(count), slots(count, std::vector<uint64_t>(kIOSwitchWindowBlocks, kPoison)), buckets(bucketCount)
    {
        for (uint32_t i = 0; i < count; ++i) windows[i].bytes = slots[i].data();
        cache.init(windows.data(), count, buckets.data(), bucketCount);
        imageBlocks[0] = image0;
        imageBlocks[1] = image1;
    }
    uint64_t *bytes(IOSwitchWindow *w) { return static_cast<uint64_t *>(w->bytes); }
    // The card's read: the image's blocks into the window, or poison for a failed one.
    IOSwitchCacheRequest *finish(const IOSwitchCacheRead &r, bool ok)
    {
        for (uint32_t b = r.first; b < r.first + r.count; ++b)
            bytes(r.window)[b] = ok ? content(r.window->image, r.window->tag + b) : kPoison;
        return cache.finished(r.window, r.first, r.count, ok);
    }
    // Copies a ready request's blocks out of its windows; false if any differs from the image.
    bool copyOut(const IOSwitchCacheRequest &r, uint8_t image, uint64_t first, uint32_t blocks)
    {
        const uint64_t firstTag = first - first % kIOSwitchWindowBlocks;
        for (uint64_t b = first; b < first + blocks; ++b) {
            const uint32_t i = uint32_t((b - firstTag) / kIOSwitchWindowBlocks);
            if (i >= r.windows) return false;
            IOSwitchWindow *w = r.window[i];
            const uint32_t at = uint32_t(b - w->tag);
            if (w->image != image || w->tag != firstTag + uint64_t(i) * kIOSwitchWindowBlocks || at >= 32) return false;
            if (!(w->valid & (1u << at)) || bytes(w)[at] != content(image, b)) return false;
        }
        return true;
    }
};

std::vector<IOSwitchCacheRequest *> list(IOSwitchCacheRequest *ready)
{
    std::vector<IOSwitchCacheRequest *> out;
    for (; ready; ready = ready->nextReady) out.push_back(ready);
    return out;
}

TEST(CryptexCache, AColdMissReadsItsWholeWindowOnceAndNeighboursWaitForIt)
{
    Rig rig(4, 8, 100, 10);
    IOSwitchCacheRead reads[32];
    uint32_t n = 0;
    IOSwitchCacheRequest a{}, b{}, c{};
    ASSERT_TRUE(rig.cache.plan(&a, 0, 5, 1, 100, IOSwitchCache::kReadAround, reads, 32, &n));
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(reads[0].first, 0u);
    EXPECT_EQ(reads[0].count, 32u);
    EXPECT_EQ(a.waiting, 1u);
    IOSwitchCacheRead none[32];
    uint32_t m = 0;
    ASSERT_TRUE(rig.cache.plan(&b, 0, 6, 2, 100, IOSwitchCache::kReadAround, none, 32, &m));
    EXPECT_EQ(m, 0u) << "a block being read is read again";
    EXPECT_EQ(b.waiting, 1u);
    EXPECT_EQ(rig.cache.check(), nullptr);
    std::vector<IOSwitchCacheRequest *> ready = list(rig.finish(reads[0], true));
    EXPECT_EQ(ready.size(), 2u);
    EXPECT_TRUE(rig.copyOut(a, 0, 5, 1));
    EXPECT_TRUE(rig.copyOut(b, 0, 6, 2));
    rig.cache.release(&a, true);
    rig.cache.release(&b, true);
    ASSERT_TRUE(rig.cache.plan(&c, 0, 31, 1, 100, IOSwitchCache::kReadAround, none, 32, &m));
    EXPECT_EQ(m, 0u);
    EXPECT_EQ(c.waiting, 0u) << "a held block is not ready";
    EXPECT_TRUE(rig.copyOut(c, 0, 31, 1));
    rig.cache.release(&c, true);
    EXPECT_EQ(rig.cache.stats().ready, 1u);
    EXPECT_EQ(rig.cache.stats().reads, 1u);
    EXPECT_EQ(rig.cache.stats().served, 4u);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

TEST(CryptexCache, TheLastWindowStopsAtTheImageEnd)
{
    Rig rig(2, 4, 40, 10);
    IOSwitchCacheRead reads[32];
    uint32_t n = 0;
    IOSwitchCacheRequest a{};
    ASSERT_TRUE(rig.cache.plan(&a, 0, 35, 1, 40, IOSwitchCache::kReadAround, reads, 32, &n));
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(reads[0].first, 0u);
    EXPECT_EQ(reads[0].count, 8u);
    IOSwitchCacheRequest past{};
    uint32_t m = 0;
    EXPECT_FALSE(rig.cache.plan(&past, 0, 39, 2, 40, IOSwitchCache::kReadAround, reads + 1, 31, &m));
    EXPECT_FALSE(rig.cache.plan(&past, 1, 10, 1, 10, IOSwitchCache::kReadAround, reads + 1, 31, &m));
}

TEST(CryptexCache, ReadAheadReadsExactlyItsBlocksAndRequestsFindThem)
{
    Rig rig(4, 8, 200, 10);
    IOSwitchCacheRead reads[kIOSwitchWindowReads];
    uint32_t n = 0;
    std::vector<IOSwitchCacheRead> issued;
    for (uint64_t at = 5, end = 40; at < end;) {
        const uint32_t covered = rig.cache.readAhead(0, at, end - at, 200, reads, &n);
        ASSERT_GT(covered, 0u);
        issued.insert(issued.end(), reads, reads + n);
        at += covered;
    }
    ASSERT_EQ(issued.size(), 2u);
    EXPECT_EQ(issued[0].first, 5u);
    EXPECT_EQ(issued[0].count, 27u);
    EXPECT_EQ(issued[1].first, 0u);
    EXPECT_EQ(issued[1].count, 8u);
    EXPECT_NE(issued[0].window, issued[1].window);
    // A request inside the read-ahead waits for it and reads only the rest of its window.
    IOSwitchCacheRequest a{};
    IOSwitchCacheRead around[32];
    ASSERT_TRUE(rig.cache.plan(&a, 0, 10, 4, 200, IOSwitchCache::kReadAround, around, 32, &n));
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(around[0].first, 0u);
    EXPECT_EQ(around[0].count, 5u);
    EXPECT_EQ(a.waiting, 1u);
    EXPECT_TRUE(list(rig.finish(around[0], true)).empty());
    EXPECT_EQ(list(rig.finish(issued[0], true)).size(), 1u);
    EXPECT_TRUE(rig.copyOut(a, 0, 10, 4));
    rig.cache.release(&a, true);
    EXPECT_TRUE(list(rig.finish(issued[1], true)).empty());
    // Read ahead again: nothing left to read.
    EXPECT_EQ(rig.cache.readAhead(0, 5, 35, 200, reads, &n), 27u);
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(rig.cache.stats().aheadBlocks, 35u);
    EXPECT_EQ(rig.cache.stats().aheadServed, 4u);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

TEST(CryptexCache, ALargeRequestIsTakenOnlyWhenItsBlocksAreAllHere)
{
    Rig rig(4, 8, 300, 10);
    IOSwitchCacheRead reads[32];
    uint32_t n = 0;
    IOSwitchCacheRequest big{};
    EXPECT_FALSE(rig.cache.plan(&big, 0, 0, 64, 300, IOSwitchCache::kCoveredOnly, reads, 32, &n));
    EXPECT_EQ(rig.cache.stats().refused, 1u);
    for (uint64_t at = 0; at < 64;) at += rig.cache.readAhead(0, at, 64 - at, 300, reads + (at ? 1 : 0), &n);
    ASSERT_TRUE(rig.cache.plan(&big, 0, 0, 64, 300, IOSwitchCache::kCoveredOnly, reads + 2, 30, &n));
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(big.waiting, 2u);
    EXPECT_TRUE(list(rig.finish(reads[0], true)).empty());
    EXPECT_EQ(list(rig.finish(reads[1], true)).size(), 1u);
    EXPECT_TRUE(rig.copyOut(big, 0, 0, 64));
    rig.cache.release(&big, true);
    // Not all here: refused, nothing held.
    IOSwitchCacheRequest wider{};
    EXPECT_FALSE(rig.cache.plan(&wider, 0, 0, 65, 300, IOSwitchCache::kCoveredOnly, reads, 32, &n));
    EXPECT_EQ(rig.cache.check(), nullptr);
}

TEST(CryptexCache, AFailedReadFailsItsWaitersAndIsReadAgain)
{
    Rig rig(2, 4, 64, 10);
    IOSwitchCacheRead reads[32];
    uint32_t n = 0;
    IOSwitchCacheRequest a{}, b{};
    ASSERT_TRUE(rig.cache.plan(&a, 0, 3, 1, 64, IOSwitchCache::kReadAround, reads, 32, &n));
    ASSERT_EQ(n, 1u);
    const std::vector<IOSwitchCacheRequest *> ready = list(rig.finish(reads[0], false));
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_TRUE(a.failed);
    rig.cache.release(&a, false);
    ASSERT_TRUE(rig.cache.plan(&b, 0, 3, 1, 64, IOSwitchCache::kReadAround, reads, 32, &n));
    EXPECT_EQ(n, 1u);
    EXPECT_FALSE(b.failed);
    EXPECT_EQ(list(rig.finish(reads[0], true)).size(), 1u);
    EXPECT_TRUE(rig.copyOut(b, 0, 3, 1));
    rig.cache.release(&b, true);
    EXPECT_EQ(rig.cache.stats().failed, 1u);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

TEST(CryptexCache, WindowsInUseAreNeverReusedOrRetired)
{
    Rig rig(1, 2, 200, 10);
    IOSwitchCacheRead reads[32];
    uint32_t n = 0, m = 0;
    IOSwitchCacheRequest a{}, b{};
    ASSERT_TRUE(rig.cache.plan(&a, 0, 3, 1, 200, IOSwitchCache::kReadAround, reads, 32, &n));
    EXPECT_FALSE(rig.cache.plan(&b, 0, 40, 1, 200, IOSwitchCache::kReadAround, reads + 1, 31, &m));
    EXPECT_EQ(rig.cache.retire(0), nullptr);
    EXPECT_EQ(rig.cache.readAhead(0, 64, 4, 200, reads + 1, &m), 0u);
    ASSERT_EQ(list(rig.finish(reads[0], true)).size(), 1u);
    EXPECT_FALSE(rig.cache.plan(&b, 0, 40, 1, 200, IOSwitchCache::kReadAround, reads + 1, 31, &m));
    EXPECT_EQ(rig.cache.retire(0), nullptr);
    rig.cache.release(&a, true);
    ASSERT_TRUE(rig.cache.plan(&b, 0, 40, 1, 200, IOSwitchCache::kReadAround, reads, 32, &n));
    EXPECT_EQ(rig.cache.stats().evicted, 1u) << "31 blocks of the first window were never served";
    EXPECT_EQ(list(rig.finish(reads[0], true)).size(), 1u);
    rig.cache.release(&b, true);
    IOSwitchWindow *w = rig.cache.retire(0);
    ASSERT_NE(w, nullptr);
    EXPECT_TRUE(w->retired);
    EXPECT_EQ(rig.cache.live(), 0u);
    EXPECT_FALSE(rig.cache.plan(&a, 0, 3, 1, 200, IOSwitchCache::kReadAround, reads, 32, &n));
    EXPECT_EQ(rig.cache.check(), nullptr);
}

// A window whose blocks have all been served (they are in the files' page cache) is reused before one still holding
// blocks nobody asked for yet.
TEST(CryptexCache, FullyServedWindowsAreReusedFirst)
{
    Rig rig(2, 4, 1000, 10);
    IOSwitchCacheRead reads[32];
    uint32_t n = 0;
    IOSwitchCacheRequest a{}, b{}, c{};
    // Window 0: read ahead (32 blocks) and served whole; window 1: read around one block.
    ASSERT_EQ(rig.cache.readAhead(0, 0, 32, 1000, reads, &n), 32u);
    ASSERT_EQ(n, 1u);
    rig.finish(reads[0], true);
    ASSERT_TRUE(rig.cache.plan(&b, 0, 32, 1, 1000, IOSwitchCache::kReadAround, reads, 32, &n));
    rig.finish(reads[0], true);
    rig.cache.release(&b, true);
    ASSERT_TRUE(rig.cache.plan(&a, 0, 0, 16, 1000, IOSwitchCache::kReadAround, reads, 32, &n));
    EXPECT_EQ(n, 0u);
    rig.cache.release(&a, true);
    ASSERT_TRUE(rig.cache.plan(&a, 0, 16, 16, 1000, IOSwitchCache::kReadAround, reads, 32, &n));
    rig.cache.release(&a, true);
    // A new window takes window 0 (all served), though window 1 was used longer ago.
    ASSERT_TRUE(rig.cache.plan(&c, 0, 500, 1, 1000, IOSwitchCache::kReadAround, reads, 32, &n));
    rig.finish(reads[0], true);
    rig.cache.release(&c, true);
    EXPECT_EQ(rig.cache.stats().evicted, 0u);
    ASSERT_TRUE(rig.cache.plan(&b, 0, 33, 1, 1000, IOSwitchCache::kReadAround, reads, 32, &n));
    EXPECT_EQ(n, 0u) << "window 1 was reused instead";
    rig.cache.release(&b, true);
    EXPECT_EQ(rig.cache.check(), nullptr);
}

// Random requests, read-aheads, read completions (in any order, some failing), copies and retirements against the
// model.
TEST(CryptexCache, RandomTrafficAgainstAModel)
{
    std::mt19937_64 rng(19);
    unsigned long requests = 0, refused = 0, failedRequests = 0, readsDone = 0;
    for (int round = 0; round < 400; ++round) {
        const uint32_t windowCount = 1 + uint32_t(rng() % 24);
        const uint32_t bucketCount = 1u << (rng() % 6);
        const uint64_t size0 = 1 + rng() % 2000, size1 = 1 + rng() % 300;
        Rig rig(windowCount, bucketCount, size0, size1);
        const bool failures = rng() % 3 == 0;

        struct Request {
            IOSwitchCacheRequest cr{};
            uint8_t image = 0;
            uint64_t first = 0;
            uint32_t blocks = 0;
            bool ready = false, done = false, mayFail = false;
        };
        std::deque<std::unique_ptr<Request>> live;
        std::vector<Request *> ready;
        struct Read {
            IOSwitchCacheRead r;
            uint8_t image;
            uint64_t tag;
        };
        std::vector<Read> inflight;
        std::map<std::pair<uint8_t, uint64_t>, int> reading;  // (image, block) -> reads in flight

        auto issue = [&](const IOSwitchCacheRead *reads, uint32_t n) {
            for (uint32_t i = 0; i < n; ++i) {
                const IOSwitchCacheRead &r = reads[i];
                ASSERT_TRUE(r.count && r.first + r.count <= kIOSwitchWindowBlocks);
                for (uint32_t b = r.first; b < r.first + r.count; ++b) {
                    ASSERT_LT(r.window->tag + b, rig.imageBlocks[r.window->image]) << "read past the image";
                    const int before = reading[{r.window->image, r.window->tag + b}]++;
                    ASSERT_EQ(before, 0) << "a block read twice at once";
                }
                inflight.push_back({r, r.window->image, r.window->tag});
            }
        };
        auto becameReady = [&](IOSwitchCacheRequest *list) {
            for (; list; list = list->nextReady) {
                Request *q = static_cast<Request *>(list->owner);
                ASSERT_FALSE(q->ready) << "a request made ready twice";
                q->ready = true;
                ready.push_back(q);
            }
        };

        for (int step = 0; step < 600; ++step) {
            const unsigned op = unsigned(rng() % 100);
            if (op < 35) {  // a request
                auto q = std::make_unique<Request>();
                q->cr.owner = q.get();
                q->image = uint8_t(rng() % 4 == 0);
                const uint64_t size = rig.imageBlocks[q->image];
                const bool large = rng() % 6 == 0;
                q->first = rng() % size;
                const uint64_t room = size - q->first;
                q->blocks = uint32_t(std::min<uint64_t>(room, large ? 17 + rng() % 240 : 1 + rng() % 16));
                IOSwitchCacheRead reads[kIOSwitchAroundWindows * kIOSwitchWindowReads];
                uint32_t n = 0;
                const auto mode = q->blocks > 16 ? IOSwitchCache::kCoveredOnly : IOSwitchCache::kReadAround;
                if (!rig.cache.plan(&q->cr, q->image, q->first, q->blocks, size, mode, reads,
                                    kIOSwitchAroundWindows * kIOSwitchWindowReads, &n)) {
                    ASSERT_EQ(n, 0u);
                    ASSERT_EQ(q->cr.windows, 0u) << "a refused request holds windows";
                    ++refused;
                    continue;
                }
                if (mode == IOSwitchCache::kCoveredOnly) ASSERT_EQ(n, 0u) << "a large request read something";
                issue(reads, n);
                ++requests;
                if (!q->cr.waiting) {
                    q->ready = true;
                    ready.push_back(q.get());
                }
                live.push_back(std::move(q));
            } else if (op < 45) {  // read ahead over a range
                const uint8_t image = uint8_t(rng() % 4 == 0);
                const uint64_t size = rig.imageBlocks[image];
                const uint64_t first = rng() % size, end = std::min<uint64_t>(size, first + 1 + rng() % 128);
                for (uint64_t at = first; at < end;) {
                    IOSwitchCacheRead reads[kIOSwitchWindowReads];
                    uint32_t n = 0;
                    const uint32_t covered = rig.cache.readAhead(image, at, end - at, size, reads, &n);
                    if (!covered) break;
                    issue(reads, n);
                    at += covered;
                }
            } else if (op < 80 && !inflight.empty()) {  // a read finishes
                const size_t i = rng() % inflight.size();
                const Read r = inflight[i];
                inflight.erase(inflight.begin() + long(i));
                ASSERT_EQ(r.r.window->tag, r.tag) << "a window renamed while a read fills it";
                ASSERT_FALSE(r.r.window->retired);
                for (uint32_t b = r.r.first; b < r.r.first + r.r.count; ++b) --reading[{r.image, r.tag + b}];
                const bool ok = !failures || rng() % 8;
                if (!ok)
                    for (auto &q : live)
                        if (!q->done && q->image == r.image && q->first < r.tag + r.r.first + r.r.count &&
                            r.tag + r.r.first < q->first + q->blocks)
                            q->mayFail = true;
                becameReady(rig.finish(r.r, ok));
                ++readsDone;
            } else if (op < 97 && !ready.empty()) {  // a ready request copies out and lets go
                const size_t i = rng() % ready.size();
                Request *q = ready[i];
                ready.erase(ready.begin() + long(i));
                ASSERT_FALSE(q->done) << "a request completed twice";
                ASSERT_EQ(q->cr.waiting, 0u);
                if (q->cr.failed) {
                    ASSERT_TRUE(q->mayFail) << "a request failed with no failed read of its blocks";
                    ++failedRequests;
                } else {
                    ASSERT_TRUE(rig.copyOut(q->cr, q->image, q->first, q->blocks)) << "wrong bytes copied out";
                }
                rig.cache.release(&q->cr, !q->cr.failed);
                q->done = true;
            } else if (op >= 97) {  // memory given back
                if (IOSwitchWindow *w = rig.cache.retire(uint32_t(rng() % (windowCount + 1)))) {
                    ASSERT_TRUE(w->retired);
                    ASSERT_EQ(w->users, 0u);
                    ASSERT_EQ(w->reading, 0u);
                    std::fill(rig.slots[size_t(w - rig.windows.data())].begin(),
                              rig.slots[size_t(w - rig.windows.data())].end(), kPoison);
                }
            }
            const char *why = rig.cache.check();
            ASSERT_EQ(why, nullptr) << why << " (round " << round << ", step " << step << ")";
        }
        // Drain: every read finishes, every request completes.
        while (!inflight.empty() || !ready.empty()) {
            if (!inflight.empty()) {
                const Read r = inflight.back();
                inflight.pop_back();
                for (uint32_t b = r.r.first; b < r.r.first + r.r.count; ++b) --reading[{r.image, r.tag + b}];
                becameReady(rig.finish(r.r, true));
            }
            while (!ready.empty()) {
                Request *q = ready.back();
                ready.pop_back();
                ASSERT_TRUE(q->cr.failed ? q->mayFail : rig.copyOut(q->cr, q->image, q->first, q->blocks));
                failedRequests += q->cr.failed;
                rig.cache.release(&q->cr, !q->cr.failed);
                q->done = true;
            }
        }
        for (auto &q : live) ASSERT_TRUE(q->done) << "a request never completed";
        ASSERT_EQ(rig.cache.check(), nullptr);
        const IOSwitchCacheStats &s = rig.cache.stats();
        ASSERT_LE(s.aheadServed, s.aheadBlocks);
        ASSERT_LE(s.aheadServed, s.served);
    }
    printf("cryptex cache: %lu requests (%lu refused), %lu reads, %lu failed requests\n", requests, refused, readsDone,
           failedRequests);
    EXPECT_GT(failedRequests, 0u);
    EXPECT_GT(refused, 0u);
}

}  // namespace
