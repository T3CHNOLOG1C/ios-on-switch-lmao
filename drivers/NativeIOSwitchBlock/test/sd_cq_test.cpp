// Host test of drivers/NativeIOSwitchBlock/IOSwitchCq.h: the SD driver's command-queue engine against a model card that
// enforces SD Physical Layer 6.00 4.19. Every rule the card has is a REQUIRE in the model, so the engine (and the
// driver-like loops below) can never send a command the card would refuse. What the driver does with the engine is
// guarded by sd_cq_source_test.cpp.
// r16-sdcq: the model has the card's cache too (9.10 4.17, Table 5-30 bytes 260/261): each write either on flash or held
// in the cache until a flush, which a power cycle loses; the driver-like loops assert that no completed write is ever
// lost, whenever the power goes. And the cards r7z could have been: one that takes the queue only with its cache on
// (4.19.1), one that shows the enable late (the Raspberry Pi kernel's "lazy" cards), one that keeps voluntary mode, one
// whose cache will not go off, one whose flush never ends. A cost model (kRegisterUs, kProgramUs, kCachedUs: assumed
// figures, not r7z's) prices each write from its command to its completion, against today's (cache off, no flush).
// r18-sdcq: the model answers as the spec says a card does, not with a REQUIRE, where the spec says what a card does:
// an illegal command (a class 1 command out of CQ mode, a CMD44 whose Task ID is taken or beyond the depth, a CMD45 not
// right after a valid CMD44, a CMD46 out of order: 4.19, 4.19.5, 4.19.7) gets no response and ILLEGAL_COMMAND in the next
// response, a command garbled on the bus gets none and COM_CRC_ERROR in the next (4.6.1, Table 4-42 clear condition B),
// any command after a CMD44 but CMD45 invalidates it (CMD13 too). Then the cards r8c could have been (its first data-path
// group's CMD44 failed and the queue went off for the boot): one whose queue opens a few commands after 03h reads back,
// one that keeps a stale Task ID 0 across a session's end, one that refuses one CMD44 or answers it with an error bit, one
// whose real depth is below its byte 6, one whose task data fails. The group runner (IOSwitchCq.h run) runs against a
// driver-like host that counts every chain's completions: each exactly once, only with data read for it (proven while
// proofs are due), whatever fails.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <vector>

#include "../IOSwitchCq.h"
#include "bazel/testing/require.h"

// The cost model: a CMD48/CMD49 (command, one 512-byte block, interrupt), a 4 KiB write programmed (the card busy until it
// is on flash), the same write into the card's cache, and a flush programming what the cache holds.
constexpr double kRegisterUs = 60, kProgramUs = 900, kCachedUs = 150;
// r18-sdcq: R1 bits the model sets (Table 4-42).
constexpr uint32_t kComCrc = 1u << 23, kIllegal = 1u << 22, kCcError = 1u << 20, kCqTran = 4u << 9;

struct Card {
    // What kind of card.
    unsigned depth = 32;
    unsigned lazyLeave = 0;     // read-backs that still show 03h after CMD49 00h
    bool stuck = false;         // never leaves CQ mode
    bool refuseEnable = false;  // CMD49 03h answered but the byte stays 00h
    bool requireCache = false;  // r16-sdcq: CMD49 03h taken only with the cache on (else the byte stays 00h)
    unsigned lazyEnable = 0;  // r16-sdcq: read-backs that still show 00h after CMD49 03h (the card in CQ mode already)
    bool voluntaryOnly = false;   // r16-sdcq: CMD49 03h enables the queue but bit 1 does not take (reads 01h)
    bool refuseCache = false;     // r16-sdcq: CMD49 01h of byte 260 answered, the cache stays off
    unsigned lazyCache = 0;       // r16-sdcq: read-backs that still show 00h after CMD49 01h of byte 260
    bool flushNeverDone = false;  // r16-sdcq: Flush Cache stays 01h (Linux's quirk for early SanDisk A2 cards)
    bool refuseDisable = false;   // r16-sdcq: CMD49 00h of byte 260 answered, the cache stays on
    long failAt = -1;             // this command (1-based count) is garbled on the bus: no response, COM_CRC_ERROR next
    // r18-sdcq: what r8c's card could have done.
    unsigned openLag = 0;    // after 03h is taken, this many class 1 commands (or settles) find the queue not open
    bool staleTask = false;  // leaving CQ mode keeps a Task ID 0 the next session's first CMD44 then finds taken
    long refuse44At = -1;    // the n-th CMD44 (1-based) gets no response
    long error44At = -1;     // the n-th CMD44 is answered with CC_ERROR (an X-type bit of an earlier command)
    long refuse44From = -1;  // every CMD44 from the n-th on gets no response (a card that never takes one again)
    unsigned rejectId = 32;  // Task IDs from this one on get no response (its real depth below byte 6's)
    long fail46At = -1;      // the n-th CMD46's data fails (a data CRC error: the task stays queued)
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
    uint32_t pendingB = 0;  // r18-sdcq: B-type bits for the next answered response
    unsigned openLeft = 0;
    long commands = 0, aborts = 0, enables = 0, legacyCommands = 0, cqReads = 0;
    long illegal = 0, cmd44s = 0, cmd46s = 0, statuses = 0;  // r18-sdcq
    std::vector<uint32_t> trace;                             // command indices in order

