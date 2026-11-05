// Direct scanout's replace/retire policy (NativeLegacyDisplay, r14-direct) on the host: Core Animation with its three
// framebuffers, recycling a surface when a later swap completes (QuartzCore "swap %u completed, recycling surface 0x%x from
// swap %u"); the display's policy (the real TegraScanoutRing; a fence-only GPU present per swap, one in flight; the frame
// replaces a flip still out; every swap retires at the latch that shows it or a newer one, in queue order, never the one in
// flight); DC A's window A latch, with boundaries anywhere between a flip's stores and folded frame-end actions. Checked:
//  - CA never draws into a surface DC A scans or may take at the next boundary, or the ring still holds;
//  - frames keep coming (no deadlock with three surfaces) at the display rate when CA keeps up, each on screen at the first
//    boundary after its rendering finished (latest frame wins);
//  - the copy path's early retire (at the offer) applied to direct frames does break the first rule (negative control).
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

#include "../drivers/TegraDisplayFlip/TegraScanoutRing.h"

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

uint64_t rng = 0x243f6a8885a308d3ull;
unsigned rnd(unsigned n)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (unsigned)(rng % n);
}

constexpr int kNone = TegraScanoutRing::kNone, kSlot0 = (int)TegraScanoutRing::kMaxBuffers;
constexpr unsigned kSurfaces = 3, kTicks = 16;  // a frame is 16 ticks

struct Swap {
    uint64_t id;
    unsigned surface;
    uint64_t seq;  // take sequence, 0: not taken
    uint64_t renderedAt, readyTick;
};

struct Sim {
    bool earlyRetire;  // the wrong policy (negative control)
    TegraScanoutRing ring;
    // DC A
    int assembly = 0, active = 0;
    bool request = false;
    unsigned pendingActions = 0;
    // CA
    bool caFree[kSurfaces] = {true, true, true};
    uint64_t lastCompletedSurface = UINT64_MAX, nextId = 1;
    std::deque<Swap> queue;  // the family's queue, oldest first
    uint64_t takeSeq = 0, entrySeq[TegraScanoutRing::kMaxEntries] = {};
    uint64_t entryId[TegraScanoutRing::kMaxEntries] = {};
    // the display
    bool inFlight = false, deferred = false;
    uint64_t inFlightId = 0, inFlightDoneTick = 0;
    unsigned inFlightSurface = 0;
    uint64_t tick = 0, presents = 0, completions = 0, onTime = 0, measured = 0, violations = 0, lastShownId = 0,
             lastReadyId = 0;
    unsigned renderTicks = 4, renderJitter = 0;

    explicit Sim(bool early) : earlyRetire(early) { ring.reset(3); }

