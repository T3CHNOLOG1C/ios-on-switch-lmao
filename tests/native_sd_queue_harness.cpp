// Host test of drivers/NativeIOSwitchBlock/IOSwitchQueue.h: which queued reads join the SD worker's read chain
// (test_native_sd_queue.py compiles and runs this).
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "IOSwitchQueue.h"

struct Request {
    Request *next;
    uint64_t lba;
    uint32_t sectors;
    bool write;
    bool chained;
    int id;
};

// A queue built from (lba, sectors, write) in queue order; ids are queue positions (0 = oldest).
struct Queue {
    std::vector<Request> storage;
    Request *head = nullptr, *tail = nullptr;
    explicit Queue(const std::vector<std::tuple<uint64_t, uint32_t, bool>> &items) : storage(items.size())
    {
        for (size_t i = 0; i < items.size(); ++i) {
            storage[i] = {nullptr, std::get<0>(items[i]), std::get<1>(items[i]), std::get<2>(items[i]), false, int(i)};
            if (i) storage[i - 1].next = &storage[i];
        }
        head = storage.empty() ? nullptr : &storage[0];
        tail = storage.empty() ? nullptr : &storage.back();
    }
    Request *at(int id) { return &storage[size_t(id)]; }
    std::vector<int> ids() const
    {
        std::vector<int> out;
        for (const Request *r = head; r; r = r->next) out.push_back(r->id);
        return out;
    }
    void checkTail() const
    {
        const Request *last = nullptr;
        for (const Request *r = head; r; r = r->next) last = r;
        assert(tail == last);
    }
};

struct Picked {
    std::vector<int> chain;
    uint64_t start;
    uint32_t sectors;
};

static Picked pick(Queue &q, int first, uint32_t limit = 2048, uint32_t maxCount = 256, unsigned scan = 128)
{
    Picked p{};
    const uint32_t count = ioswitchGatherReads(q.head, q.at(first), limit, maxCount, scan, &p.start, &p.sectors);
    assert(ioswitchChainTiles(q.head, p.start, p.sectors, count));  // what the driver checks before unlinking
    Request *chain = ioswitchUnlinkChain(q.head, q.tail, count);
    uint64_t at = p.start;
    for (Request *r = chain; r; r = r->next) {
        assert(r->lba == at && !r->write && !r->chained);
        at += r->sectors;
        p.chain.push_back(r->id);
    }
    assert(p.chain.size() == count && at == p.start + p.sectors && p.sectors <= limit);
    for (const Request *r = q.head; r; r = r->next) assert(!r->chained);
    q.checkTail();
    return p;
}

static void basics()
{
    // A lone read.
    {
        Queue q({{100, 8, false}});
        Picked p = pick(q, 0);
        assert(p.chain == std::vector<int>{0} && p.start == 100 && p.sectors == 8 && !q.head && !q.tail);
    }
    // Queued in rising order right behind each other (the old rule's case): one chain, one pass.
    {
        Queue q({{100, 8, false}, {108, 8, false}, {116, 16, false}, {500, 8, false}});
        Picked p = pick(q, 0);
        assert((p.chain == std::vector<int>{0, 1, 2}) && p.sectors == 32);
        assert(q.ids() == std::vector<int>{3});
    }
    // Continuations anywhere in the queue, before and after, in any order: one chain in card order.
    {
        Queue q({{108, 8, false}, {900, 8, false}, {116, 8, false}, {100, 8, false}, {2000, 8, true}, {92, 8, false}});
        Picked p = pick(q, 0);
        assert((p.chain == std::vector<int>{5, 3, 0, 2}) && p.start == 92 && p.sectors == 32);
        assert((q.ids() == std::vector<int>{1, 4}));
    }
    // The picked read may sit in the middle (a passing read): older requests before it stay queued, in order.
    {
        Queue q({{5000, 8, false}, {100, 8, false}, {6000, 8, true}, {108, 8, false}});
        Picked p = pick(q, 1);
        assert((p.chain == std::vector<int>{1, 3}));
        assert((q.ids() == std::vector<int>{0, 2}));
    }
}

