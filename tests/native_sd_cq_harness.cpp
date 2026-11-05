// Host test of drivers/NativeIOSwitchBlock/IOSwitchCq.h: the SD driver's command-queue engine against a model card that
// enforces SD Physical Layer 6.00 4.19 (test_native_sd_cq.py runs this). Every rule the card has is an assert in the
// model, so the engine (and the driver-like loops below) can never send a command the card would refuse.
// r16-sdcq: the model has the card's cache too (9.10 4.17, Table 5-30 bytes 260/261): each write either on flash or held
// in the cache until a flush, which a power cycle loses; the driver-like loops assert that no completed write is ever
// lost, whenever the power goes. And the cards r7z could have been: one that takes the queue only with its cache on
// (4.19.1), one that shows the enable late (the Raspberry Pi kernel's "lazy" cards), one that keeps voluntary mode, one
// whose cache will not go off, one whose flush never ends. A cost model (kRegisterUs, kProgramUs, kCachedUs: assumed
// figures, not r7z's) prices each write from its command to its completion, against today's (cache off, no flush).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <vector>

#include "IOSwitchCq.h"

// The cost model: a CMD48/CMD49 (command, one 512-byte block, interrupt), a 4 KiB write programmed (the card busy until it
// is on flash), the same write into the card's cache, and a flush programming what the cache holds.
constexpr double kRegisterUs = 60, kProgramUs = 900, kCachedUs = 150;

struct Card {
    // What kind of card.
    unsigned depth = 32;
    unsigned lazyLeave = 0;      // read-backs that still show 03h after CMD49 00h
    bool stuck = false;          // never leaves CQ mode
    bool refuseEnable = false;   // CMD49 03h answered but the byte stays 00h
    bool requireCache = false;   // r16-sdcq: CMD49 03h taken only with the cache on (else the byte stays 00h)
    unsigned lazyEnable = 0;     // r16-sdcq: read-backs that still show 00h after CMD49 03h (the card in CQ mode already)
    bool voluntaryOnly = false;  // r16-sdcq: CMD49 03h enables the queue but bit 1 does not take (reads 01h)
    bool refuseCache = false;    // r16-sdcq: CMD49 01h of byte 260 answered, the cache stays off
    unsigned lazyCache = 0;      // r16-sdcq: read-backs that still show 00h after CMD49 01h of byte 260
    bool flushNeverDone = false;  // r16-sdcq: Flush Cache stays 01h (Linux's quirk for early SanDisk A2 cards)
    bool refuseDisable = false;   // r16-sdcq: CMD49 00h of byte 260 answered, the cache stays on
    long failAt = -1;             // this command (1-based count) fails on the bus and is not taken
    // State.
    bool cq = false;
    uint8_t byte262 = 0;  // the CQ control byte
    unsigned lazy = 0;
    unsigned enableLag = 0;  // read-backs of byte 262 still to show 00h
    // r16-sdcq: the cache: Cache Enable as the card holds it, read-backs still to show it off, Flush Cache; the writes on
    // flash and those only in the cache (a power cycle now would lose them); the card's busy time so far (the cost model).
    uint8_t byte260 = 0, byte261 = 0;
    unsigned cacheLag = 0;
    bool lostCommand = false;
    long flushes = 0, legacyWrites = 0;
    std::set<long> onFlash;
    std::vector<long> held;
    double busyUs = 0;
    bool half = false;  // a CMD44 waiting for its CMD45
    uint32_t halfTask = 0, halfBlocks = 0;
    struct Task {
        bool used;
        uint32_t blocks;
        uint64_t lba;
    } tasks[32] = {};
    unsigned assigned = 0, executedNext = 0;
    long commands = 0, aborts = 0, enables = 0, legacyCommands = 0, cqReads = 0;
    std::vector<uint32_t> trace;  // command indices in order