    bool latchable(int entry) const { return entry == active || (request && entry == assembly); }
    // CA's completion callback for swap s: the surface of the previously completed swap is recycled.
    void complete(const Swap &s)
    {
        ++completions;
        if (lastCompletedSurface != UINT64_MAX && lastCompletedSurface != s.surface) caFree[lastCompletedSurface] = true;
        lastCompletedSurface = s.surface;
    }
    Swap *find(uint64_t id)
    {
        for (Swap &s : queue)
            if (s.id == id) return &s;
        return nullptr;
    }
    void retireThrough(uint64_t seq)
    {
        while (!queue.empty() && queue.front().seq && queue.front().seq <= seq &&
               !(inFlight && queue.front().id == inFlightId)) {
            complete(queue.front());
            queue.pop_front();
        }
    }
    void caDraw()
    {
        for (unsigned surface = 0; surface < kSurfaces; ++surface) {
            if (!caFree[surface]) continue;
            const int entry = kSlot0 + (int)surface;
            if (latchable(entry) || ring.busy(entry)) {
                ++violations;  // CA draws into a surface DC A scans, may take, or the ring holds
                CHECK(earlyRetire, "CA drew into surface %u: active %d assembly %d request %d front %d pending %d "
                      "superseded %d held %d", surface, active, assembly, request, ring.front, ring.pending,
                      ring.superseded, ring.held);
            }
            caFree[surface] = false;
            queue.push_back({nextId++, surface, 0, tick, tick + renderTicks + (renderJitter ? rnd(renderJitter) : 0)});
            kick();
            return;
        }
    }
    void kick()
    {
        if (inFlight) return;
        Swap *next = nullptr;
        for (Swap &s : queue)
            if (!s.seq) {
                next = &s;
                break;
            }
        if (!next) return;
        if (ring.held != kNone || ring.superseded != kNone || ring.busy(kSlot0 + (int)next->surface)) {
            deferred = true;
            return;
        }
        next->seq = ++takeSeq;
        inFlight = true;
        inFlightId = next->id;
        inFlightSurface = next->surface;
        inFlightDoneTick = next->readyTick > tick ? next->readyTick : tick + 1;  // the fence-only present
        if (earlyRetire) {  // the copy path's retire at the offer, wrongly applied to a direct frame
            while (!queue.empty() && queue.front().seq) {
                complete(queue.front());
                queue.pop_front();
            }
        }
    }
    void fenceDone()
    {
        if (!inFlight || tick < inFlightDoneTick) return;
        inFlight = false;
        lastReadyId = inFlightId;
        const int entry = kSlot0 + (int)inFlightSurface;
        const TegraScanoutRing::Ready how = ring.ready(entry, true);
        Swap *s = find(inFlightId);
        entrySeq[entry] = s ? s->seq : takeSeq;
        entryId[entry] = inFlightId;
        if (how != TegraScanoutRing::kHold) {
            assembly = entry;  // START_ADDR, then (possibly after a boundary) ACT_REQ
            if (!rnd(6)) boundary();
            request = true;
        }
        kick();
    }
    void boundary()
    {
        if (request) {
            active = assembly;
            request = false;
        }
        ++pendingActions;
    }
    void frameEndAction()
    {
        if (!pendingActions) return;
        pendingActions = 0;
        if (ring.pending == kNone) return;
        const int asked = ring.pending, replaced = ring.superseded;
        switch (ring.latch(active, request)) {
        case TegraScanoutRing::kTaken:
            ++presents;
            noteShown(entryId[asked]);
            if (!earlyRetire) retireThrough(entrySeq[asked]);
            break;
        case TegraScanoutRing::kTakenReplaced:
            ++presents;
            noteShown(entryId[replaced]);
            if (!earlyRetire) retireThrough(entrySeq[replaced]);
            break;
        case TegraScanoutRing::kInconsistent:
            CHECK(false, "inconsistent latch: active %d assembly %d request %d", active, assembly, request);
            break;
        case TegraScanoutRing::kWaiting:
            return;
        }
        if (deferred) {
            deferred = false;
            kick();
        }
    }
    void noteShown(uint64_t id) { lastShownId = id > lastShownId ? id : lastShownId; }
    // One frame: the boundary, the frame-end action (sometimes folded into the next), CA's vsync-driven draw.
    void run(unsigned frames)
    {
        for (unsigned f = 0; f < frames; ++f) {
            for (unsigned t = 0; t < kTicks; ++t, ++tick) {
                if (t == 0) {
                    const uint64_t ready = lastReadyId;  // the newest frame whose rendering finished before this boundary
                    boundary();
                    if (ready && f > 10) {
                        ++measured;
                        onTime += entryId[active] >= ready;  // what DC A scans from this boundary on
                    }
                    if (rnd(8)) frameEndAction();
                }
                if (t == 2) caDraw();  // CA renders on its vsync, a few ticks after the boundary
                fenceDone();
                if (t == 9) frameEndAction();  // a late or folded frame-end action
            }
        }
    }
};
}  // namespace

int main()
{
    // CA keeps up: rendering takes a quarter frame.
    Sim fast(false);
    fast.run(2000);
    std::printf("fast: %llu presents, %llu completions in 2000 frames, %llu of %llu on screen at the first boundary\n",
                (unsigned long long)fast.presents, (unsigned long long)fast.completions,
                (unsigned long long)fast.onTime, (unsigned long long)fast.measured);
    CHECK(fast.presents > 1900, "fast CA: only %llu presents", (unsigned long long)fast.presents);
    CHECK(fast.onTime == fast.measured, "fast CA: a ready frame was not on screen at the next boundary");
    CHECK(!fast.violations, "fast CA: %llu violations", (unsigned long long)fast.violations);

    // CA sometimes late: rendering 2 to 30 ticks (up to two frames), replacements happen.
    for (unsigned seed = 1; seed <= 50; ++seed) {
        rng = 0x9e3779b97f4a7c15ull * seed;
        Sim slow(false);
        slow.renderTicks = 2;
        slow.renderJitter = 28;
        slow.run(1000);
        CHECK(!slow.violations, "seed %u: %llu violations", seed, (unsigned long long)slow.violations);
        CHECK(slow.presents > 300, "seed %u: only %llu presents (deadlock?)", seed, (unsigned long long)slow.presents);
        CHECK(slow.completions + slow.queue.size() + 0 >= slow.nextId - 1 - 3, "seed %u: swaps lost", seed);
    }

    // Negative control: early retire on direct frames lets CA draw into a scanned surface.
    uint64_t bad = 0;
    for (unsigned seed = 1; seed <= 20; ++seed) {
        rng = 0x51ed27f1ull * seed;
        Sim early(true);
        early.renderTicks = 2;
        early.renderJitter = 28;
        early.run(500);
        bad += early.violations;
    }
    std::printf("negative control (retire at the offer): %llu draws into a scanned surface\n", (unsigned long long)bad);
    CHECK(bad > 0, "early retire should be caught drawing into a scanned surface");

    if (failures) {
        std::printf("FAIL: direct policy (%d failures)\n", failures);
        return 1;
    }
    std::printf("PASS: direct policy\n");
    return 0;
}