    bool queueEmpty() const { return executedNext == assigned; }
    bool fails()
    {
        ++commands;
        if (commands == failAt) {
            half = false;  // the card saw garbage: a half-assigned task is invalid now
            lostCommand = true;
            pendingB |= kComCrc;
            return true;
        }
        return false;
    }
    // r18-sdcq: an answer, carrying (and clearing) what the previous command left; no answer, and why in the next.
    uint32_t answer(uint32_t bits = 0)
    {
        const uint32_t r1 = pendingB | bits | kCqTran;
        pendingB = 0;
        return r1;
    }
    int noAnswer()
    {
        pendingB |= kIllegal;
        ++illegal;
        return 2;
    }
    void clearTasks()
    {
        for (Task &t : tasks) t.used = false;
        assigned = executedNext = 0;
        half = false;
    }
    int command(uint32_t index, uint32_t argument, bool busy, uint32_t *r1)
    {
        trace.push_back(index);
        *r1 = 0;
        if (fails()) return 2;
        // Class 1 commands are illegal outside CQ mode (4.19: no response, Illegal Command in the next R1), and before a
        // lazy card's queue is open.
        if (!cq || openLeft) {
            if (openLeft) --openLeft;
            half = false;
            return noAnswer();
        }
        // Anything after a valid CMD44 but CMD45 invalidates it (4.19.5): the engine only ever sends CMD43 there (its
        // recovery, after CMD13).
        if (half && index != 45) {
            REQUIRE(index == 43);
            half = false;
        }
        switch (index) {
        case 44: {
            REQUIRE(!busy);
            const uint32_t task = argument >> 16 & 0x1f, blocks = argument & 0xffff;
            REQUIRE(argument >> 30 == 1);        // read; bit 31 reserved 0
            REQUIRE(!(argument & 0x3fe00000u));  // extended address, priority, reserved: 0
            ++cmd44s;
            if (cmd44s == refuse44At || (refuse44From > 0 && cmd44s >= refuse44From)) return noAnswer();
            // Task ID already exists, beyond the depth, no blocks: no response (4.19.5).
            if (!blocks || task >= depth || task >= rejectId || tasks[task].used) return noAnswer();
            // Sequential mode: the queue is empty before a group, tasks assigned at once from 0 in order (4.19.1.2).
            REQUIRE(task == assigned && executedNext == 0);
            half = true;
            halfTask = task;
            halfBlocks = blocks;
            *r1 = answer(cmd44s == error44At ? kCcError : 0);
            return 0;
        }
        case 45:
            REQUIRE(!busy);
            if (!half) return noAnswer();
            half = false;
            tasks[halfTask] = {true, halfBlocks, argument};
            ++assigned;
            *r1 = answer();
            return 0;
        case 43:
            REQUIRE(busy &&
                    argument == kSdAbortQueue);  // abort the entire queue (no error on an empty one: 9.10 4.19.8)
            clearTasks();
            ++aborts;
            *r1 = answer();
            return 0;
        default: REQUIRE(false); return 1;
        }
    }
    // r18-sdcq: CMD13, legal in every state (Table 4-74). Card status, or in CQ mode with bit 15 the Task Status Register
    // (4.10.3: a ready bit per task). It invalidates a half-assigned CMD44 (4.19.5: "including CMD13").
    int status(bool taskStatus, uint32_t *r1)
    {
        trace.push_back(13);
        ++statuses;
        *r1 = 0;
        if (fails()) return 2;
        half = false;
        if (taskStatus && cq) {
            uint32_t ready = 0;
            for (unsigned i = 0; i < 32; ++i)
                if (tasks[i].used) ready |= 1u << i;
            *r1 = ready;
            return 0;
        }
        *r1 = answer();
        return 0;
    }
    void settled()
    {
        if (openLeft) --openLeft;
    }
    int control(uint32_t byte, bool write, uint8_t *value, uint32_t *r1)
    {
        trace.push_back(write ? 49 : 48);
        busyUs += kRegisterUs;
        *r1 = 0;
        if (fails()) return 2;
        REQUIRE(!half);
        // The driver's control(): an R1 error bit is an error (30), the command executed all the same.
        const uint32_t carried = pendingB;
        *r1 = answer();
        const int result = carried ? 30 : 0;
        // r16-sdcq: the only bytes the driver may touch, and the only values it may write (ioswitchSdPerfWriteAddress).
        REQUIRE(byte == kSdPerfCacheControl || byte == kSdPerfFlushControl || byte == kSdPerfCqControl);
        if (write) REQUIRE(ioswitchSdPerfWriteAddress(IOSwitchSdExtFunction{true, 2, 0}, byte, *value));
        if (byte == kSdPerfCacheControl) {
            if (!write) {
                if (!result) *value = cacheLag ? 0 : byte260;
                if (cacheLag) --cacheLag;
                return result;
            }
            REQUIRE(!cq);  // the engine turns the cache on and off out of CQ mode only
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
            return result;
        }
        if (byte == kSdPerfFlushControl) {
            if (!write) {
                if (!result) *value = byte261;
                return result;
            }
            // Busy until the flush is done (4.17), so a read-back after it shows 00h. CMD49 is legal in CQ mode (Table
            // 4-74); the engine never flushes with tasks it assigned, but a CMD43 lost on the bus can leave some behind
            // before the stuck card's power cycle (reads only: the flush does not touch them).
            REQUIRE(queueEmpty() || lostCommand);
            ++flushes;
            if (flushNeverDone && byte260) {
                byte261 = 1;
                return result;
            }
            busyUs += kProgramUs * double(held.size());  // busy until what it holds is programmed
            for (long id : held) onFlash.insert(id);
            held.clear();
            byte261 = 0;
            return result;
        }
        if (!write) {
            if (!result) *value = enableLag ? 0 : byte262;
            if (enableLag) --enableLag;
            if (lazy && !--lazy) {
                cq = false;
                byte262 = 0;
            }
            return result;
        }
        // The CQ control byte, 00h or 03h (sequential mode).
        REQUIRE(*value == 0 || *value == (kSdCqEnable | kSdCqSequential));
        if (*value) {
            ++enables;
            if (refuseEnable || (requireCache && !byte260)) return result;
            // Enabling needs an empty queue, nothing half-assigned (the engine only enables from legacy mode).
            REQUIRE(!cq || queueEmpty());
            cq = true;
            byte262 = voluntaryOnly ? kSdCqEnable : *value;
            enableLag = lazyEnable;
            openLeft = openLag;
        } else if (stuck) {
            enableLag = 0;  // stays, and shows it
        } else if (lazyLeave) {
            enableLag = 0;  // the disable supersedes a late enable: it shows 03h until it is out
            lazy = lazyLeave;
        } else {
            cq = false;
            byte262 = 0;
            enableLag = 0;
            openLeft = 0;
            clearTasks();
            if (staleTask) tasks[0].used = true;  // r18-sdcq: a Task ID 0 the card keeps across the session's end
        }
        return result;
    }
    // r16-sdcq: a 4 KiB CMD25 (legacy, out of CQ mode), its busy ended: on flash with the cache off; with it on, the card
    // may only hold it. Returns the write's id.
    long legacyWrite()
    {
        trace.push_back(25);
        REQUIRE(!cq);
        ++legacyCommands;
        pendingB = 0;
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
        lazy = enableLag = cacheLag = openLeft = 0;
        pendingB = 0;
        clearTasks();
    }
    // CMD46 of a task (the driver's issue()+complete()): in Task ID order, an assigned task, in CQ mode. r18-sdcq: 0, or
    // the step a failure returns (no response to an illegal one; a data error on fail46At, the task still queued).
    int task46(uint32_t argument)
    {
        trace.push_back(46);
        ++cmd46s;
        if (!cq || openLeft || half) {
            if (openLeft) --openLeft;
            half = false;
            return noAnswer();
        }
        const uint32_t task = argument >> 16 & 0x1f;
        REQUIRE(!(argument & ~(0x1fu << 16)));
        if (task != executedNext || task >= assigned || !tasks[task].used) return noAnswer();  // 4.19.7
        pendingB = 0;
        if (cmd46s == fail46At) return 2;
        tasks[task].used = false;
        ++executedNext;
        ++cqReads;
        if (executedNext == assigned) assigned = executedNext = 0;  // the queue is empty again
        return 0;
    }
    void execute(uint32_t argument) { REQUIRE(!task46(argument)); }
    // CMD17/18/24/25: illegal in CQ mode (Table 4-74).
    void legacy()
    {
        trace.push_back(18);
        REQUIRE(!cq);
        ++legacyCommands;
        pendingB = 0;
    }
};