    bool queueEmpty() const { return executedNext == assigned; }
    bool fails()
    {
        ++commands;
        if (commands == failAt) {
            half = false;  // the card saw garbage: a half-assigned task is invalid now
            lostCommand = true;
            return true;
        }
        return false;
    }
    int command(uint32_t index, uint32_t argument, bool busy, uint32_t *r1)
    {
        trace.push_back(index);
        if (fails()) return 2;
        // Class 1 commands are illegal outside CQ mode (4.19.1: no response, Illegal Command in the next R1).
        assert(cq);
        // Anything after a valid CMD44 but CMD45 invalidates it (4.19.5): the engine always follows CMD44 with CMD45.
        assert(!half || index == 45);
        *r1 = 4u << 9;  // CQ Tran
        switch (index) {
        case 44: {
            assert(!busy);
            const uint32_t task = argument >> 16 & 0x1f, blocks = argument & 0xffff;
            assert(argument >> 30 == 1);        // read; bit 31 reserved 0
            assert(!(argument & 0x3fe00000u));  // extended address, priority, reserved: 0
            assert(blocks && task < depth && !tasks[task].used);
            // Sequential mode: the queue is empty before a group, tasks assigned at once from 0 in order (4.19.1.2).
            assert(task == assigned && executedNext == 0);
            half = true;
            halfTask = task;
            halfBlocks = blocks;
            return 0;
        }
        case 45:
            assert(!busy && half);
            half = false;
            tasks[halfTask] = {true, halfBlocks, argument};
            ++assigned;
            return 0;
        case 43:
            assert(busy && argument == kSdAbortQueue);  // abort the entire queue
            for (Task &t : tasks) t.used = false;
            assigned = executedNext = 0;
            ++aborts;
            return 0;
        default: assert(false); return 1;
        }
    }
    int control(uint32_t byte, bool write, uint8_t *value)
    {
        trace.push_back(write ? 49 : 48);
        busyUs += kRegisterUs;
        if (fails()) return 2;
        assert(!half);
        // r16-sdcq: the only bytes the driver may touch, and the only values it may write (ioswitchSdPerfWriteAddress).
        assert(byte == kSdPerfCacheControl || byte == kSdPerfFlushControl || byte == kSdPerfCqControl);
        if (write) assert(ioswitchSdPerfWriteAddress(IOSwitchSdExtFunction{true, 2, 0}, byte, *value));
        if (byte == kSdPerfCacheControl) {
            if (!write) {
                *value = cacheLag ? 0 : byte260;
                if (cacheLag) --cacheLag;
                return 0;
            }
            assert(!cq);  // the engine turns the cache on and off out of CQ mode only
            if (*value) {
                if (!refuseCache) {
                    byte260 = 1;
                    cacheLag = lazyCache;
                }
            } else {
                // An answered 00h supersedes an enable still to show (its busy lasts until it is done, 5.7.2.2): the
                // read-backs after it show the truth. What the cache holds stays held (nothing says disabling it
                // commits it): only writes whose flush failed, which never completed.
                cacheLag = 0;
                if (!refuseDisable) byte260 = 0;
            }
            return 0;
        }
        if (byte == kSdPerfFlushControl) {
            if (!write) {
                *value = byte261;
                return 0;
            }
            // Busy until the flush is done (4.17), so a read-back after it shows 00h. CMD49 is legal in CQ mode (Table
            // 4-74); the engine never flushes with tasks it assigned, but a CMD43 lost on the bus can leave some behind
            // before the stuck card's power cycle (reads only: the flush does not touch them).
            assert(queueEmpty() || lostCommand);
            ++flushes;
            if (flushNeverDone && byte260) {
                byte261 = 1;
                return 0;
            }
            busyUs += kProgramUs * double(held.size());  // busy until what it holds is programmed
            for (long id : held) onFlash.insert(id);
            held.clear();
            byte261 = 0;
            return 0;
        }
        if (!write) {
            *value = enableLag ? 0 : byte262;
            if (enableLag) --enableLag;
            if (lazy && !--lazy) {
                cq = false;
                byte262 = 0;
            }
            return 0;
        }
        // The CQ control byte, 00h or 03h (sequential mode).
        assert(*value == 0 || *value == (kSdCqEnable | kSdCqSequential));
        if (*value) {
            ++enables;
            if (refuseEnable || (requireCache && !byte260)) return 0;
            // Enabling needs an empty queue, nothing half-assigned (the engine only enables from legacy mode).
            assert(!cq || queueEmpty());
            cq = true;
            byte262 = voluntaryOnly ? kSdCqEnable : *value;
            enableLag = lazyEnable;
        } else if (stuck) {
            enableLag = 0;  // stays, and shows it
        } else if (lazyLeave) {
            enableLag = 0;  // the disable supersedes a late enable: it shows 03h until it is out
            lazy = lazyLeave;
        } else {
            cq = false;
            byte262 = 0;
            enableLag = 0;
            for (Task &t : tasks) t.used = false;
            assigned = executedNext = 0;
        }
        return 0;
    }
    // r16-sdcq: a 4 KiB CMD25 (legacy, out of CQ mode), its busy ended: on flash with the cache off; with it on, the card
    // may only hold it. Returns the write's id.
    long legacyWrite()
    {
        trace.push_back(25);
        assert(!cq);
        ++legacyCommands;
        const long id = ++legacyWrites;
        if (byte260) {
            held.push_back(id);
            busyUs += kCachedUs;
        } else {
            onFlash.insert(id);
            busyUs += kProgramUs;
        }
        return id;
    }
    // r16-sdcq: power off and initialize again (sleep, the driver's answer to a stuck card, or the power just going):
    // whatever the cache held is gone; every enable is back at 0.
    void powerCycle()
    {
        held.clear();
        cq = false;
        byte262 = byte260 = byte261 = 0;
        lazy = enableLag = cacheLag = 0;
        half = false;
        for (Task &t : tasks) t.used = false;
        assigned = executedNext = 0;
    }
    // CMD46 of a task (the driver's issue()+complete()): in Task ID order, an assigned task, in CQ mode.
    void execute(uint32_t argument)
    {
        trace.push_back(46);
        assert(cq && !half);
        const uint32_t task = argument >> 16 & 0x1f;
        assert(!(argument & ~(0x1fu << 16)));
        assert(task == executedNext && task < assigned && tasks[task].used);
        tasks[task].used = false;
        ++executedNext;
        ++cqReads;
        if (executedNext == assigned) assigned = executedNext = 0;  // the queue is empty again
    }
    // CMD17/18/24/25: illegal in CQ mode (Table 4-74).
    void legacy()
    {
        trace.push_back(18);
        assert(!cq);
        ++legacyCommands;
    }
};

