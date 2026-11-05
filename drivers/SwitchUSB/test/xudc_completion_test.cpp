// drivers/SwitchUSB's device-mode completion accounting (XudcTransfer.h "Completion accounting", TdRing) on the host,
// against a model XUDC: which request each transfer event is for, and that every request completes exactly once, in
// order, with the bytes the controller put in it.
//
// The model controller works through the TDs on a transfer ring the way xHCI and Linux tegra-xudc describe it: TRBs in
// ring order, a TD ends at its TRB without CHAIN; OUT packets fill whole-packet TRBs; an IOC TRB that fills raises an
// event; a short packet (or a zero-length one) ends the TD with an event on the TRB it landed in (ISP), the residue
// that TRB's or the TD's, and the controller then skips the rest of the TD, a while later, reading those TRBs as it
// goes, and may raise a second event on the TD's last TRB (IOC) when it gets there. Events go through one FIFO of a
// fixed size.
//
// The model driver is SwitchXUDC.cpp's data path in miniature: requests laid out by XudcTransfer.h plan() from
// mbuf-like scatter-gather onto the ring within TdRing::room(), events matched by TdRing::decide/apply, the next
// request queued from each completion (as AppleUSBDeviceMux does), the serial path when only a reserved tail holds the
// room, a cancel fencing the ring until a drain has emptied the event ring. The model also checks the controller's
// side: the driver never writes a TRB the controller has not passed yet.
//
// LegacyMatching runs on the same model: an event completes the request whose TRBs hold it and fails every OUT request
// before it with 0 bytes, and the TRBs free at once. Second events alone cost it nothing (its one free slot is always
// the short TD's last TRB) but it rewrites TRBs the controller is still skipping, and a 64-event ring overflows under a
// burst, which it turns into silently failed reads.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "../XudcTransfer.h"