struct Bus {
    Card *card;
    long settles = 0;
    int command(uint32_t index, uint32_t argument, bool busy, uint32_t *r1)
    {
        return card->command(index, argument, busy, r1);
    }
    int control(uint32_t byte, bool write, uint8_t *value, uint32_t *r1)
    {
        return card->control(byte, write, value, r1);
    }
    int status(bool tasks, uint32_t *r1) { return card->status(tasks, r1); }
    void settle()
    {
        ++settles;
        card->settled();
    }
    uint8_t lastCommand() const { return card->trace.empty() ? 0 : static_cast<uint8_t>(card->trace.back()); }
    uint32_t lastIrq() const { return 0; }
};
using Engine = IOSwitchCqEngine<Bus>;
// r16-sdcq: each control byte is read back at most this many times (a card slow to show it, bounded).
static_assert(Engine::kReadBacks == 10);

static std::vector<IOSwitchCqTask> group(unsigned n, uint64_t base = 1000)
{
    std::vector<IOSwitchCqTask> t;
    for (unsigned i = 0; i < n; ++i) t.push_back({base + i * 977, 8u * (1 + i % 3)});
    return t;
}

// A whole group the way the driver ran one before r18-sdcq's runner: in, assign, execute in order (out: leave).
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

// r18-sdcq: the driver's side of a group for the engine's run (NativeIOSwitchBlock.cpp runGroup's Host): chain i's task,
// its legacy command, its completion, its proof. Every completion is counted: exactly one per chain (the last by the
// caller, after run), the caller's earlier batch exactly one, and only with data read for that chain (by its task, or the
// legacy way), proven if it came from the queue while proofs were due.
struct Chain {
    char data;  // 0 nothing read yet, 'Q' its task, 'L' the legacy way
    bool proven;
    int completions;
};
struct GroupHost {
    Card *card;
    std::vector<Chain> chains;
    bool proving;
    int earlier = 0;
    std::vector<uint8_t> proofs;  // what each chain's proof finds (default: matches)
    std::vector<IOSwitchCqError> errors;
    unsigned cycles = 0, issued = ~0u;
    uint32_t issuedArgument = 0;
    GroupHost(Card *c, unsigned n, bool prove) : card(c), chains(n, Chain{0, false, 0}), proving(prove), proofs(n, 0) {}
    void beginTask(unsigned i, uint32_t argument)
    {
        REQUIRE(i < chains.size() && !chains[i].completions && issued == ~0u);
        issued = i;
        issuedArgument = argument;
    }
    int endTask(unsigned i, uint32_t *r1)
    {
        REQUIRE(issued == i);
        issued = ~0u;
        *r1 = 0;
        if (const int step = card->task46(issuedArgument)) return step;  // the driver: lines reset, CMD12
        chains[i].data = 'Q';
        chains[i].proven = false;
        return 0;
    }
    void beginLegacy(unsigned i)
    {
        REQUIRE(i < chains.size() && !chains[i].completions && issued == ~0u);
        card->legacy();  // out of CQ mode
        issued = i;
    }
    void endLegacy(unsigned i)
    {
        REQUIRE(issued == i);
        issued = ~0u;
        chains[i].data = 'L';
    }
    void complete(unsigned i)
    {
        REQUIRE(i < chains.size() && chains[i].data);
        REQUIRE(!(proving && chains[i].data == 'Q' && !chains[i].proven));
        REQUIRE(++chains[i].completions == 1);
    }
    void completeEarlier() { REQUIRE(++earlier == 1); }
    uint8_t prove(unsigned i)
    {
        REQUIRE(chains[i].data == 'Q' && !chains[i].completions);
        card->legacy();  // the proof's read, out of CQ mode
        // Matches, or Unproven (its legacy read failed: the bytes stand as read, as before r18-sdcq), or Broken (the
        // driver completes it with an I/O error): each may complete; Differs never with the queue's bytes.
        chains[i].proven = proofs[i] != Engine::kProofDiffers;
        return proofs[i];
    }
    void recovered(const IOSwitchCqError &e) { errors.push_back(e); }
    void powerCycle()
    {
        card->powerCycle();
        ++cycles;
    }
    // After run: the caller completes the last chain; every chain then has exactly one completion.
    void finish()
    {
        complete(static_cast<unsigned>(chains.size() - 1));
        REQUIRE(earlier == 1);
        for (const Chain &c : chains) REQUIRE(c.completions == 1 && c.data);
    }
};

static Engine::Outcome runOne(Engine &e, Bus &bus, GroupHost &host, unsigned n, uint64_t base = 1000,
                              unsigned *proven = nullptr)
{
    const std::vector<IOSwitchCqTask> t = group(n, base);
    unsigned matched = 0;
    const Engine::Outcome outcome = e.run(bus, host, t.data(), n, host.proving, &matched);
    host.finish();
    if (proven) *proven = matched;
    return outcome;
}