static void ordering()
{
    // A read that continues the chain but overlaps an older queued write stays behind that write.
    {
        Queue q({{100, 8, false}, {108, 8, true}, {108, 8, false}});
        Picked p = pick(q, 0);
        assert(p.chain == std::vector<int>{0});
        assert((q.ids() == std::vector<int>{1, 2}));
    }
    // ...also when the write only partly overlaps it.
    {
        Queue q({{100, 8, false}, {110, 2, true}, {108, 8, false}});
        assert(pick(q, 0).chain == std::vector<int>{0});
    }
    // A newer read of blocks an older read also wants is not pulled ahead of it.
    {
        Queue q({{100, 8, false}, {104, 8, false}, {108, 8, false}});
        Picked p = pick(q, 0);
        assert(p.chain == std::vector<int>{0});  // 104+8 overlaps nothing older but does not continue; 108 overlaps 104
    }
    // An older read that continues the chain joins even past a newer write (it was ahead of that write anyway).
    {
        Queue q({{108, 8, false}, {200, 8, true}, {100, 8, false}});
        Picked p = pick(q, 2);  // a passing read picked from the back
        assert((p.chain == std::vector<int>{2, 0}) && p.start == 100);
        assert(q.ids() == std::vector<int>{1});
    }
    // Writes never join a read chain, even when they continue it.
    {
        Queue q({{100, 8, false}, {108, 8, true}, {116, 8, false}});
        Picked p = pick(q, 0);
        assert(p.chain == std::vector<int>{0});
    }
    // A write earlier in the queue than a contiguous read but not overlapping it does not stop it.
    {
        Queue q({{100, 8, false}, {5000, 8, true}, {108, 8, false}});
        assert((pick(q, 0).chain == std::vector<int>{0, 2}));
    }
}

static void limits()
{
    // The command's length: 1 MiB of sectors, never more; what does not fit stays queued.
    {
        std::vector<std::tuple<uint64_t, uint32_t, bool>> items;
        for (int i = 0; i < 300; ++i) items.push_back({uint64_t(i) * 8, 8, false});
        Queue q(items);
        Picked p = pick(q, 0, 2048, 256, 1000);
        assert(p.chain.size() == 256 && p.sectors == 2048);
        assert(q.ids().size() == 44 && q.ids().front() == 256);
    }
    // The request count.
    {
        Queue q({{0, 8, false}, {8, 8, false}, {16, 8, false}, {24, 8, false}});
        assert(pick(q, 0, 2048, 3).chain.size() == 3);
    }
    // A continuation that would pass the length limit is skipped, a shorter one still joins.
    {
        Queue q({{0, 8, false}, {8, 64, false}, {72, 8, false}, {1000, 8, false}});
        Picked p = pick(q, 0, 64);
        assert(p.chain == std::vector<int>{0});
        Queue r({{0, 8, false}, {8, 56, false}, {64, 8, false}});
        assert((pick(r, 0, 64).chain == std::vector<int>{0, 1}));
    }
    // Only the first `scan` queued requests are looked at.
    {
        Queue q({{0, 8, false}, {500, 8, false}, {600, 8, false}, {8, 8, false}});
        assert(pick(q, 0, 2048, 256, 3).chain == std::vector<int>{0});
        Queue r({{0, 8, false}, {500, 8, false}, {600, 8, false}, {8, 8, false}});
        assert((pick(r, 0, 2048, 256, 4).chain == std::vector<int>{0, 3}));
    }
    // Falling order still gathers everything (one request per pass).
    {
        std::vector<std::tuple<uint64_t, uint32_t, bool>> items;
        for (int i = 0; i < 40; ++i) items.push_back({uint64_t(39 - i) * 8, 8, false});
        Queue q(items);
        Picked p = pick(q, 0);
        assert(p.chain.size() == 40 && p.start == 0 && p.chain.front() == 39 && !q.head);
    }
}