namespace {

using xudc::Decision;
using xudc::Outcome;
using xudc::Verdict;

constexpr uint32_t kMps = 512;
constexpr uint32_t kSerialBytes = 65536;

struct Rng {
    std::mt19937_64 engine;
    explicit Rng(uint64_t seed) : engine(seed) {}
    uint64_t below(uint64_t n) { return std::uniform_int_distribution<uint64_t>(0, n - 1)(engine); }
    uint64_t range(uint64_t lo, uint64_t hi) { return std::uniform_int_distribution<uint64_t>(lo, hi)(engine); }
    bool chance(double p) { return std::uniform_real_distribution<double>(0.0, 1.0)(engine) < p; }
};

struct Ev {
    uint32_t dci, index, code, residue;
};

// The event ring as the driver reads it: in order; one slot stays empty, as the xHCI producer counts it. An event
// that finds it full is dropped (counted): the hardware's overflow.
struct EventRing {
    explicit EventRing(size_t slots) : capacity(slots) {}
    size_t capacity;
    std::deque<Ev> fifo;
    size_t deepest = 0;
    uint64_t dropped = 0;
    void raise(const Ev &e)
    {
        if (fifo.size() + 1 >= capacity) {
            ++dropped;
            return;
        }
        fifo.push_back(e);
        deepest = std::max(deepest, fifo.size());
    }
};

// What the controller does on a TD's last TRB after a short packet ended the TD before it.
enum class Second { None, ShortCode, SuccessCode };

struct Behaviour {
    bool perTd;     // the residue the whole TD's (else the event's TRB's)
    Second second;  // the second event
};

template <uint32_t Slots> struct Wire {
    struct Trb {
        uint32_t length, flags;
        uint64_t request;  // which request's TD this TRB belongs to (the check's bookkeeping, not the hardware's)
    };
    Trb trb[Slots] = {};
    uint32_t enqueue = 0;     // the driver's producer position (Hardware::pushData)
    uint32_t dequeue = 0;     // the next TRB the controller reads
    uint32_t owned = 0;       // TRBs written that the controller has not passed yet
    uint64_t overwrites = 0;  // TRBs written over ones the controller had not passed (a driver bug)

    uint32_t push(uint32_t length, uint32_t flags, uint64_t request)
    {
        if (owned >= Slots) ++overwrites;
        const uint32_t index = enqueue;
        trb[index] = {length, flags, request};
        enqueue = (enqueue + 1) % Slots;
        ++owned;
        return index;
    }
};

// The TD the controller is on (or skipping the rest of).
struct Progress {
    bool active;
    uint32_t first, trbs, j, fill, got, total;
    uint64_t request;
};

template <uint32_t Slots> struct Controller {
    uint32_t dci = 0;
    bool in = false;
    Behaviour how = {};
    Wire<Slots> wire;
    EventRing *events = nullptr;
    Progress cur = {}, skip = {};
    bool skipping = false;
    std::map<uint64_t, uint64_t> delivered;  // bytes the controller moved per request
    uint64_t babble = 0;  // a packet that did not fit its TRB (the layout's whole-packet rule broken)

    static uint32_t at(uint32_t first, uint32_t i) { return (first + i) % Slots; }

    bool load()
    {
        if (cur.active) return true;
        if (skipping || !wire.owned) return false;
        cur = {true, wire.dequeue, 0, 0, 0, 0, 0, wire.trb[wire.dequeue].request};
        for (uint32_t i = 0;; ++i) {
            const auto &t = wire.trb[at(cur.first, i)];
            cur.total += t.length;
            if (!(t.flags & xudc::TRB_CHAIN)) {
                cur.trbs = i + 1;
                break;
            }
        }
        return true;
    }
    void pass(const Progress &p)
    {
        wire.dequeue = at(p.first, p.trbs);
        wire.owned -= p.trbs;
    }

    // OUT: the host's next packet (0..mps bytes). False: no TD to take it (the controller NAKs; the host retries).
    bool packet(uint32_t bytes)
    {
        if (!load()) return false;
        const auto &t = wire.trb[at(cur.first, cur.j)];
        if (bytes > t.length - cur.fill) {
            ++babble;
            bytes = t.length - cur.fill;
        }
        cur.fill += bytes;
        cur.got += bytes;
        delivered[cur.request] += bytes;
        if (bytes < kMps) {  // short (or zero-length): the TD ends here
            const uint32_t residue = how.perTd ? cur.total - cur.got : t.length - cur.fill;
            events->raise({dci, at(cur.first, cur.j), xudc::COMP_SHORT_PACKET, residue});
            if (cur.j + 1 < cur.trbs) {
                skipping = true;
                skip = cur;
            } else {
                pass(cur);
            }
            cur.active = false;
            return true;
        }
        if (cur.fill == t.length) {
            if (t.flags & xudc::TRB_IOC)
                events->raise({dci, at(cur.first, cur.j), xudc::COMP_SUCCESS, how.perTd ? cur.total - cur.got : 0});
            if (cur.j + 1 == cur.trbs) {
                pass(cur);
                cur.active = false;
            } else {
                ++cur.j;
                cur.fill = 0;
            }
        }
        return true;
    }

    // The rest of a TD after its short packet: walked, its second event if this controller raises one.
    bool finishSkip()
    {
        if (!skipping) return false;
        const uint32_t last = at(skip.first, skip.trbs - 1);
        if (how.second == Second::ShortCode)
            events->raise({dci, last, xudc::COMP_SHORT_PACKET, how.perTd ? skip.total - skip.got : 0});
        if (how.second == Second::SuccessCode) events->raise({dci, last, xudc::COMP_SUCCESS, 0});
        pass(skip);
        skipping = false;
        return true;
    }

    // IN: the next TD, all of it.
    bool sendIn()
    {
        if (!load()) return false;
        delivered[cur.request] += cur.total;
        events->raise({dci, at(cur.first, cur.trbs - 1), xudc::COMP_SUCCESS, 0});
        pass(cur);
        cur.active = false;
        return true;
    }

    // Hardware::dropQueued: paused, the dequeue moved to the enqueue position; what was in progress is gone.
    void drop()
    {
        cur.active = false;
        skipping = false;
        wire.dequeue = wire.enqueue;
        wire.owned = 0;
    }
};

struct Layout {
    std::vector<xudc::Run> runs;
    uint32_t length;
};

// What AppleUSBDeviceMux hands the controller: a read or a write over mbufs: 2 KiB clusters, 4 KiB pages, 16 KiB jumbo
// clusters, or one wired buffer; anywhere below 4 GiB (whole packets: reads are multiples of the packet size).
Layout mbufs(Rng &rng, uint32_t length)
{
    Layout l{{}, length};
    const uint32_t chunk = static_cast<uint32_t>(std::vector<uint64_t>{2048, 4096, 16384, 65536}[rng.below(4)]);
    for (uint32_t off = 0; off < length; off += chunk) {
        const uint32_t take = std::min(chunk, length - off);
        l.runs.push_back({0x8000'0000 + rng.below(1u << 16) * 65536, take});
    }
    return l;
}

struct Completion {
    uint64_t request;
    Outcome outcome;
    uint32_t actual;
};

struct Tally {
    uint64_t heads = 0, second = 0, stale = 0, lost = 0, unexplained = 0, serial = 0, shortEarly = 0, failed = 0;
};

// The legacy matching: the request whose TRBs hold the event completes, every one before it
// with it (OUT: failed with 0 bytes), and its TRBs are free at once.
struct LegacyMatching {};

template <uint32_t Slots, class Matching = void> struct Endpoint {
    using Tds = xudc::TdRing<Slots, 64>;
    static constexpr uint32_t kMaxPieces = Slots - 2;  // SwitchXUDC.cpp kMaxPieces = kDataRingTrbs - 3
    struct Req {
        uint64_t id;
        Layout layout;
        uint32_t done;
    };

    Controller<Slots> hw;
    Tds tds{};
    std::deque<Req> queue;
    bool serialBusy = false;
    uint32_t serialLength = 0;
    uint64_t nextId = 0;
    std::vector<Completion> completed;
    Tally tally;
    uint8_t *known;  // the driver's OUT residue reading (one per driver, as SwitchXUDC::residue)

    bool out() const { return !hw.in; }

    enum class Admit { Queued, Wait, Serial };
    Admit admit(Req &r)
    {
        const bool probe = out() && *known == xudc::kResidueUnknown && r.layout.length >= 2 * kMps;
        const uint32_t room = tds.room();
        std::vector<xudc::Piece> pieces(kMaxPieces);
        xudc::PlanStats why = {};
        const auto runs = static_cast<uint32_t>(r.layout.runs.size());
        const uint32_t n = xudc::plan(r.layout.runs.data(), runs, r.layout.length, kMps, out(), probe, 1ull << 32,
                                      pieces.data(), std::min(room, kMaxPieces), &why);
        if (!n) {
            xudc::PlanStats ignored = {};
            const bool fits = xudc::plan(r.layout.runs.data(), runs, r.layout.length, kMps, out(), probe, 1ull << 32,
                                         pieces.data(), kMaxPieces, &ignored) != 0;
            return fits && tds.onRing() ? Admit::Wait : Admit::Serial;
        }
        if (out() && n > 1 && (*known == xudc::kResidueBroken || (*known == xudc::kResidueUnknown && !probe)))
            return Admit::Serial;
        const uint32_t base = (xudc::TRB_NORMAL << 10) | (out() ? xudc::TRB_ISP : 0);
        uint32_t first = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const bool last = i + 1 == n;
            const uint32_t flags =
                base | (last ? xudc::TRB_IOC : xudc::TRB_CHAIN) | (probe && i == 0 && !last ? xudc::TRB_IOC : 0);
            const uint32_t index = hw.wire.push(pieces[i].length, flags, r.id);
            if (!i) first = index;
        }
        tds.push({static_cast<uint16_t>(first), static_cast<uint16_t>(n), r.layout.length, probe && n > 1});
        return Admit::Queued;
    }

    void serialPiece()
    {
        Req &r = queue.front();
        serialLength = std::min(r.layout.length - r.done, kSerialBytes);
        const uint32_t index =
            hw.wire.push(serialLength, (xudc::TRB_NORMAL << 10) | xudc::TRB_ISP | xudc::TRB_IOC, r.id);
        tds.push({static_cast<uint16_t>(index), 1, serialLength, false});
    }

    void kick()
    {
        while (!serialBusy && !tds.fenced() && tds.onRing() < queue.size()) {
            const Admit a = admit(queue[tds.onRing()]);
            if (a == Admit::Queued) continue;
            if (a == Admit::Wait || tds.onRing()) break;
            ++tally.serial;
            serialBusy = true;
            queue.front().done = 0;
            serialPiece();
            break;
        }
    }

    void enqueue(const Layout &l)
    {
        queue.push_back({nextId++, l, 0});
        kick();
    }

    // The head is done: off the queue, the function told (it queues its next transfer), the ring refilled.
    std::function<void(Endpoint &)> onComplete;
    void completeHead(Outcome outcome, uint32_t actual)
    {
        const Req r = queue.front();
        queue.pop_front();
        completed.push_back({r.id, outcome, actual});
        if (outcome != Outcome::Done) ++tally.failed;
        if (onComplete) onComplete(*this);
        kick();
    }

    // SwitchXUDC::transferEvent.
    void event(const Ev &e)
    {
        if constexpr (std::is_same_v<Matching, LegacyMatching>) return legacyEvent(e);
        const Decision d =
            tds.decide(e.index, e.code, e.residue, hw.in, *known, [this](uint32_t t) { return hw.wire.trb[t].length; });
        switch (d.verdict) {
        case Verdict::Head: break;
        case Verdict::SecondEvent:
            ++tally.second;
            tds.apply(d);
            return kick();
        case Verdict::Stale: ++tally.stale; return;
        case Verdict::Lost: ++tally.lost; return;
        case Verdict::Unexplained: ++tally.unexplained; return;
        }
        ++tally.heads;
        if (d.learned != xudc::kResidueUnknown && d.learned != *known && *known != xudc::kResidueBroken)
            *known = d.learned;
        if (serialBusy) {
            tds.apply(d);
            Req &r = queue.front();
            const uint32_t actual = d.outcome == Outcome::Done ? d.actual : 0;
            r.done += actual;
            if (d.outcome == Outcome::Done && r.done < r.layout.length && actual == serialLength) return serialPiece();
            serialBusy = false;
            return completeHead(d.outcome, r.done);
        }
        if (d.outcome == Outcome::Pending) {
            tds.apply(d);
            return kick();
        }
        if (d.armTail) ++tally.shortEarly;
        tds.apply(d);
        completeHead(d.outcome, d.outcome == Outcome::Done ? d.actual : 0);
    }

    void legacyEvent(const Ev &e)
    {
        uint32_t k = 0, pos = 0;
        for (; k < tds.onRing(); ++k) {
            const auto &t = tds.td(k);
            pos = (e.index + Slots - t.first) % Slots;
            if (pos < t.trbs) break;
        }
        if (k == tds.onRing()) return;  // no request holds it: ignored
        const auto &t = tds.td(k);
        const bool shortPacket = e.code == xudc::COMP_SHORT_PACKET;
        uint32_t actual = 0;
        if (hw.in) {
            actual = e.residue <= t.bytes ? t.bytes - e.residue : 0;
        } else {
            std::vector<uint32_t> lengths;
            for (uint32_t i = 0; i < t.trbs; ++i) lengths.push_back(hw.wire.trb[Tds::at(t.first, i)].length);
            const xudc::OutResult o = xudc::outEvent(*known, lengths.data(), t.trbs, pos, e.residue, shortPacket);
            if (o.learned != xudc::kResidueUnknown && o.learned != *known && *known != xudc::kResidueBroken)
                *known = o.learned;
            if (!o.done) return;
            actual = o.failed ? 0 : o.actual;
        }
        for (uint32_t i = 0; i <= k; ++i) {
            const bool mine = i == k;
            const Outcome outcome = mine || hw.in ? Outcome::Done : Outcome::Error;
            const uint32_t moved = mine ? actual : hw.in ? tds.td(0).bytes : 0;
            tds.apply({Verdict::Head, Outcome::Done, false, xudc::kResidueUnknown, 0, 0, 0});  // TRBs free at once
            completeHead(outcome, moved);
        }
    }

    // SwitchXUDC::cancel: the controller stopped, every request aborted, the ring fenced (lifted at once when the event
    // ring is empty and no drain is under way).
    void cancel(bool draining, const EventRing &events)
    {
        hw.drop();
        serialBusy = false;
        std::deque<Req> aborted;
        aborted.swap(queue);
        if (tds.cancel() && !draining && events.fifo.empty()) tds.liftFence();
        for (const Req &r : aborted) completed.push_back({r.id, Outcome::Error, 0});
    }
};

// A request's bytes as the controller moved them, against what the driver reported.
template <class E> void checkCompletions(const E &ep, bool aborts, const char *what)
{
    SCOPED_TRACE(what);
    uint64_t expect = 0;
    for (const Completion &c : ep.completed) {
        ASSERT_EQ(c.request, expect) << "every request completes once, in order";
        ++expect;
        if (c.outcome != Outcome::Done) {
            ASSERT_TRUE(aborts) << "request " << c.request << " failed";
            continue;
        }
        const auto it = ep.hw.delivered.find(c.request);
        const uint64_t moved = it == ep.hw.delivered.end() ? 0 : it->second;
        ASSERT_EQ(c.actual, moved) << "request " << c.request << ": the length is what the controller moved";
    }
}

template <uint32_t Slots, class Matching = void> struct Session {
    using Ep = Endpoint<Slots, Matching>;
    Rng rng;
    EventRing events;
    uint8_t known = xudc::kResidueUnknown;
    Ep outEp, inEp;
    std::deque<uint32_t> packets;  // the host's OUT packets still to send (each <= mps; a short one ends a transfer)
    uint64_t hostBytes = 0, messages = 0, cancels = 0;
    bool draining = false;
    double cancelChance = 0;
    uint32_t readBytes;
    size_t perEndpointDeepest = 0;

    Session(uint64_t seed, Behaviour how, size_t eventCapacity, uint32_t reads, uint32_t depth)
        : rng(seed), events(eventCapacity), readBytes(reads)
    {
        for (Ep *ep : {&outEp, &inEp}) {
            ep->hw.dci = ep == &outEp ? 2 : 3;
            ep->hw.in = ep == &inEp;
            ep->hw.how = how;
            ep->hw.events = &events;
        }
        outEp.known = inEp.known = &known;
        // AppleUSBDeviceMux: a new read for each one done, a new write while it has data.
        outEp.onComplete = [this](Ep &ep) {
            if (cancelChance && rng.chance(cancelChance)) return cancel(rng.chance(0.5) ? inEp : ep);
            ep.queue.push_back({ep.nextId++, mbufs(rng, readBytes), 0});
        };
        inEp.onComplete = [this](Ep &ep) { ep.queue.push_back({ep.nextId++, write(), 0}); };
        for (uint32_t i = 0; i < depth; ++i) {
            outEp.enqueue(mbufs(rng, readBytes));
            inEp.enqueue(write());
        }
    }

    Layout write()
    {
        const auto length = static_cast<uint32_t>(rng.chance(0.7) ? rng.range(16, 2048) : rng.range(2048, 65536));
        Layout l{{}, length};
        for (uint32_t off = 0; off < length;) {  // mbufs at odd places: IN has no cache-line rule
            const auto take = std::min<uint32_t>(length - off, static_cast<uint32_t>(rng.range(1, 8192)));
            l.runs.push_back({0x9000'0000 + rng.below(1u << 20) * 64 + rng.below(64), take});
            off += take;
        }
        return l;
    }

    // One mux packet from the Mac: mostly TCP acknowledgements and window updates (a 16-byte mux header and a TCP
    // header), some data, now and then more than one read holds; a transfer of whole packets ends with a ZLP.
    void message()
    {
        const double r = rng.range(0, 999) / 1000.0;
        const auto length = static_cast<uint32_t>(r < 0.75   ? rng.range(36, 120)
                                                  : r < 0.90 ? rng.range(121, 4096)
                                                  : r < 0.97 ? kMps * rng.range(1, 32)
                                                             : rng.range(4097, 3 * readBytes));
        for (uint32_t left = length;; left -= kMps) {
            if (left < kMps) {
                packets.push_back(left);
                break;
            }
            packets.push_back(kMps);
        }
        hostBytes += length;
        ++messages;
    }

    void cancel(Ep &ep)
    {
        ++cancels;
        // Bytes the host sent into a TD that is gone: an abort loses them (both sides of the check drop them).
        ep.cancel(draining, events);
        for (uint32_t i = 0; i < 8; ++i)
            ep.queue.push_back({ep.nextId++, &ep == &inEp ? write() : mbufs(rng, readBytes), 0});
        ep.kick();
    }

    // One step of the hardware: a packet from the host, an IN TD to the host, or the end of a skipped TD.
    bool hardwareStep()
    {
        switch (rng.below(3)) {
        case 0:
            if (packets.empty()) return false;
            if (!outEp.hw.packet(packets.front())) return false;
            packets.pop_front();
            return true;
        case 1: return inEp.hw.sendIn();
        default: return outEp.hw.finishSkip() || inEp.hw.finishSkip();
        }
    }

    size_t pendingFor(uint32_t dci) const
    {
        return static_cast<size_t>(
            std::count_if(events.fifo.begin(), events.fifo.end(), [dci](const Ev &e) { return e.dci == dci; }));
    }

    // SwitchXUDC::serviceEvents: until the event ring is empty, the hardware going on meanwhile; then the fences lift.
    void drain(double concurrency)
    {
        draining = true;
        perEndpointDeepest = std::max({perEndpointDeepest, pendingFor(2), pendingFor(3)});
        while (!events.fifo.empty()) {
            const Ev e = events.fifo.front();
            events.fifo.pop_front();
            (e.dci == 2 ? outEp : inEp).event(e);
            while (rng.chance(concurrency)) hardwareStep();
        }
        draining = false;
        const bool liftOut = outEp.tds.fenced(), liftIn = inEp.tds.fenced();
        if (liftOut) outEp.tds.liftFence();
        if (liftIn) inEp.tds.liftFence();
        if (liftOut) outEp.kick();
        if (liftIn) inEp.kick();
    }

    // Runs the session for `count` host messages, then until everything sent has been read.
    void run(uint64_t count, double concurrency, uint32_t burst)
    {
        while (messages < count || !packets.empty() || outEp.hw.skipping || inEp.hw.skipping || !events.fifo.empty()) {
            if (messages < count && packets.size() < 64) message();
            const uint32_t steps = static_cast<uint32_t>(rng.range(1, burst));
            for (uint32_t i = 0; i < steps; ++i) hardwareStep();
            if (rng.chance(0.5) || messages >= count) drain(concurrency);
            if (outEp.tally.lost || inEp.tally.lost) return;
        }
    }
};

template <uint32_t Slots> void checkSession(Session<Slots> &s, bool aborts)
{
    for (auto *ep : {&s.outEp, &s.inEp}) {
        SCOPED_TRACE(ep->hw.in ? "IN" : "OUT");
        EXPECT_EQ(ep->tally.lost, 0u) << "no event lost";
        EXPECT_EQ(ep->tally.unexplained, 0u) << "every event for a request or a cancel";
        EXPECT_EQ(ep->hw.wire.overwrites, 0u) << "never a TRB the controller had not passed";
        EXPECT_EQ(ep->hw.babble, 0u) << "every packet fit its TRB";
        if (!aborts) EXPECT_EQ(ep->tally.failed, 0u) << "no request failed";
        checkCompletions(*ep, aborts, ep->hw.in ? "IN" : "OUT");
        EXPECT_LE(s.perEndpointDeepest, Slots - 1) << "events owed per endpoint fit the event ring's sizing";
    }
    EXPECT_EQ(s.events.dropped, 0u) << "the event ring never filled";
    if (!aborts) {  // every byte the Mac sent reached a completed read, in order
        uint64_t got = 0;
        for (const Completion &c : s.outEp.completed) got += c.actual;
        EXPECT_EQ(got, s.hostBytes);
    }
}

std::string describe(const Behaviour &how)
{
    return std::string(how.perTd ? "residue per TD" : "residue per TRB") +
           (how.second == Second::None        ? ", one event per short TD"
            : how.second == Second::ShortCode ? ", second event (short packet) on the last TRB"
                                              : ", second event (success) on the last TRB");
}

const Behaviour kBehaviours[] = {
    {false, Second::None}, {false, Second::ShortCode}, {false, Second::SuccessCode},
    {true, Second::None},  {true, Second::ShortCode},  {true, Second::SuccessCode},
};

using Ring255 = xudc::TdRing<255, 64>;  // the driver's: kDataRingTrbs - 1
using Ring15 = xudc::TdRing<15, 8>;

// A TD's TRB lengths for decide(): ring index -> length.
struct Lengths {
    std::map<uint32_t, uint32_t> at;
    uint32_t operator()(uint32_t index) const { return at.at(index); }
};

template <class R> void put(R &ring, Lengths &lengths, uint32_t first, std::vector<uint32_t> trbs, bool probe = false)
{
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < trbs.size(); ++i) {
        lengths.at[R::at(first, i)] = trbs[i];
        bytes += trbs[i];
    }
    ring.push({static_cast<uint16_t>(first), static_cast<uint16_t>(trbs.size()), bytes, probe});
}

}  // namespace

TEST(XudcCompletion, ShortPacketOneEvent)
{
    Ring255 ring{};
    Lengths len;
    put(ring, len, 0, {512, 2048, 2048});  // an OUT read of three TRBs; 100 bytes arrive
    EXPECT_EQ(ring.room(), 254u - 3);
    Decision d = ring.decide(0, xudc::COMP_SHORT_PACKET, 412, false, xudc::kResiduePerTrb, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.outcome, Outcome::Done);
    EXPECT_EQ(d.actual, 100u);
    EXPECT_TRUE(d.armTail);
    ring.apply(d);
    EXPECT_EQ(ring.onRing(), 0u);
    EXPECT_EQ(ring.tailTrbs(), 3u) << "the TD's TRBs stay reserved";
    EXPECT_EQ(ring.room(), 254u - 3);
    put(ring, len, 3, {2048, 2048});  // the next read; this controller raises no second event
    d = ring.decide(4, xudc::COMP_SUCCESS, 0, false, xudc::kResiduePerTrb, len);
    ASSERT_EQ(d.verdict, Verdict::Head) << "the next TD's own event";
    EXPECT_EQ(d.actual, 4096u);
    EXPECT_FALSE(d.armTail);
    ring.apply(d);
    EXPECT_EQ(ring.tailTrbs(), 0u) << "the controller is past the tail";
    EXPECT_EQ(ring.room(), 254u);
}

TEST(XudcCompletion, ShortPacketTwoEvents)
{
    for (const uint32_t code : {xudc::COMP_SHORT_PACKET, xudc::COMP_SUCCESS}) {
        Ring255 ring{};
        Lengths len;
        put(ring, len, 10, {512, 2048, 2048});
        put(ring, len, 13, {512, 2048});
        Decision d = ring.decide(10, xudc::COMP_SHORT_PACKET, 4508, false, xudc::kResiduePerTd, len);  // 100 bytes
        ASSERT_EQ(d.verdict, Verdict::Head);
        EXPECT_EQ(d.actual, 100u);
        ring.apply(d);
        d = ring.decide(12, code, 0, false, xudc::kResiduePerTd, len);  // the last TRB's IOC event
        EXPECT_EQ(d.verdict, Verdict::SecondEvent) << "absorbed, whatever its code";
        ring.apply(d);
        EXPECT_EQ(ring.tailTrbs(), 0u);
        EXPECT_EQ(ring.onRing(), 1u) << "the next read untouched";
        d = ring.decide(14, xudc::COMP_SHORT_PACKET, 2048, false, xudc::kResiduePerTd, len);  // a ZLP after 512
        ASSERT_EQ(d.verdict, Verdict::Head);
        EXPECT_EQ(d.actual, 512u);
        EXPECT_FALSE(d.armTail) << "short on its last TRB: one event";
    }
}

TEST(XudcCompletion, MultiTrbTd)
{
    Ring255 ring{};
    Lengths len;
    put(ring, len, 0, {4096, 4096, 4096, 1024});
    Decision d = ring.decide(1, xudc::COMP_SUCCESS, 0, false, xudc::kResiduePerTrb, len);
    EXPECT_EQ(d.verdict, Verdict::Unexplained) << "a full TRB without IOC: nothing asked for it";
    d = ring.decide(3, xudc::COMP_SUCCESS, 0, false, xudc::kResiduePerTrb, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 13312u);
    d = ring.decide(3, xudc::COMP_SUCCESS, 0, true, xudc::kResiduePerTrb, len);  // IN
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 13312u);
    d = ring.decide(2, xudc::COMP_SHORT_PACKET, 4096 - 7, false, xudc::kResiduePerTrb, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 8192u + 7);
    EXPECT_TRUE(d.armTail);
    d = ring.decide(2, 4 /* USB transaction error */, 0, false, xudc::kResiduePerTrb, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.outcome, Outcome::Error);
    EXPECT_TRUE(d.armTail) << "ended before its last TRB: reserved as after a short packet";
}

TEST(XudcCompletion, ProbeOnTheFirstTrb)
{
    Ring255 ring{};
    Lengths len;
    put(ring, len, 0, {512, 4096}, true);
    Decision d = ring.decide(0, xudc::COMP_SUCCESS, 4096, false, xudc::kResidueUnknown, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.outcome, Outcome::Pending);
    EXPECT_EQ(d.learned, xudc::kResiduePerTd);
    ring.apply(d);
    EXPECT_EQ(ring.onRing(), 1u);
    d = ring.decide(1, xudc::COMP_SHORT_PACKET, 4000, false, xudc::kResiduePerTd, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 608u);
}

TEST(XudcCompletion, RingWrap)
{
    Ring15 ring{};
    Lengths len;
    put(ring, len, 12, {512, 512, 512, 512, 512});  // TRBs 12, 13, 14, 0, 1
    put(ring, len, 2, {512, 512});                  // 2, 3
    EXPECT_EQ(ring.room(), 14u - 7);
    for (const uint32_t index : {12u, 13u, 14u, 0u}) {
        const Decision d = ring.decide(index, xudc::COMP_SHORT_PACKET, 512, false, xudc::kResiduePerTrb, len);
        ASSERT_EQ(d.verdict, Verdict::Head) << index;
        EXPECT_EQ(d.pos, (index + 15 - 12) % 15);
    }
    Decision d = ring.decide(1, xudc::COMP_SUCCESS, 0, false, xudc::kResiduePerTrb, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 2560u);
    d = ring.decide(3, xudc::COMP_SUCCESS, 0, false, xudc::kResiduePerTrb, len);
    EXPECT_EQ(d.verdict, Verdict::Lost) << "the second TD's event while the first has had none";
    EXPECT_EQ(d.k, 1u);
    EXPECT_EQ(d.pos, 1u);
    d = ring.decide(7, xudc::COMP_SUCCESS, 0, false, xudc::kResiduePerTrb, len);
    EXPECT_EQ(d.verdict, Verdict::Unexplained) << "a free TRB";
    d = ring.decide(Ring15::kNoTrb, xudc::COMP_SUCCESS, 0, false, xudc::kResiduePerTrb, len);
    EXPECT_EQ(d.verdict, Verdict::Unexplained) << "outside the ring";
}

// The ring full of mux reads and the head ends at a short packet on its first TRB: the next read cannot go onto the
// TD's TRBs (the controller may still be skipping through them) until an event shows the controller past it. Without
// the tail, 11 would be free at once from TRB 252: 252..254 and 0..7, the TD's own; the one free slot, TRB 8, its last.
TEST(XudcCompletion, TailKeepsTheSkippedTrbs)
{
    Ring255 ring{};
    Lengths len;
    uint32_t enqueue = 0;
    std::vector<uint32_t> nine(9, 2048);
    nine[0] = 512;
    while (ring.room() >= 9) {  // 28 reads of 9 TRBs: 252 of 254
        put(ring, len, enqueue, nine);
        enqueue = Ring255::at(enqueue, 9);
    }
    ASSERT_EQ(ring.onRing(), 28u);
    Decision d = ring.decide(0, xudc::COMP_SHORT_PACKET, 512 - 60, false, xudc::kResiduePerTrb, len);
    ASSERT_EQ(d.verdict, Verdict::Head);
    EXPECT_EQ(d.actual, 60u);
    ring.apply(d);
    EXPECT_EQ(ring.room(), 2u) << "TRBs 252 and 253 only";
    d = ring.decide(8, xudc::COMP_SUCCESS, 0, false, xudc::kResiduePerTrb, len);
    EXPECT_EQ(d.verdict, Verdict::SecondEvent);
    ring.apply(d);
    EXPECT_EQ(ring.room(), 11u) << "past it: the TD's TRBs are free";
}

TEST(XudcCompletion, CancelFencesUntilTheEventsAreRead)
{
    Ring255 ring{};
    Lengths len;
    put(ring, len, 40, {2048, 2048});
    put(ring, len, 42, {2048});
    EXPECT_TRUE(ring.cancel());
    EXPECT_TRUE(ring.fenced());
    EXPECT_EQ(ring.room(), 0u) << "nothing goes on a fenced ring";
    EXPECT_EQ(ring.onRing(), 0u);
    for (const uint32_t index : {41u, 42u, 7u}) {
        const Decision d = ring.decide(index, xudc::COMP_SUCCESS, 0, false, xudc::kResiduePerTrb, len);
        EXPECT_EQ(d.verdict, Verdict::Stale) << index;
    }
    ring.liftFence();
    EXPECT_EQ(ring.room(), 254u);
    EXPECT_FALSE(ring.cancel()) << "nothing on the ring: no fence";
    Ring255 tailOnly{};
    put(tailOnly, len, 0, {512, 2048});
    tailOnly.apply(tailOnly.decide(0, xudc::COMP_SHORT_PACKET, 500, false, xudc::kResiduePerTrb, len));
    EXPECT_TRUE(tailOnly.cancel()) << "a tail may still raise its second event";
}

// A long usbmux-like session per controller behaviour: 16 KiB mux reads over 2 KiB clusters (9 TRBs or so, 64 of them
// queued: the ring stays full), writes of any size, the hardware racing the drain.
TEST(XudcSession, UsbmuxStreamNoLoss)
{
    for (const Behaviour &how : kBehaviours) {
        SCOPED_TRACE(describe(how));
        Session<255> s(11, how, 2 * xudc::kEventSegmentTrbs, 16384, 64);
        s.run(25000, 0.3, 24);
        checkSession(s, false);
        EXPECT_GT(s.outEp.tally.shortEarly, 10000u) << "TDs ended before their last TRB";
        if (how.second != Second::None)
            EXPECT_EQ(s.outEp.tally.second, s.outEp.tally.shortEarly) << "each one's second event absorbed";
        else
            EXPECT_EQ(s.outEp.tally.second, 0u);
        std::printf("XUDC session (%s): %llu messages, %llu bytes; OUT %zu reads (%llu ended early, %llu second "
                    "events, %llu serial), IN %zu writes; event ring at most %zu deep\n",
                    describe(how).c_str(), static_cast<unsigned long long>(s.messages),
                    static_cast<unsigned long long>(s.hostBytes), s.outEp.completed.size(),
                    static_cast<unsigned long long>(s.outEp.tally.shortEarly),
                    static_cast<unsigned long long>(s.outEp.tally.second),
                    static_cast<unsigned long long>(s.outEp.tally.serial), s.inEp.completed.size(), s.events.deepest);
    }
}

// A ring of 15 TRBs: it wraps every request or two, a read of 2 KiB clusters (8 TRBs) and its tail do not fit at once,
// so a read with nothing on the ring beside a tail takes the serial path.
TEST(XudcSession, SmallRingWraps)
{
    for (const Behaviour &how : kBehaviours) {
        SCOPED_TRACE(describe(how));
        Session<15> s(5, how, 2 * xudc::kEventSegmentTrbs, 16384, 8);
        s.run(20000, 0.5, 8);
        checkSession(s, false);
        EXPECT_GT(s.outEp.tally.serial, 0u) << "a tail blocked a read with nothing on the ring";
    }
}

// Aborts at random, from completions inside the drain too: stale events dropped, nothing lost or reused early.
TEST(XudcSession, CancelsMidStream)
{
    for (const Behaviour &how : kBehaviours) {
        SCOPED_TRACE(describe(how));
        Session<255> s(23, how, 2 * xudc::kEventSegmentTrbs, 16384, 32);
        s.cancelChance = 0.002;
        s.run(20000, 0.3, 24);
        checkSession(s, true);
        EXPECT_GT(s.cancels, 5u);
        EXPECT_GT(s.outEp.tally.stale + s.inEp.tally.stale, 0u) << "stale events met a fence";
    }
}

// An event ring that does overflow (64 events, the hardware running far ahead of the drain), on a controller
// raising one event per TD: the first event lost shows as a Lost verdict (the driver's panic), and every request
// completed before it is right: never a silent loss. (With second events a lost short-packet event could pass for its
// TD's own: the event ring is sized so it cannot overflow, kEventSegmentTrbs, and the controller's ring-full report
// panics.)
TEST(XudcSession, OverflowIsNeverSilent)
{
    Session<255> s(3, {false, Second::None}, 64, 16384, 64);
    s.run(40000, 0.0, 400);
    EXPECT_GT(s.events.dropped, 0u);
    EXPECT_GT(s.outEp.tally.lost + s.inEp.tally.lost, 0u) << "the drop was seen";
    EXPECT_EQ(s.outEp.tally.failed + s.inEp.tally.failed, 0u);
    checkCompletions(s.outEp, false, "OUT");
    checkCompletions(s.inEp, false, "IN");
}

namespace {
struct LegacyResult {
    uint64_t failed, wrong, overwrites, dropped;
};
LegacyResult legacy(Behaviour how, size_t eventCapacity, double concurrency, uint32_t burst)
{
    Session<255, LegacyMatching> s(11, how, eventCapacity, 16384, 64);
    s.run(25000, concurrency, burst);
    uint64_t wrong = 0;
    for (const Completion &c : s.outEp.completed) {
        const auto it = s.outEp.hw.delivered.find(c.request);
        if (c.outcome == Outcome::Done && c.actual != (it == s.outEp.hw.delivered.end() ? 0 : it->second)) ++wrong;
    }
    return {s.outEp.tally.failed, wrong, s.outEp.hw.wire.overwrites, s.events.dropped};
}
}  // namespace

// The same sessions with the legacy matching. Second events alone: no read lost (they find no request), but TRBs
// rewritten while the controller skips them. A 64-event ring under a burst: reads failed with 0 bytes, silently.
TEST(XudcSession, LegacyMatching)
{
    const LegacyResult second = legacy({false, Second::SuccessCode}, 2 * xudc::kEventSegmentTrbs, 0.3, 24);
    EXPECT_EQ(second.failed + second.wrong, 0u);
    EXPECT_GT(second.overwrites, 0u);
    const LegacyResult burst = legacy({false, Second::None}, 64, 0.0, 400);
    EXPECT_GT(burst.dropped, 0u);
    EXPECT_GT(burst.failed, 0u) << "overflow becomes silently failed reads";
    std::printf("XUDC legacy matching, second events %llu reads failed, %llu wrong, %llu TRBs rewritten before the "
                "controller passed them; with a 64-event ring under a burst %llu events dropped, %llu reads failed "
                "with 0 bytes, %llu wrong\n",
                static_cast<unsigned long long>(second.failed), static_cast<unsigned long long>(second.wrong),
                static_cast<unsigned long long>(second.overwrites), static_cast<unsigned long long>(burst.dropped),
                static_cast<unsigned long long>(burst.failed), static_cast<unsigned long long>(burst.wrong));
}