static void basics()
{
    // The order of commands of one group of 3, then leaving. r18-sdcq: CMD43 opens the session (an empty queue).
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        REQUIRE(e.state == Engine::kIdle && e.depth == 32);
        REQUIRE(runGroup(e, bus, card, 3) && e.state == Engine::kOn && card.cq);
        REQUIRE(e.leave(bus) && e.state == Engine::kIdle && !card.cq);
        const std::vector<uint32_t> expected = {49, 48, 43, 44, 45, 44, 45, 44, 45, 46, 46, 46, 49, 48};
        REQUIRE(card.trace == expected);
        REQUIRE(e.groups == 1 && e.tasks == 3 && e.enters == 1 && e.leaves == 1 && !e.why && !card.illegal);
        // Legacy commands are fine again; a second group re-enters.
        card.legacy();
        REQUIRE(runGroup(e, bus, card, 2) && e.enters == 2);
        // Two groups back to back stay in CQ mode (no CMD49 in between).
        const size_t before = card.trace.size();
        REQUIRE(runGroup(e, bus, card, 4));
        REQUIRE(card.trace[before] == 44 && e.enters == 2);
        REQUIRE(e.leave(bus));
        e.resetCounts();
        REQUIRE(!e.groups && !e.tasks && !e.enters && !e.leaves);
    }
    // A card without a queue (depth field 0 or 1) is never used.
    {
        Engine e;
        e.start(0, false);
        REQUIRE(e.state == Engine::kOff && !e.usable());
        Card card;
        Bus bus{&card};
        REQUIRE(!e.enter(bus) && card.trace.empty());
        e.start(1, false);
        REQUIRE(e.state == Engine::kOff);
    }
    // The depth bounds a group; a group may use all of it. A group larger than the queue is the caller's bug: stopped.
    {
        Card card;
        card.depth = 4;
        Bus bus{&card};
        Engine e;
        e.start(4, false);
        REQUIRE(runGroup(e, bus, card, 4));
        REQUIRE(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(5);
        REQUIRE(!e.submit(bus, t.data(), 5) && e.state == Engine::kOff && e.stopped && !card.cq && e.why);
    }
    // Tasks must be assignable: 1..65535 blocks, a 32-bit block address (checked before anything is sent).
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(8, false);
        REQUIRE(e.enter(bus));
        const size_t before = card.trace.size();
        IOSwitchCqTask bad[1] = {{1ull << 32, 8}};
        REQUIRE(!e.submit(bus, bad, 1) && e.state == Engine::kOff && !card.cq);
        REQUIRE(std::count(card.trace.begin() + long(before), card.trace.end(), 44u) == 0);
    }
    // Encodings.
    REQUIRE(ioswitchSdCmd44(true, 0, 8) == 0x40000008u && ioswitchSdCmd44(true, 31, 0xffff) == 0x401fffffu);
    REQUIRE(ioswitchSdCmd44(false, 3, 1) == 0x00030001u);
    REQUIRE(ioswitchSdTaskArgument(5) == 0x50000u && ioswitchSdCmd43(kSdAbortQueue, 0) == 1 &&
            ioswitchSdCmd43(kSdAbortTask, 7) == 0x70002u);
    // r18-sdcq: the error line's names.
    char names[96];
    ioswitchSdR1ErrorNames(kIllegal | kComCrc | kCqTran, names, sizeof(names));
    REQUIRE(!strcmp(names, "COM_CRC|ILLEGAL"));
    ioswitchSdR1ErrorNames(kCqTran, names, sizeof(names));
    REQUIRE(!strcmp(names, "-"));
    ioswitchSdR1ErrorNames(0xffffffffu, names, 8);
    REQUIRE(strlen(names) == 7);
}