// Random queues: every invariant the driver relies on, and that nothing that could still join was left out.
static void randomQueues()
{
    std::mt19937_64 random(0x5dc0de);
    for (unsigned round = 0; round < 20000; ++round) {
        const unsigned n = 1 + unsigned(random() % 40);
        std::vector<std::tuple<uint64_t, uint32_t, bool>> items;
        for (unsigned i = 0; i < n; ++i) {
            const uint64_t lba = (random() % 64) * 8;
            const uint32_t sectors = 8 * uint32_t(1 + random() % (random() % 4 ? 2 : 8));
            items.push_back({lba, sectors, random() % 5 == 0});
        }
        Queue q(items);
        std::vector<int> reads;
        for (unsigned i = 0; i < n; ++i)
            if (!std::get<2>(items[i])) reads.push_back(int(i));
        if (reads.empty()) continue;
        const int first = reads[random() % reads.size()];
        const uint32_t limit = random() % 2 ? 2048 : 8 * uint32_t(1 + random() % 16);
        if (std::get<1>(items[size_t(first)]) > limit) continue;
        const std::vector<int> before = q.ids();
        Picked p = pick(q, first, limit, 256, 128);
        std::vector<bool> taken(n);
        for (int id : p.chain) taken[size_t(id)] = true;
        assert(taken[size_t(first)]);
        // Every member other than `first` overlaps no older request that is still queued.
        for (int id : p.chain) {
            if (id == first) continue;
            const Request &r = q.storage[size_t(id)];
            for (int older = 0; older < id; ++older)
                if (!taken[size_t(older)]) assert(!ioswitchOverlaps(&q.storage[size_t(older)], &r));
        }
        // The rest of the queue keeps its order.
        std::vector<int> rest;
        for (int id : before)
            if (!taken[size_t(id)]) rest.push_back(id);
        assert(q.ids() == rest);
        // Maximal: no queued read continues the chain within the limit without overlapping an older queued request.
        for (int id : rest) {
            const Request &r = q.storage[size_t(id)];
            if (r.write) continue;
            if (r.lba != p.start + p.sectors && r.lba + r.sectors != p.start) continue;
            if (p.sectors + r.sectors > limit) continue;
            bool overlaps = false;
            for (int older = 0; older < id && !overlaps; ++older)
                overlaps = !taken[size_t(older)] && ioswitchOverlaps(&q.storage[size_t(older)], &r);
            assert(overlaps);
        }
    }
}

// The driver's check before it unlinks: a marked set that does not tile its range exactly is refused, and clearing the
// marks leaves the queue as it was.
static void tiling()
{
    Queue q({{100, 8, false}, {108, 8, false}, {104, 8, false}, {116, 8, true}, {124, 8, false}});
    auto mark = [&](std::vector<int> ids) {
        ioswitchClearChain(q.head);
        for (int id : ids) q.at(id)->chained = true;
    };
    mark({0, 1});
    assert(ioswitchChainTiles(q.head, 100, 16, 2));
    assert(!ioswitchChainTiles(q.head, 100, 24, 2));  // a hole (lengths short of the range)
    assert(!ioswitchChainTiles(q.head, 100, 16, 3));  // a member missing
    assert(!ioswitchChainTiles(q.head, 104, 12, 2));  // a member outside the range
    mark({0, 1, 2});
    assert(!ioswitchChainTiles(q.head, 100, 24, 3));  // overlapping members, even with the right total
    mark({0, 1, 3});
    assert(!ioswitchChainTiles(q.head, 100, 24, 3));  // a write
    mark({});
    assert(ioswitchChainTiles(q.head, 100, 0, 0));
    mark({4, 0});
    ioswitchClearChain(q.head);
    for (const Request *r = q.head; r; r = r->next) assert(!r->chained);
    assert((q.ids() == std::vector<int>{0, 1, 2, 3, 4}));
}

int main()
{
    tiling();
    basics();
    ordering();
    limits();
    randomQueues();
    puts("PASS: sd queue merging");
    return 0;
}