struct Bus {
    Card *card;
    long settles = 0;
    int command(uint32_t index, uint32_t argument, bool busy, uint32_t *r1)
    {
        return card->command(index, argument, busy, r1);
    }
    int control(uint32_t byte, bool write, uint8_t *value) { return card->control(byte, write, value); }
    void settle() { ++settles; }
};
using Engine = IOSwitchCqEngine<Bus>;

static std::vector<IOSwitchCqTask> group(unsigned n, uint64_t base = 1000)
{
    std::vector<IOSwitchCqTask> t;
    for (unsigned i = 0; i < n; ++i) t.push_back({base + i * 977, 8u * (1 + i % 3)});
    return t;
}

// A whole group the way the driver runs one: in, assign, execute in order, out.
static bool runGroup(Engine &e, Bus &bus, Card &card, unsigned n)
{
    if (!e.enter(bus)) return false;
    const std::vector<IOSwitchCqTask> t = group(n);
    if (!e.submit(bus, t.data(), n)) return false;
    while (e.pending()) {
        card.execute(e.argument());
        e.executed();
    }
    return e.empty();
}

static void basics()
{
    // The order of commands of one group of 3, then leaving.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(e.state == Engine::kIdle && e.depth == 32);
        assert(runGroup(e, bus, card, 3) && e.state == Engine::kOn && card.cq);
        assert(e.leave(bus) && e.state == Engine::kIdle && !card.cq);
        const std::vector<uint32_t> expected = {49, 48, 44, 45, 44, 45, 44, 45, 46, 46, 46, 49, 48};
        assert(card.trace == expected);
        assert(e.groups == 1 && e.tasks == 3 && e.enters == 1 && e.leaves == 1 && !e.why);
        // Legacy commands are fine again; a second group re-enters.
        card.legacy();
        assert(runGroup(e, bus, card, 2) && e.enters == 2);
        // Two groups back to back stay in CQ mode (no CMD49 in between).
        const size_t before = card.trace.size();
        assert(runGroup(e, bus, card, 4));
        assert(card.trace[before] == 44 && e.enters == 2);
        assert(e.leave(bus));
    }
    // A card without a queue (depth field 0 or 1) is never used.
    {
        Engine e;
        e.start(0, false);
        assert(e.state == Engine::kOff);
        Card card;
        Bus bus{&card};
        assert(!e.enter(bus) && card.trace.empty());
        e.start(1, false);
        assert(e.state == Engine::kOff);
    }
    // The depth bounds a group; a group may use all of it.
    {
        Card card;
        card.depth = 4;
        Bus bus{&card};
        Engine e;
        e.start(4, false);
        assert(runGroup(e, bus, card, 4));
        assert(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(5);
        assert(!e.submit(bus, t.data(), 5) && e.state == Engine::kOff && !card.cq && e.why);
    }
    // Tasks must be assignable: 1..65535 blocks, a 32-bit block address.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(8, false);
        assert(e.enter(bus));
        IOSwitchCqTask bad[1] = {{1ull << 32, 8}};
        assert(!e.submit(bus, bad, 1) && e.state == Engine::kOff && !card.cq);
    }
    // Encodings.
    assert(ioswitchSdCmd44(true, 0, 8) == 0x40000008u && ioswitchSdCmd44(true, 31, 0xffff) == 0x401fffffu);
    assert(ioswitchSdCmd44(false, 3, 1) == 0x00030001u);
    assert(ioswitchSdTaskArgument(5) == 0x50000u && ioswitchSdCmd43(kSdAbortQueue, 0) == 1 &&
           ioswitchSdCmd43(kSdAbortTask, 7) == 0x70002u);
}