static void failures()
{
    // CMD44 of the third task is garbled on the bus. r18-sdcq: not off for the boot: the recovery reads CMD13 (the
    // garbled command's COM_CRC_ERROR shows there), aborts the queue (CMD43, answered: still in CQ mode) and the queue
    // takes a group at once.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        card.failAt = 3 + 2 * 2 + 1;  // 49, 48, 43, then 44/45 twice, then the third CMD44
        REQUIRE(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(4);
        REQUIRE(!e.submit(bus, t.data(), 4) && e.state == Engine::kOn && e.error.command == 44 && e.error.task == 2);
        REQUIRE(e.error.previous == 45 && e.error.step == 2);
        REQUIRE(e.recover(bus) == Engine::kInPlace && e.state == Engine::kOn && card.cq && card.aborts == 2);
        REQUIRE(e.error.cardStatus & kComCrc && !e.error.statusStep && !e.error.abortStep && !e.error.tasksStep);
        REQUIRE(e.usable() && !e.stopped && e.recoveries == 1);
        REQUIRE(e.submit(bus, t.data(), 4));
        while (e.pending()) {
            card.execute(e.argument());
            e.executed();
        }
        REQUIRE(e.leave(bus));
        card.legacy();
    }
    // CMD45 garbled: the half-assigned task goes with the abort.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        card.failAt = 5;  // 49, 48, 43, 44, then CMD45
        REQUIRE(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(2);
        REQUIRE(!e.submit(bus, t.data(), 2) && e.error.command == 45);
        REQUIRE(e.recover(bus) == Engine::kInPlace && card.queueEmpty() && !card.half);
    }
    // A task's data fails: abort, the queue empty, still in CQ mode.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        REQUIRE(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(3);
        REQUIRE(e.submit(bus, t.data(), 3));
        card.execute(e.argument());
        e.executed();
        REQUIRE(e.recover(bus) == Engine::kInPlace && !e.pending() && card.queueEmpty() && e.error.taskStatus == 6u);
        // fail() is still the stop: abort, leave, off for the boot.
        REQUIRE(e.submit(bus, t.data(), 3));
        REQUIRE(e.fail(bus, "a proof differed") && e.state == Engine::kOff && e.stopped && !card.cq);
        REQUIRE(!e.pending() && !e.enter(bus));
    }
    // A failure with nothing queued (a proof that differed after leaving): nothing sent, off.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        REQUIRE(runGroup(e, bus, card, 2) && e.leave(bus));
        const size_t before = card.trace.size();
        REQUIRE(e.fail(bus, "a proof differed") && card.trace.size() == before && e.state == Engine::kOff);
        REQUIRE(!strcmp(e.why, "a proof differed"));
    }
    // The card does not take the enable: the engine makes sure it is out (writes 00h, reads 00h). r18-sdcq: idle, not
    // off: the next group tries again.
    {
        Card card;
        card.refuseEnable = true;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        REQUIRE(!e.enter(bus) && e.state == Engine::kIdle && !card.cq && e.why && e.error.command == 48);
        REQUIRE(card.trace.back() == 48 && e.usable());
    }
    // The enable's own CMD49 is garbled: no response, COM_CRC_ERROR in the next R1 (off()'s CMD49 00h, sent again).
    {
        Card card;
        card.failAt = 1;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        REQUIRE(!e.enter(bus) && e.state == Engine::kIdle && !card.cq && e.error.command == 49 && e.error.step == 2);
        REQUIRE(e.enter(bus));
    }
    // A card slow to leave: read back until it is out, a settle between read-backs.
    {
        Card card;
        card.lazyLeave = 3;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        REQUIRE(runGroup(e, bus, card, 2));
        REQUIRE(e.leave(bus) && !card.cq && bus.settles == 3 && e.state == Engine::kIdle);
        card.legacy();
    }
    // A card that never leaves: false after kReadBacks read-backs, stuck (the driver power-cycles it). r18-sdcq: the
    // power cycle gives the queue back, kMaxStuck times a boot; then off for the boot.
    {
        Card card;
        card.stuck = true;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        for (unsigned cycle = 1; cycle <= Engine::kMaxStuck; ++cycle) {
            const long settles = bus.settles;
            REQUIRE(runGroup(e, bus, card, 2));
            REQUIRE(!e.leave(bus) && e.state == Engine::kOff && e.stuck && card.cq &&
                    bus.settles - settles == long(Engine::kReadBacks - 1));
            card.powerCycle();
            e.powerCycled();
            REQUIRE(e.powerCycles == cycle && !e.stuck);
            REQUIRE(cycle < Engine::kMaxStuck ? e.state == Engine::kIdle && !e.stopped
                                              : e.state == Engine::kOff && e.stopped && !e.enter(bus));
        }
    }
    // System sleep between groups: CQ mode does not survive the power cycle; the engine knows.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        REQUIRE(runGroup(e, bus, card, 2) && e.leave(bus));
        e.powerCycled();
        REQUIRE(e.state == Engine::kIdle && runGroup(e, bus, card, 2) && e.leave(bus));
    }
    // Leaving with tasks still queued is a bug in the caller: r18-sdcq: they are aborted first, the queue stays on.
    {
        Card card;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        REQUIRE(e.enter(bus));
        const std::vector<IOSwitchCqTask> t = group(3);
        REQUIRE(e.submit(bus, t.data(), 3));
        REQUIRE(e.leave(bus) && e.state == Engine::kIdle && !card.cq && card.aborts == 2 && e.usable());
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
    for (long id : completed) REQUIRE(card.onFlash.count(id));
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
        REQUIRE(!e.enter(bus) && !card.cq && e.cqRead == 0 && e.cqReads == Engine::kReadBacks && e.why);
        REQUIRE(bus.settles == Engine::kReadBacks - 1);  // the enable's read-backs (leaving reads 00h at once)
    }
    {
        Card card;
        card.requireCache = true;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        REQUIRE(runGroup(e, bus, card, 4) && e.cacheOn && card.byte260 == 1);
        REQUIRE(e.leave(bus) && !card.cq && !e.cacheOn && card.byte260 == 0 && e.useCache);
        const std::vector<uint32_t> expected = {49, 48, 49, 48, 43, 44, 45, 44, 45, 44, 45,
                                                44, 45, 46, 46, 46, 46, 49, 48, 49, 48};
        REQUIRE(card.trace == expected);
        // The write: today's command and cost exactly (no flush), on flash when it completes.
        std::set<long> completed;
        const WriteResult w = driverWrite(e, bus, card, completed);
        REQUIRE(w.completed && w.commands == 1 && w.us == kProgramUs && card.held.empty());
        // The next session turns the cache on again; a power cycle in the middle of it loses nothing written.
        REQUIRE(runGroup(e, bus, card, 2) && card.byte260 == 1);
        card.powerCycle();
        e.powerCycled();
        assertKept(card, completed);
        REQUIRE(!e.cacheOn && e.state == Engine::kIdle);
        REQUIRE(runGroup(e, bus, card, 2) && e.leave(bus) && card.byte260 == 0);
        // The queue off for the boot: the cache stays off for good.
        REQUIRE(e.fail(bus, "a proof differed") && !e.cacheOn && !e.useCache && card.byte260 == 0);
    }
    // A lazy card: the enable shows after a few read-backs (settled between), within the engine's window.
    {
        Card card;
        card.lazyEnable = 3;
        Bus bus{&card};
        Engine e;
        e.start(32, false);
        REQUIRE(e.enter(bus) && e.cqReads == 4 && bus.settles == 3 && e.cqRead == 3);
        REQUIRE(e.leave(bus) && !card.cq);
    }
    // One lazier than the window: out of CQ mode (it was in already), the cache off. r18-sdcq: idle, tried again later.
    {
        Card card;
        card.lazyEnable = Engine::kReadBacks;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        REQUIRE(!e.enter(bus) && !card.cq && e.state == Engine::kIdle && e.cqRead == 0 && !e.cacheOn && !card.byte260);
        card.legacy();
    }
    // A card that keeps voluntary mode: the read-back says which bit did not take; out of CQ mode, the cache off.
    {
        Card card;
        card.voluntaryOnly = true;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        REQUIRE(!e.enter(bus) && !card.cq && e.cqRead == kSdCqEnable && !e.cacheOn && card.byte260 == 0);
        REQUIRE(e.cacheRead == kSdCacheEnable);  // the cache did come on first
        REQUIRE(strstr(ioswitchSdCqControlMeaning(e.cqRead), "voluntary"));
        REQUIRE(strstr(ioswitchSdCqControlMeaning(0), "disabled") &&
                strstr(ioswitchSdCqControlMeaning(3), "sequential"));
        card.legacy();
    }
    // A cache that will not come on (the card answers the enable): tried once (read back 00h, 00h written and read back),
    // never again this boot; the queue still gets its try.
    {
        Card card;
        card.refuseCache = true;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        REQUIRE(runGroup(e, bus, card, 2) && e.leave(bus) && !e.cacheOn && !e.useCache && e.cacheRead == 0);
        const size_t before = card.trace.size();
        REQUIRE(runGroup(e, bus, card, 2) && e.leave(bus));
        REQUIRE(card.trace[before] == 49 && card.trace[before + 1] == 48 && card.trace[before + 2] == 43 &&
                card.trace[before + 3] == 44);  // 262 only
    }
    // r18-sdcq: a cache enable garbled on the bus is no refusal: the next session tries it again.
    {
        Card card;
        card.failAt = 1;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        REQUIRE(runGroup(e, bus, card, 2) && e.useCache && e.leave(bus));
        REQUIRE(runGroup(e, bus, card, 2) && e.cacheOn && card.byte260 == 1 && e.leave(bus));
    }
    // A cache lazier than the window: 00h cancels it; the engine knows it is off.
    {
        Card card;
        card.lazyCache = Engine::kReadBacks + 1;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        REQUIRE(runGroup(e, bus, card, 2) && !e.cacheOn && card.byte260 == 0 && !e.useCache);
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
        REQUIRE(runGroup(e, bus, card, 2) && e.leave(bus) && e.cacheOn && card.byte260 == 1 && !e.useCache);
        std::set<long> completed;
        const WriteResult w = driverWrite(e, bus, card, completed);
        REQUIRE(w.completed && w.commands == 3 && card.held.empty());  // CMD25, the flush's CMD49 and CMD48
        REQUIRE(w.us == kCachedUs + 2 * kRegisterUs + kProgramUs);
        card.powerCycle();
        e.powerCycled();
        assertKept(card, completed);
        // Once it would not go off, no session turns it on again: after the power cycle it stays off, writes as today.
        REQUIRE(runGroup(e, bus, card, 2) && e.leave(bus) && !e.cacheOn && card.byte260 == 0);
        REQUIRE(driverWrite(e, bus, card, completed).us == kProgramUs);
    }
    {
        Card card;
        card.refuseDisable = true;
        card.flushNeverDone = true;
        Bus bus{&card};
        Engine e;
        e.start(32, true);
        std::set<long> completed;
        REQUIRE(driverWrite(e, bus, card, completed).completed);
        REQUIRE(runGroup(e, bus, card, 2) && e.leave(bus) && e.cacheOn);
        const WriteResult lost = driverWrite(e, bus, card, completed);
        REQUIRE(!lost.completed && card.held.size() == 1);
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
        REQUIRE(driverWrite(e, bus, card, completed).completed);
        REQUIRE(runGroup(e, bus, card, 2) && card.byte260 == 1);
        REQUIRE(!e.fail(bus, "data error") && e.stuck && card.cq && e.cacheOn && !e.flush(bus));
        card.powerCycle();
        e.powerCycled();
        assertKept(card, completed);
        REQUIRE(!e.cacheOn && e.state == Engine::kOff);
    }
}

