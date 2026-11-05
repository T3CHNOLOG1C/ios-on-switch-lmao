// Host test of drivers/NativeIOSwitchBlock/IOSwitchCq.h: the SD driver's command-queue engine against a model card that
// enforces SD Physical Layer 6.00 4.19 (test_native_sd_cq.py runs this). Every rule the card has is an assert in the
// model, so the engine (and the driver-like loops below) can never send a command the card would refuse.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "IOSwitchCq.h"

struct Card {
    // What kind of card.
    unsigned depth = 32;
    unsigned lazyLeave = 0;     // read-backs that still show 03h after CMD49 00h
    bool stuck = false;         // never leaves CQ mode
    bool refuseEnable = false;  // CMD49 03h answered but the byte stays 00h
    long failAt = -1;           // this command (1-based count) fails on the bus and is not taken
    // State.
    bool cq = false;
    uint8_t byte262 = 0;  // the CQ control byte
    unsigned lazy = 0;
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
    int control(bool write, uint8_t *value)
    {
        trace.push_back(write ? 49 : 48);
        if (fails()) return 2;
        assert(!half);
        if (!write) {
            *value = byte262;
            if (lazy && !--lazy) {
                cq = false;
                byte262 = 0;
            }
            return 0;
        }
        // The only byte the driver may write: the CQ control byte, 00h or 03h (sequential mode).
        assert(*value == 0 || *value == (kSdCqEnable | kSdCqSequential));
        if (*value) {
            ++enables;
            if (refuseEnable) return 0;
            // Enabling needs an empty queue, nothing half-assigned (the engine only enables from legacy mode).
            assert(!cq || queueEmpty());
            cq = true;
            byte262 = *value;
        } else if (stuck) {
            // stays
        } else if (lazyLeave) {
            lazy = lazyLeave;
        } else {
            cq = false;
            byte262 = 0;
            for (Task &t : tasks) t.used = false;
            assigned = executedNext = 0;
        }
        return 0;
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
    int control(bool write, uint8_t *value) { return card->control(write, value); }
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
        e.start(32);
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
        e.start(0);
        assert(e.state == Engine::kOff);
        Card card;
        Bus bus{&card};
        assert(!e.enter(bus) && card.trace.empty());
        e.start(1);
        assert(e.state == Engine::kOff);
    }
    // The depth bounds a group; a group may use all of it.
    {
        Card card;
        card.depth = 4;
        Bus bus{&card};
        Engine e;
        e.start(4);
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
        e.start(8);
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
        e.start(32);
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
        e.start(32);
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
        e.start(32);
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
        e.start(32);
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
        e.start(32);
        assert(!e.enter(bus) && e.state == Engine::kOff && !card.cq && e.why);
        assert(card.trace.back() == 48);
    }
    // The enable's own CMD49 fails on the bus.
    {
        Card card;
        card.failAt = 1;
        Bus bus{&card};
        Engine e;
        e.start(32);
        assert(!e.enter(bus) && e.state == Engine::kOff && !card.cq);
    }
    // A card slow to leave: read back until it is out, a settle between read-backs.
    {
        Card card;
        card.lazyLeave = 3;
        Bus bus{&card};
        Engine e;
        e.start(32);
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
        e.start(32);
        assert(runGroup(e, bus, card, 2));
        assert(!e.leave(bus) && e.state == Engine::kOff && e.stuck && card.cq &&
               bus.settles == Engine::kLeaveTries - 1);
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
        e.start(32);
        assert(runGroup(e, bus, card, 2) && e.leave(bus));
        e.powerCycled();
        assert(e.state == Engine::kIdle && runGroup(e, bus, card, 2) && e.leave(bus));
    }
    // Leaving with tasks still queued is a bug in the caller: it aborts them first.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32);
        assert(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(3);
        assert(e.submit(bus, t.data(), 3));
        assert(e.leave(bus) && e.state == Engine::kOff && !card.cq && card.aborts == 1);
    }
}

// The driver's loop over random work with random bus failures and card behaviours: whatever happens, the card's rules
// hold (the model asserts them), a legacy command only ever finds the card out of CQ mode, the engine never says the card
// is out of CQ mode while it is in, and after a failure the engine stays off. When the engine cannot get the card out
// (false from leave/fail) the driver power-cycles it, as here.
static void randomWork()
{
    std::mt19937_64 random(0x5dc9);
    unsigned powerCycles = 0, groups = 0, failures = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        Card card;
        card.depth = 2 + unsigned(random() % 31);
        card.lazyLeave = random() % 8 == 0 ? unsigned(random() % 12) : 0;
        card.stuck = random() % 50 == 0;
        card.refuseEnable = random() % 50 == 0;
        card.failAt = random() % 3 == 0 ? long(1 + random() % 60) : -1;
        Bus bus{&card};
        Engine e;
        e.start(uint8_t(card.depth));
        // The driver's answer to "the card stays in CQ mode": power off, initialize again.
        const auto out = [&](bool isOut) {
            if (isOut) {
                assert(!card.cq && !e.stuck);
                return;
            }
            assert(e.state == Engine::kOff && e.stuck);
            card.cq = false;
            card.byte262 = 0;
            card.lazy = 0;
            card.half = false;
            for (Card::Task &t : card.tasks) t.used = false;
            card.assigned = card.executedNext = 0;
            e.powerCycled();
            ++powerCycles;
        };
        for (unsigned step = 0; step < 40; ++step) {
            const unsigned what = unsigned(random() % 4);
            if (what == 0) {
                out(e.leave(bus));  // a write or a lone read: legacy, out of CQ mode first
                card.legacy();
            } else if (what == 1) {
                out(e.leave(bus));  // the queue ran dry
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
        }
    }
    assert(powerCycles > 0 && groups > 1000 && failures > 100);  // every path was exercised
}

int main()
{
    basics();
    failures();
    randomWork();
    puts("PASS: sd command queue");
    return 0;
}