static void failures()
{
    // CMD44 of the third task fails on the bus: the queue is aborted, CQ left, off for the boot, never entered again.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        card.failAt = 2 + 2 * 2 + 1;  // 49, 48, then 44/45 twice, then the third CMD44
        assert(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(4);
        assert(!e.submit(bus, t.data(), 4));
        assert(e.state == Engine::kOff && !card.cq && card.aborts == 1 && e.why);
        assert(!e.enter(bus) && card.enables == 1);
        card.legacy();
    }
    // CMD45 fails: the half-assigned task is covered by the abort too.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        card.failAt = 4;  // 49, 48, 44, then CMD45
        assert(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(2);
        assert(!e.submit(bus, t.data(), 2) && e.state == Engine::kOff && !card.cq && card.aborts == 1);
    }
    // A task's data fails (the driver calls fail): abort, leave, off.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(3);
        assert(e.submit(bus, t.data(), 3));
        card.execute(e.argument());
        e.executed();
        assert(e.fail(bus, "CMD46 failed") && e.state == Engine::kOff && !card.cq && card.aborts == 1);
        assert(!e.pending() && !e.enter(bus));
    }
    // A failure with nothing queued (a proof that differed after leaving): nothing sent, off.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(runGroup(e, bus, card, 2) && e.leave(bus));
        const size_t before = card.trace.size();
        assert(e.fail(bus, "a proof differed") && card.trace.size() == before && e.state == Engine::kOff);
        assert(!strcmp(e.why, "a proof differed"));
    }
    // The card does not take the enable: the engine makes sure it is out (writes 00h, reads 00h) and stays off.
    {
        Card card;
        card.refuseEnable = true;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(!e.enter(bus) && e.state == Engine::kOff && !card.cq && e.why);
        assert(card.trace.back() == 48);
    }
    // The enable's own CMD49 fails on the bus.
    {
        Card card;
        card.failAt = 1;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(!e.enter(bus) && e.state == Engine::kOff && !card.cq);
    }
    // A card slow to leave: read back until it is out, a settle between read-backs.
    {
        Card card;
        card.lazyLeave = 3;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(runGroup(e, bus, card, 2));
        assert(e.leave(bus) && !card.cq && bus.settles == 3 && e.state == Engine::kIdle);
        card.legacy();
    }
    // A card that never leaves: false after kLeaveTries read-backs; off (the driver power-cycles it).
    {
        Card card;
        card.stuck = true;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(runGroup(e, bus, card, 2));
        assert(!e.leave(bus) && e.state == Engine::kOff && e.stuck && card.cq &&
               bus.settles == Engine::kReadBacks - 1);
        // Power-cycled by the driver: the card is out, the engine stays off.
        card.cq = false;
        card.byte262 = 0;
        e.powerCycled();
        assert(e.state == Engine::kOff && !e.enter(bus));
    }
    // System sleep between groups: CQ mode does not survive the power cycle; the engine knows.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(runGroup(e, bus, card, 2) && e.leave(bus));
        e.powerCycled();
        assert(e.state == Engine::kIdle && runGroup(e, bus, card, 2) && e.leave(bus));
    }
    // Leaving with tasks still queued is a bug in the caller: it aborts them first.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(3);
        assert(e.submit(bus, t.data(), 3));
        assert(e.leave(bus) && e.state == Engine::kOff && !card.cq && card.aborts == 1);
    }
}