// r18-sdcq: r8c's boot as the model sees it, through the group runner: setup's test group (16 tasks, its own session,
// then the legacy compare), then the data path's first group, proving. Each card r8c's could have been fails that group
// somewhere; every request completes exactly once and the queue stays on.
static void setupGroup(Engine &e, Bus &bus, Card &card)
{
    REQUIRE(runGroup(e, bus, card, 16) && e.leave(bus));
    for (unsigned i = 0; i < 16; ++i) card.legacy();  // the compare's legacy reads
    e.resetCounts();
}
// The second session's first CMD44 gets no response (the 17th CMD44 of the boot).
TEST(SdCommandQueue, R8cSecondSessionCmd44Unanswered)
{
    Card card;
    card.refuse44At = 17;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    setupGroup(e, bus, card);
    GroupHost host(&card, 6, true);
    unsigned proven = 0;
    REQUIRE(runOne(e, bus, host, 6, 5000, &proven) == Engine::kGroupRetried && proven == 6);
    REQUIRE(host.errors.size() == 1 && e.recoveries == 1 && e.retried == 1 && !e.legacyGroups);
    const IOSwitchCqError &err = host.errors[0];
    REQUIRE(err.command == 44 && err.task == 0 && err.step == 2 && err.previous == 43 && err.attempt == 1);
    // The delayed bit: CMD13 right after names the unanswered CMD44; CMD43 answered, so the retry stays in CQ mode.
    REQUIRE(err.cardStatus & kIllegal && !err.abortStep && err.outcome == Engine::kInPlace && err.tasks == 6);
    REQUIRE(e.usable() && !e.stopped && e.why);
    // The next groups go through the queue (the proofs left CQ mode; back to back, they stay in it: no CMD49).
    GroupHost next(&card, 4, false);
    REQUIRE(runOne(e, bus, next, 4) == Engine::kGroupQueued);
    const size_t before = card.trace.size();
    GroupHost third(&card, 2, false);
    REQUIRE(runOne(e, bus, third, 2, 9000) == Engine::kGroupQueued && card.trace[before] == 44);
    REQUIRE(e.leave(bus) && card.illegal == 1);
}

// The CMD44 answered with an error bit some earlier command left (CC_ERROR, clear by read): recovered, retried.
TEST(SdCommandQueue, R8cCmd44AnsweredWithAnErrorBit)
{
    Card card;
    card.error44At = 17;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    setupGroup(e, bus, card);
    GroupHost host(&card, 3, true);
    REQUIRE(runOne(e, bus, host, 3) == Engine::kGroupRetried && host.errors[0].r1 & kCcError);
    REQUIRE(!host.errors[0].step && !(host.errors[0].cardStatus & kCcError) && e.usable());
}

// A card that keeps Task ID 0 across a session's end ("Task ID already exists": no response, 4.19.5): the CMD43
// that opens every session empties it, so nothing fails at all.
TEST(SdCommandQueue, R8cStaleTaskIdClearedByTheOpeningAbort)
{
    Card card;
    card.staleTask = true;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    setupGroup(e, bus, card);
    REQUIRE(card.tasks[0].used);  // the stale task the setup session left
    const long aborts = card.aborts;
    GroupHost host(&card, 5, true);
    REQUIRE(runOne(e, bus, host, 5) == Engine::kGroupQueued && !card.illegal && !e.recoveries);
    REQUIRE(card.cmd44s == 16 + 5 && card.aborts == aborts + 1);  // the opening CMD43 cleared it, nothing else
}

// A card whose queue opens a few commands after 03h reads back (a lazy init): the opening CMD43 is probed (settled
// between) until the card answers, before any CMD44.
TEST(SdCommandQueue, R8cLateQueueProbedOpen)
{
    Card card;
    card.openLag = 3;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    GroupHost host(&card, 4, true);
    REQUIRE(runOne(e, bus, host, 4) == Engine::kGroupQueued && !e.recoveries && e.probes >= 1);
    REQUIRE(card.cmd44s == 4);  // no CMD44 before the queue was open
}

// One that never opens within the window: out of CQ mode, recovered, tried again; the group the legacy way.
TEST(SdCommandQueue, R8cQueueThatNeverOpens)
{
    Card card;
    card.openLag = 1000;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    GroupHost host(&card, 4, true);
    REQUIRE(runOne(e, bus, host, 4) == Engine::kGroupLegacy && host.errors.size() == 2 && !card.cq);
    REQUIRE(host.errors[0].command == 43 && host.errors[1].attempt == 2 && e.usable() && !card.cmd44s);
}

// A card whose real depth is 31 although byte 6 says 32 (Task ID 31 gets no response): the 32-task group fails twice
// and goes the legacy way; at 31 (the driver's depth: one below byte 6, as the Raspberry Pi kernel) nothing fails.
TEST(SdCommandQueue, R8cDepthBelowByteSix)
{
    Card card;
    card.rejectId = 31;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    GroupHost host(&card, 32, false);
    REQUIRE(runOne(e, bus, host, 32) == Engine::kGroupLegacy && host.errors.size() == 2);
    REQUIRE(host.errors[0].command == 44 && host.errors[0].task == 31 && e.usable() && e.legacyGroups == 1);
    Engine capped;
    capped.start(31, true);
    Card card2;
    card2.rejectId = 31;
    Bus bus2{&card2};
    GroupHost host2(&card2, 31, false);
    REQUIRE(runOne(capped, bus2, host2, 31) == Engine::kGroupQueued && !card2.illegal);
}

// A task's data fails (CMD46): recovered in CQ mode, the chains from that task on assigned again (Task ID 0 on).
TEST(SdCommandQueue, R8cTaskDataFailure)
{
    Card card;
    card.fail46At = 3;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    GroupHost host(&card, 5, false);
    REQUIRE(runOne(e, bus, host, 5) == Engine::kGroupRetried && host.errors[0].command == 46);
    REQUIRE(host.errors[0].task == 2 && host.errors[0].outcome == Engine::kInPlace);
    for (unsigned i = 0; i < 5; ++i) REQUIRE(host.chains[i].data == 'Q');
    REQUIRE(e.tasks == 5 + 3);
}

// While proving: a data failure re-runs the rest through the queue and every chain is proven before it completes.
TEST(SdCommandQueue, R8cTaskDataFailureWhileProving)
{
    Card card;
    card.fail46At = 2;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    GroupHost host(&card, 4, true);
    unsigned proven = 0;
    REQUIRE(runOne(e, bus, host, 4, 1000, &proven) == Engine::kGroupRetried && proven == 4);
}

// A proof that differs: the one stop for the boot; the whole group read again the legacy way, every chain once.
TEST(SdCommandQueue, R8cProofThatDiffersStops)
{
    Card card;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    GroupHost host(&card, 4, true);
    host.proofs[2] = Engine::kProofDiffers;
    REQUIRE(runOne(e, bus, host, 4) == Engine::kGroupStopped && e.stopped && !e.usable());
    for (const Chain &c : host.chains) REQUIRE(c.data == 'L');
    GroupHost after(&card, 2, false);
    REQUIRE(runOne(e, bus, after, 2) == Engine::kGroupStopped && after.chains[0].data == 'L');
    Engine::runLegacy(after, 0);
}

// A proof whose legacy read failed proves nothing (Unproven) and one whose bytes could not be made right (Broken)
// still completes (with an error, the driver's): neither stops the queue.
TEST(SdCommandQueue, R8cUnprovenAndBrokenProofs)
{
    Card card;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    GroupHost host(&card, 3, true);
    host.proofs[0] = Engine::kProofUnproven;
    host.proofs[1] = Engine::kProofBroken;
    unsigned proven = 0;
    REQUIRE(runOne(e, bus, host, 3, 1000, &proven) == Engine::kGroupQueued && proven == 1 && e.usable());
}

// A card stuck in CQ mode at a proving group's end: power-cycled, the group the legacy way, the queue on after it.
TEST(SdCommandQueue, R8cStuckAtProvingGroupEnd)
{
    Card card;
    card.stuck = true;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    GroupHost host(&card, 3, true);
    REQUIRE(runOne(e, bus, host, 3) == Engine::kGroupLegacy && host.cycles == 1 && e.usable() && !card.cq);
    for (const Chain &c : host.chains) REQUIRE(c.data == 'L');
}

// Chains too few for the queue (fewer than a group from legacy mode needs): each the legacy way, every one once.
TEST(SdCommandQueue, R8cTooFewChainsForTheQueue)
{
    Card card;
    GroupHost host(&card, 3, false);
    Engine::runLegacy(host, 3);
    host.finish();
}

// A card that never takes a CMD44 again: kStrikes groups in a row fail, then the queue pauses (no group for
// kPauseMin reads that could start one), tries again, pauses twice as long; never off for the boot.
TEST(SdCommandQueue, R8cPersistentFailuresPause)
{
    Card card;
    card.refuse44From = 1;
    Bus bus{&card};
    Engine e;
    e.start(32, true);
    unsigned groups = 0;
    for (unsigned pause = 0; pause < 3; ++pause) {
        for (unsigned i = 0; i < Engine::kStrikes; ++i) {
            REQUIRE(e.admit());
            GroupHost host(&card, 2, false);
            REQUIRE(runOne(e, bus, host, 2, 100 * ++groups) == Engine::kGroupLegacy);
        }
        REQUIRE(e.pausedNow && e.pauses == pause + 1 && e.pauseLeft == Engine::kPauseMin << pause);
        for (unsigned i = 0; i < Engine::kPauseMin << pause; ++i) REQUIRE(!e.admit());
        REQUIRE(e.admit() && e.usable() && !e.stopped);
    }
    REQUIRE(e.skipped == Engine::kPauseMin * 7 && e.legacyGroups == 3 * Engine::kStrikes);
    // The card gets well: a clean run of kForgive groups brings the pause back to its shortest.
    card.refuse44From = -1;
    for (unsigned i = 0; i < Engine::kForgive; ++i) {
        GroupHost host(&card, 2, false);
        REQUIRE(runOne(e, bus, host, 2) == Engine::kGroupQueued);
    }
    REQUIRE(e.pauseLength == Engine::kPauseMin && e.leave(bus));
}