// r16-sdcq: what the driver's write path does with a write (endBatch): out of CQ mode (a legacy command), the CMD25,
// then commitWrite. Completed (in `completed`) only when that says so; its cost from the CMD25 to the completion.
struct WriteResult {
    bool completed;
    double us;
    long commands;
};
static WriteResult driverWrite(Engine &e, Bus &bus, Card &card, std::set<long> &completed)
{
    const double before = card.busyUs;
    const long commandsBefore = card.commands + card.legacyCommands;
    const long id = card.legacyWrite();
    const bool ok = e.commitWrite(bus);
    if (ok) completed.insert(id);
    return {ok, card.busyUs - before, card.commands + card.legacyCommands - commandsBefore};
}
// No completed write is lost, whenever the power goes (called at every power cycle).
static void assertKept(const Card &card, const std::set<long> &completed)
{
    for (long id : completed) assert(card.onFlash.count(id));
}

// r16-sdcq: what r7z's card could have been, and the cache's contract with the driver.
static void bringUp()
{
    // The card takes the queue only with its cache on (4.19.1): without it the byte reads back 00h and the engine says
    // so; a session with it turns it on before 03h and off after 00h, so the write after it finds it off.
    {
        Card card;
        card.requireCache = true;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(!e.enter(bus) && !card.cq && e.cqRead == 0 && e.cqReads == Engine::kReadBacks && e.why);
        assert(bus.settles == Engine::kReadBacks - 1);  // the enable's read-backs (leaving reads 00h at once)
    }
    {
        Card card;
        card.requireCache = true;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        assert(runGroup(e, bus, card, 4) && e.cacheOn && card.byte260 == 1);
        assert(e.leave(bus) && !card.cq && !e.cacheOn && card.byte260 == 0 && e.useCache);
        const std::vector<uint32_t> expected = {49, 48, 49, 48, 44, 45, 44, 45, 44, 45, 44, 45, 46, 46, 46, 46, 49, 48, 49, 48};
        assert(card.trace == expected);
        // The write: today's command and cost exactly (no flush), on flash when it completes.
        std::set<long> completed;
        const WriteResult w = driverWrite(e, bus, card, completed);
        assert(w.completed && w.commands == 1 && w.us == kProgramUs && card.held.empty());
        // The next session turns the cache on again; a power cycle in the middle of it loses nothing written.
        assert(runGroup(e, bus, card, 2) && card.byte260 == 1);
        card.powerCycle();
        e.powerCycled();
        assertKept(card, completed);
        assert(!e.cacheOn && e.state == Engine::kIdle);
        assert(runGroup(e, bus, card, 2) && e.leave(bus) && card.byte260 == 0);
        // The queue off for the boot: the cache stays off for good.
        assert(e.fail(bus, "a proof differed") && !e.cacheOn && !e.useCache && card.byte260 == 0);
    }
    // A lazy card: the enable shows after a few read-backs (settled between), within the engine's window.
    {
        Card card;
        card.lazyEnable = 3;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        assert(e.enter(bus) && e.cqReads == 4 && bus.settles == 3 && e.cqRead == 3);
        assert(e.leave(bus) && !card.cq);
    }
    // One lazier than the window: off, and out of CQ mode (it was in already).
    {
        Card card;
        card.lazyEnable = Engine::kReadBacks;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        assert(!e.enter(bus) && !card.cq && e.state == Engine::kOff && e.cqRead == 0 && !e.cacheOn && !card.byte260);
        card.legacy();
    }
    // A card that keeps voluntary mode: the read-back says which bit did not take; out of CQ mode, the cache off.
    {
        Card card;
        card.voluntaryOnly = true;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        assert(!e.enter(bus) && !card.cq && e.cqRead == kSdCqEnable && !e.cacheOn && card.byte260 == 0);
        assert(e.cacheRead == kSdCacheEnable);  // the cache did come on first
        assert(strstr(ioswitchSdCqControlMeaning(e.cqRead), "voluntary"));
        assert(strstr(ioswitchSdCqControlMeaning(0), "disabled") && strstr(ioswitchSdCqControlMeaning(3), "sequential"));
        card.legacy();
    }
    // A cache that will not come on: tried once (read back 00h, 00h written and read back), never again this boot; the
    // queue still gets its try.
    {
        Card card;
        card.refuseCache = true;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        assert(runGroup(e, bus, card, 2) && e.leave(bus) && !e.cacheOn && !e.useCache && e.cacheRead == 0);
        const size_t before = card.trace.size();
        assert(runGroup(e, bus, card, 2) && e.leave(bus));
        assert(card.trace[before] == 49 && card.trace[before + 1] == 48 && card.trace[before + 2] == 44);  // 262 only
    }
    // A cache lazier than the window: 00h cancels it; the engine knows it is off.
    {
        Card card;
        card.lazyCache = Engine::kReadBacks + 1;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        assert(runGroup(e, bus, card, 2) && !e.cacheOn && card.byte260 == 0 && !e.useCache);
    }
    // A cache that will not go off: every write is flushed before it completes (write-through all the same), the cache
    // left alone from then on; with a flush that never ends the write fails, nothing panics, and a power cycle loses
    // only that write.
    {
        Card card;
        card.refuseDisable = true;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        assert(runGroup(e, bus, card, 2) && e.leave(bus) && e.cacheOn && card.byte260 == 1 && !e.useCache);
        std::set<long> completed;
        const WriteResult w = driverWrite(e, bus, card, completed);
        assert(w.completed && w.commands == 3 && card.held.empty());  // CMD25, the flush's CMD49 and CMD48
        assert(w.us == kCachedUs + 2 * kRegisterUs + kProgramUs);
        card.powerCycle();
        e.powerCycled();
        assertKept(card, completed);
        // Once it would not go off, no session turns it on again: after the power cycle it stays off, writes as today.
        assert(runGroup(e, bus, card, 2) && e.leave(bus) && !e.cacheOn && card.byte260 == 0);
        assert(driverWrite(e, bus, card, completed).us == kProgramUs);
    }
    {
        Card card;
        card.refuseDisable = true;
        card.flushNeverDone = true;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        std::set<long> completed;
        assert(driverWrite(e, bus, card, completed).completed);
        assert(runGroup(e, bus, card, 2) && e.leave(bus) && e.cacheOn);
        const WriteResult lost = driverWrite(e, bus, card, completed);
        assert(!lost.completed && card.held.size() == 1);
        card.powerCycle();
        e.powerCycled();
        assertKept(card, completed);
    }
    // A card stuck in CQ mode with its cache on: nothing to flush (no write ran in the session), the power cycle loses
    // nothing completed.
    {
        Card card;
        card.stuck = true;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        std::set<long> completed;
        assert(driverWrite(e, bus, card, completed).completed);
        assert(runGroup(e, bus, card, 2) && card.byte260 == 1);
        assert(!e.fail(bus, "data error") && e.stuck && card.cq && e.cacheOn && !e.flush(bus));
        card.powerCycle();
        e.powerCycled();
        assertKept(card, completed);
        assert(!e.cacheOn && e.state == Engine::kOff);
    }
}