// The driver's loop over random work with random bus failures and card behaviours: whatever happens, the card's rules
// hold (the model asserts them), a legacy command only ever finds the card out of CQ mode, the engine never says the card
// is out of CQ mode while it is in, and after a failure the engine stays off. When the engine cannot get the card out
// (false from leave/fail) the driver power-cycles it, as here. r16-sdcq: the power also goes at random points (sleep, a
// stuck card's reset, or just lost: between a write's command and its completion too), and no completed write is ever
// lost; a flush that fails fails its write and nothing else; each write's cost is compared with today's. r18-sdcq: groups
// go through the runner with r8c's kinds of card in the mix: every chain completes exactly once, only with data read
// for it, proven while proving; the queue stops for the boot only for a proof that differs or kMaxStuck power cycles.
static void randomWork()
{
    std::mt19937_64 random(0x5dc9);
    unsigned powerCycles = 0, groups = 0, failures = 0, sleeps = 0, losses = 0, recovered = 0, legacyGroups = 0;
    unsigned stops = 0;
    unsigned long writes = 0, sameAsToday = 0, flushedWrites = 0, failedWrites = 0, chains = 0;
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
        card.openLag = random() % 10 == 0 ? unsigned(random() % 14) : 0;
        card.staleTask = random() % 10 == 0;
        card.refuse44At = random() % 6 == 0 ? long(1 + random() % 40) : -1;
        card.error44At = random() % 10 == 0 ? long(1 + random() % 40) : -1;
        card.refuse44From = random() % 40 == 0 ? long(1 + random() % 40) : -1;
        card.rejectId = random() % 20 == 0 ? unsigned(1 + random() % 31) : 32;
        card.fail46At = random() % 6 == 0 ? long(1 + random() % 60) : -1;
        const bool differs = random() % 60 == 0;
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
        // The driver's answer to "the card stays in CQ mode" (cqLeave -> cqPowerCycle): the power cycle (resetCard),
        // which needs no flush first.
        const auto out = [&](bool isOut) {
            if (isOut) {
                REQUIRE(!card.cq && !e.stuck);
                return;
            }
            REQUIRE(e.state == Engine::kOff && e.stuck);
            powerCycle();
        };
        bool gone = false;
        unsigned proofsLeft = random() % 2 ? 32 : 0;
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
                    REQUIRE(cacheAtWrite);  // only a flush can fail a write
                    ++failedWrites;
                    continue;
                }
                nowUs += w.us;
                if (!cacheAtWrite) {
                    // The cache off: today's write exactly, one command, no flush.
                    REQUIRE(w.commands == 1 && w.us == kProgramUs);
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
                // A group, as the worker hands one to runGroup: through the runner while the queue is usable and not
                // paused, else (as groupLocked) nothing is gathered and the read goes the legacy way.
                const unsigned n = 1 + unsigned(random() % card.depth);
                if (!e.usable() || !e.admit()) {
                    out(e.leave(bus));
                    card.legacy();
                    continue;
                }
                const bool proving = proofsLeft != 0;
                GroupHost host(&card, n, proving);
                if (differs && proving) host.proofs[random() % n] = Engine::kProofDiffers;
                const std::vector<IOSwitchCqTask> t = group(n, random() % 100000);
                unsigned proven = 0;
                const Engine::Outcome outcome = e.run(bus, host, t.data(), n, proving, &proven);
                host.finish();  // every chain exactly once
                chains += n;
                ++groups;
                proofsLeft = proofsLeft > proven ? proofsLeft - proven : 0;
                recovered += unsigned(host.errors.size());
                if (outcome == Engine::kGroupLegacy) ++legacyGroups;
                if (!host.errors.empty()) ++failures;
                if (outcome == Engine::kGroupStopped) {
                    ++stops;
                    REQUIRE(e.stopped && (differs || e.powerCycles >= Engine::kMaxStuck));
                }
                if (outcome == Engine::kGroupQueued) REQUIRE(host.errors.empty());
                for (const IOSwitchCqError &err : host.errors) REQUIRE(err.attempt == 1 || err.attempt == 2);
                REQUIRE(e.state != Engine::kOn || card.cq);
            }
            if (e.state == Engine::kOff) REQUIRE(e.why || card.depth < 2 || e.stuck);
            // The queue is off only when stopped for the boot (or never usable, or stuck until its power cycle).
            REQUIRE(e.usable() || e.stopped || e.stuck || card.depth < 2);
            // Whenever the engine is not in CQ mode and has not said the card is stuck, the card is out of CQ mode.
            REQUIRE(e.state == Engine::kOn || e.stuck || !card.cq);
            // r16-sdcq: the engine never thinks the cache is off while the card has it on (or will show it later); out
            // of CQ mode it is off unless it would not go off; nothing completed is only in the cache.
            REQUIRE(e.cacheOn || !card.byte260);
            REQUIRE(e.state == Engine::kOn || e.stuck || !card.byte260 || !e.useCache || card.cacheLag);
            for (long id : card.held) REQUIRE(!completed.count(id));
        }
        if (!gone) powerCycle();  // the end of every round: the power goes
    }
    // Every path was exercised.
    REQUIRE(powerCycles > 20000 && groups > 20000 && failures > 1000 && sleeps > 1000 && losses > 1000);
    REQUIRE(recovered > failures && legacyGroups > 100 && stops > 10 && chains > 100000);
    REQUIRE(flushedWrites > 100 && failedWrites > 0 && sameAsToday > 10000);
    printf(
        "groups %u (%lu chains, every one completed once): %u with errors (%u recovered, %u read the legacy way), %u "
        "stopped for the boot\n",
        groups, chains, failures, recovered, legacyGroups, stops);
    printf(
        "writes %lu: %lu as today (cache off, %.0f us each), %lu flushed (cache would not go off), %lu failed (flush "
        "failed); mean %.1f us now against %.1f us today (model: register %.0f us, program %.0f us, cached %.0f "
        "us)\n",
        writes, sameAsToday, kProgramUs, flushedWrites, failedWrites, nowUs / double(sameAsToday + flushedWrites),
        todayUs / double(writes), kRegisterUs, kProgramUs, kCachedUs);
}

TEST(SdCommandQueue, Basics)
{
    basics();
}
TEST(SdCommandQueue, Failures)
{
    failures();
}
TEST(SdCommandQueue, BringUpAndTheCache)
{
    bringUp();
}
TEST(SdCommandQueue, RandomWork)
{
    randomWork();
}