// The driver's loop over random work with random bus failures and card behaviours: whatever happens, the card's rules
// hold (the model asserts them), a legacy command only ever finds the card out of CQ mode, the engine never says the card
// is out of CQ mode while it is in, and after a failure the engine stays off. When the engine cannot get the card out
// (false from leave/fail) the driver power-cycles it, as here. r16-sdcq: the power also goes at random points (sleep, a
// stuck card's reset, or just lost: between a write's command and its completion too), and no completed write is ever
// lost; a flush that fails fails its write and nothing else; each write's cost is compared with today's.
static void randomWork()
{
    std::mt19937_64 random(0x5dc9);
    unsigned powerCycles = 0, groups = 0, failures = 0, sleeps = 0, losses = 0;
    unsigned long writes = 0, sameAsToday = 0, flushedWrites = 0, failedWrites = 0;
    double todayUs = 0, nowUs = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        Card card;
        card.depth = 2 + unsigned(random() % 31);
        card.lazyLeave = random() % 8 == 0 ? unsigned(random() % 12) : 0;
        card.stuck = random() % 50 == 0;
        card.refuseEnable = random() % 50 == 0;
        card.requireCache = random() % 4 == 0;
        card.lazyEnable = random() % 6 == 0 ? unsigned(random() % 12) : 0;
        card.voluntaryOnly = random() % 40 == 0;
        card.refuseCache = random() % 30 == 0;
        card.lazyCache = random() % 8 == 0 ? unsigned(random() % 12) : 0;
        card.refuseDisable = random() % 20 == 0;
        card.flushNeverDone = random() % 30 == 0;
        card.failAt = random() % 3 == 0 ? long(1 + random() % 60) : -1;
        Bus bus{&card};
        Engine e;
        e.start(uint8_t(card.depth), random() % 4 != 0);  // a card with a cache, most of the time
        std::set<long> completed;
        const auto powerCycle = [&]() {
            card.powerCycle();
            e.powerCycled();
            assertKept(card, completed);
            ++powerCycles;
        };
        // The driver's answer to "the card stays in CQ mode" (cqLeave -> cqStop): fail() once more, then the power
        // cycle (resetCard), which needs no flush first.
        const auto out = [&](bool isOut) {
            if (isOut) {
                assert(!card.cq && !e.stuck);
                return;
            }
            assert(e.state == Engine::kOff && e.stuck);
            (void)e.fail(bus, "the card stays in CQ mode");
            powerCycle();
        };
        bool gone = false;
        for (unsigned step = 0; step < 40 && !gone; ++step) {
            const unsigned what = unsigned(random() % 8);
            if (what == 0 || what == 1) {
                // A write (legacy: out of CQ mode first), sometimes with the power going before it completes.
                out(e.leave(bus));
                const bool cacheAtWrite = e.cacheOn;
                if (random() % 25 == 0) {
                    (void)card.legacyWrite();
                    powerCycle();  // never completed: may be lost
                    ++losses;
                    gone = true;
                    break;
                }
                const WriteResult w = driverWrite(e, bus, card, completed);
                ++writes;
                todayUs += kProgramUs;  // today: CMD25 into a card with its cache off, completed at its busy's end
                if (!w.completed) {
                    assert(cacheAtWrite);  // only a flush can fail a write
                    ++failedWrites;
                    continue;
                }
                nowUs += w.us;
                if (!cacheAtWrite) {
                    // The cache off: today's write exactly, one command, no flush.
                    assert(w.commands == 1 && w.us == kProgramUs);
                    ++sameAsToday;
                } else {
                    ++flushedWrites;
                }
            } else if (what == 2) {
                out(e.leave(bus));  // the queue ran dry
            } else if (what == 3) {
                // System sleep and wake: out of CQ mode, power-cycled (no flush needed), the queue usable again.
                out(e.leave(bus));
                powerCycle();
                ++sleeps;
            } else if (what == 4 && random() % 4 == 0) {
                // The power just goes, whatever the card is doing (in a CQ session, between commands).
                powerCycle();
                ++losses;
                gone = true;
                break;
            } else {
                const unsigned n = 1 + unsigned(random() % card.depth);
                if (!e.enter(bus)) {
                    if (e.stuck) out(false);  // enter failed and could not get the card out
                    card.legacy();
                    continue;
                }
                const std::vector<IOSwitchCqTask> t = group(n, random() % 100000);
                if (!e.submit(bus, t.data(), n)) {
                    if (e.stuck) out(false);
                    ++failures;
                    card.legacy();
                    continue;
                }
                ++groups;
                if (random() % 30 == 0) {
                    // The power goes in the middle of a group: reads only, nothing written can be lost.
                    powerCycle();
                    ++losses;
                    gone = true;
                    break;
                }
                while (e.pending()) {
                    if (random() % 40 == 0) {
                        out(e.fail(bus, "data error"));
                        ++failures;
                        break;
                    }
                    card.execute(e.argument());
                    e.executed();
                }
            }
            if (e.state == Engine::kOff) assert(e.why || card.depth < 2);
            // Whenever the engine is not in CQ mode and has not said the card is stuck, the card is out of CQ mode.
            assert(e.state == Engine::kOn || !card.cq);
            // r16-sdcq: the engine never thinks the cache is off while the card has it on (or will show it later); out
            // of CQ mode it is off unless it would not go off; nothing completed is only in the cache.
            assert(e.cacheOn || !card.byte260);
            assert(e.state == Engine::kOn || e.stuck || !card.byte260 || !e.useCache || card.cacheLag);
            for (long id : card.held) assert(!completed.count(id));
        }
        if (!gone) powerCycle();  // the end of every round: the power goes
    }
    // Every path was exercised.
    assert(powerCycles > 20000 && groups > 1000 && failures > 100 && sleeps > 1000 && losses > 1000);
    assert(flushedWrites > 100 && failedWrites > 0 && sameAsToday > 10000);
    printf("writes %lu: %lu as today (cache off, %.0f us each), %lu flushed (cache would not go off), %lu failed (flush "
           "failed); mean %.1f us now against %.1f us today (model: register %.0f us, program %.0f us, cached %.0f "
           "us)\n",
           writes, sameAsToday, kProgramUs, flushedWrites, failedWrites,
           nowUs / double(sameAsToday + flushedWrites), todayUs / double(writes), kRegisterUs, kProgramUs, kCachedUs);
}

int main()
{
    basics();
    failures();
    bringUp();
    randomWork();
    puts("PASS: sd command queue");
    return 0;
}
