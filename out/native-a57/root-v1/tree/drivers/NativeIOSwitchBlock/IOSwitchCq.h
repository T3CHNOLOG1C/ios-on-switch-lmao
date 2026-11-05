// SPDX-License-Identifier: BSD-2-Clause-Patent
// r14-sdcq: the SD command queue (SD Physical Layer 6.00, 4.19) as the SD driver (NativeIOSwitchBlock.cpp) drives it: its
// state and the order of its commands, apart from the controller registers (the `Bus`), so the host can run it against a
// model card that enforces the spec's rules (tests/native_sd_cq_harness.cpp).
//
// Sequential CQ mode, reads only. In Voluntary mode the card marks tasks ready in its Task Status register and the host
// has to read it with CMD13 until one is (4.19.6: "at least one task ready within 250 ms"; nothing signals it), so an
// idle queue would need a timer. In Sequential mode (Performance Enhancement byte 262 = 03h) the host assigns a group of
// tasks at once from Task ID 0 with an empty queue (CMD44 then CMD45 for each), then executes them with CMD46 in Task ID
// order "without polling for ready state" (4.19.1.2): the card starts each one's data when it has it (within 100 ms,
// 4.19.7) and the controller's transfer-complete interrupt ends it. Every task's address reaches the card before the
// first CMD46, so it can fetch them while earlier ones move: the overlap a queue is for, with nothing polled.
//
// While CQ is enabled CMD17/18/24/25 are illegal (Table 4-74: only CMD0/12/13, CMD43-47, CMD32/33/38, CMD48/49/58/59,
// CMD6 and CMD19 are legal), so the driver leaves CQ mode before any legacy command (writes, lone reads after its queue
// ran dry, proofs) and when its queue runs dry. Every CMD49 is read back with CMD48. Any error aborts the queue (CMD43),
// leaves CQ mode and keeps the queue off for the boot (`fail`).
//
// r16-sdcq: r7z's card (SanDisk SR01T, 2024-11) took CMD49 03h with a clean R1 and its CMD48 read-back did not show 03h.
// What the engine did not do, against the spec and both Linux kernels that drive SD CQ: (1) the cache first. 4.19.1:
// "Cache Feature (Refer to Section 4.17) should be enabled before enabling Command Queue"; 4.16.2: A2 performance is
// measured with both; Linux's mmc_sd_init_card enables the cache (byte 260) before anything else, and the Raspberry Pi
// kernel (rpi-6.12.y sd.c sd_parse_ext_reg_perf) does not even count a queue as supported without the cache and Power
// Off Notification. (2) A lazy card: one read-back right after the CMD49's busy. The Raspberry Pi kernel re-reads the
// byte for 10 ms (sd_ops.c mmc_sd_cmdq_switch, commit 45fa95bc: "cards may have a lazy init/deinit sequence"; "make sure
// the register write actually stuck, as some cards claim support but never report back"). Now every control byte this
// engine writes is read back up to kReadBacks times, a settle() between (~10 ms in all): each read-back is a CMD48, a
// command waited for by interrupt; the settle is a sleep, not a spin. (3) Which bit did not take: Enable CQ (bit 0) and
// CQ Mode (bit 1) are separate (Table 5-30), so the read-back is kept (lastRead) for the driver's one line.
//
// The cache (byte 260) is on only inside a CQ session, because 4.19.1 asks for it there, never for writes: enter() turns
// it on right before 262 = 03h, and leaving CQ mode turns it off right after 262 = 00h. The queue carries reads only and
// every write is a legacy command, out of CQ mode, so every write finds the cache off and completes, as before, once the
// card's busy says it is programmed: nothing written is ever held in the cache, no synchronize has anything to flush, and
// a power cycle at any point loses no completed write. A cache that will not go off (cacheOn after a failed disable) is
// the one case left: every write is then flushed before it completes (commitWrite: byte 261 = 01h, the card busy on DAT0
// until done, then the bit reads 0: 4.17), write-through either way, and a flush that fails fails only that write. (SD has
// no per-write forced programming: CMD23's argument is all block count, 4.15 / Table 4-24; bit 24 is eMMC's.) Nothing
// here panics. The bytes this engine ever writes: 260 with 00h or 01h, 261 with 01h, 262 with 00h or 03h (Bus::control
// refuses anything else).
#pragma once

#include <stdint.h>

#include "IOSwitchSdCard.h"

struct IOSwitchCqTask {
    uint64_t lba;
    uint32_t sectors;
};

// Bus: int command(uint32_t index, uint32_t argument, bool busy, uint32_t *r1): CMD43 (busy) / CMD44 / CMD45, 0 or a
// nonzero controller error (no response, CRC, timeout); int control(uint32_t byte, bool write, uint8_t *value): CMD49 of
// *value into performance-function byte `byte` (260, 261 or 262) / CMD48 of that byte into *value, 0 or nonzero (a
// controller error or an R1 error); void settle(): about a millisecond (only between read-backs of a byte the card has
// not shown the written value in yet).
template <typename Bus> struct IOSwitchCqEngine {
    enum State : uint8_t { kOff, kIdle, kOn };
    static constexpr uint32_t kR1Errors = 0xfdf90008u;  // the driver's R1_ERRORS
    static constexpr uint8_t kEnabled = kSdCqEnable | kSdCqSequential, kDisabled = 0;
    // Read-backs of a control byte before the card is taken not to have applied a write (a settle() before each but the
    // first, ~10 ms in all: the Raspberry Pi kernel's mmc_sd_cmdq_switch allows 10 ms for the CQ byte too).
    static constexpr unsigned kReadBacks = 10;
    static constexpr unsigned kMaxDepth = 32;
    State state = kOff;
    uint8_t depth = 0;
    unsigned queued = 0, next = 0;  // tasks assigned in the current group, and the next one to execute
    const char *why = nullptr;      // the first failure's reason
    bool stuck = false;             // the card would not leave CQ mode: only a power cycle gets it out
    // r16-sdcq: the card's cache. useCache: the card offers one and it has not refused it, so each CQ session turns it on
    // first. cacheOn: it may be enabled (set before the CMD49 that enables it; cleared only by a read-back that shows it
    // off, or by a power cycle), so a write must be flushed before it completes (commitWrite).
    bool useCache = false, cacheOn = false;
    // r16-sdcq: the last value each control byte read back (260, 261, 262) and how many read-backs the last check took;
    // what the last enables read back (the cleanup after a failure reads the bytes again), and in how many read-backs.
    uint8_t lastRead[3] = {};
    unsigned lastReads = 0;
    uint8_t cacheRead = 0, cqRead = 0;
    unsigned cacheReads = 0, cqReads = 0;
    uint64_t enters = 0, leaves = 0, groups = 0, tasks = 0, settles = 0, flushes = 0;

    // The card offers a queue of `queueDepth` (2..32) and the driver may use it; r16-sdcq: `cache`, it offers a cache too.
    void start(uint8_t queueDepth, bool cache)
    {
        depth = queueDepth > kMaxDepth ? kMaxDepth : queueDepth;
        state = depth >= 2 ? kIdle : kOff;
        useCache = cache && state == kIdle;
    }
    // The card was powered off and initialized again (system sleep, or the driver's answer to `stuck`): CQ mode and the
    // cache (Cache Enable is 0 after power-up) did not survive it; the next session turns the cache on again.
    void powerCycled()
    {
        if (state == kOn) state = kIdle;
        queued = next = 0;
        stuck = false;
        cacheOn = false;
    }

    // r16-sdcq: Cache Enable (byte 260 = 01h), out of CQ mode, read back. False when the card does not show it on: then
    // 00h is written and read back, so cacheOn stays only for a card that might still have taken it.
    bool enableCache(Bus &bus)
    {
        if (state == kOn) return false;
        cacheOn = true;
        uint8_t value = kSdCacheEnable;
        const bool taken =
            !bus.control(kSdPerfCacheControl, true, &value) && readBack(bus, kSdPerfCacheControl, 1, kSdCacheEnable);
        cacheRead = lastRead[0];
        cacheReads = lastReads;
        if (taken) return true;
        (void)disableCache(bus);
        return false;
    }
    // r16-sdcq: Flush Cache (byte 261 = 01h): the card busy on DAT0 while it commits its cache (at most 1 s: 4.17), then
    // the bit reads 0. Out of CQ mode only (early SanDisk A2 cards never finished a flush in CQ mode: Linux quirks.h).
    // True with nothing to flush. A CMD49 that failed proves nothing (bit 0 also reads 0 when no flush was asked for): it
    // is sent once more (asking for a flush twice is harmless), and only one the card answered counts.
    bool flush(Bus &bus)
    {
        if (!cacheOn) return true;
        if (state == kOn || stuck) return false;
        ++flushes;
        uint8_t value = kSdFlushCache;
        if (bus.control(kSdPerfFlushControl, true, &value)) {
            value = kSdFlushCache;
            if (bus.control(kSdPerfFlushControl, true, &value)) return false;
        }
        return readBack(bus, kSdPerfFlushControl, kSdFlushCache, 0);
    }
    // r16-sdcq: the cache off (byte 260 = 00h, resent once if its CMD49 failed on the bus, read back), out of CQ mode. No
    // flush first: nothing written while it was on has completed without one (commitWrite). False when the card does not
    // show it off: cacheOn stays, and writes are flushed. Only a CMD49 the card answered makes a 00h read-back mean off
    // (its busy lasts until it is done, 5.7.2.2; a lazy enable reads 00h too). A card stuck in CQ mode is left alone: its
    // power cycle clears it.
    bool disableCache(Bus &bus)
    {
        if (!cacheOn) return true;
        if (state == kOn || stuck) return false;
        uint8_t value = 0;
        if (bus.control(kSdPerfCacheControl, true, &value)) {
            value = 0;
            if (bus.control(kSdPerfCacheControl, true, &value)) return false;
        }
        if (!readBack(bus, kSdPerfCacheControl, 1, 0)) return false;
        cacheOn = false;
        return true;
    }
    // r16-sdcq: what a legacy write needs before it completes: nothing while the cache is off (every session's end turns
    // it off), a flush while it may be on. False: the write is not known to be on flash (the driver fails it).
    bool commitWrite(Bus &bus) { return !cacheOn || flush(bus); }

    // Into CQ mode (from kIdle): CMD49 03h, read back 03h (lazily: kReadBacks). False (and off for the boot) when it does
    // not take; lastRead[2] then holds what the byte read back.
    // r16-sdcq: the cache on first while the card offers it (4.19.1); a card that will not show it on keeps it off for
    // the boot and still gets its try at the queue.
    bool enter(Bus &bus)
    {
        if (state == kOn) return true;
        if (state != kIdle) return false;
        if (useCache && !cacheOn && !enableCache(bus)) useCache = false;
        uint8_t value = kEnabled;
        // A failure here may still have left the card in CQ mode: fail() takes it out either way.
        if (bus.control(kSdPerfCqControl, true, &value))
            return fail(bus, "CMD49 enabling the queue failed", true), false;
        const bool taken = readBack(bus, kSdPerfCqControl, 3, kEnabled);
        cqRead = lastRead[2];
        cqReads = lastReads;
        if (!taken) return fail(bus, "the CQ control byte does not read back 03h", true), false;
        state = kOn;
        queued = next = 0;
        ++enters;
        return true;
    }

    // Out of CQ mode (from kOn, every task executed): CMD49 00h, read back 00h, then (r16-sdcq) the cache off, so the
    // legacy commands that follow (writes among them) find it off. True when the card is out of CQ mode (a cache that
    // would not go off leaves cacheOn: writes are then flushed); false when it stays in (then off for the boot: the
    // driver power-cycles the card, the only way left, and its power cycle clears the cache).
    bool leave(Bus &bus)
    {
        if (state != kOn) return true;
        if (queued != next) return fail(bus, "left with tasks still queued");
        if (!off(bus)) {
            why = why ? why : "the card stays in CQ mode";
            state = kOff;
            stuck = true;
            useCache = false;
            return false;
        }
        state = kIdle;
        queued = next = 0;
        ++leaves;
        // A cache that will not go off is left alone from here (no session turns it on or tries again: each try is up
        // to kReadBacks read-backs); writes are flushed while it may be on.
        if (useCache && !disableCache(bus)) useCache = false;
        return true;
    }

    // Assigns tasks 0..n-1 (n <= depth) to an empty queue: CMD44 (read, id, blocks) then CMD45 (start block) for each.
    // False at the first failure (off for the boot, the queue aborted).
    bool submit(Bus &bus, const IOSwitchCqTask *t, unsigned n)
    {
        if (state != kOn || queued != next) return fail(bus, "tasks assigned to a busy queue"), false;
        if (!n || n > depth) return fail(bus, "a group larger than the queue"), false;
        queued = next = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (!t[i].sectors || t[i].sectors > 0xffff || t[i].lba > 0xffffffffull)
                return fail(bus, "a task the queue cannot take"), false;
            uint32_t r1 = 0;
            if (bus.command(44, ioswitchSdCmd44(true, static_cast<uint8_t>(i), t[i].sectors), false, &r1) ||
                (r1 & kR1Errors))
                return fail(bus, "CMD44 failed"), false;
            ++queued;  // a CMD45 that fails leaves at most this one half-assigned: abort covers it
            if (bus.command(45, static_cast<uint32_t>(t[i].lba), false, &r1) || (r1 & kR1Errors))
                return fail(bus, "CMD45 failed"), false;
        }
        ++groups;
        tasks += n;
        return true;
    }

    // The CMD46 argument of the next task, in Task ID order; executed() once its data is in, fail() if it went wrong.
    bool pending() const { return state == kOn && next < queued; }
    uint32_t argument() const { return ioswitchSdTaskArgument(static_cast<uint8_t>(next)); }
    void executed() { ++next; }
    bool empty() const { return next == queued; }

    // An error (or a proof that differed): abort what is queued (CMD43), leave CQ mode, off for the boot. True when the
    // card is out of CQ mode. `maybeOn`: the card may be in CQ mode although the engine never saw it get there.
    // r16-sdcq: the cache off with the queue; a card stuck in CQ mode is left to the driver's power cycle, which clears
    // it (no completed write is in it: commitWrite).
    bool fail(Bus &bus, const char *reason, bool maybeOn = false)
    {
        if (!why) why = reason;
        const bool wasOn = state == kOn || maybeOn;
        state = kOff;
        bool out = true;
        if (wasOn) {
            if (queued != next) {
                uint32_t r1 = 0;
                (void)bus.command(43, ioswitchSdCmd43(kSdAbortQueue, 0), true, &r1);
            }
            queued = next = 0;
            out = off(bus);
            stuck = !out;
        }
        useCache = false;
        if (out) (void)disableCache(bus);
        return out;
    }

private:
    // r16-sdcq: CMD48 of `byte` until (value & mask) == want, at most kReadBacks times; a settle() before each but the
    // first. The last value read in lastRead.
    bool readBack(Bus &bus, uint32_t byte, uint8_t mask, uint8_t want)
    {
        for (unsigned attempt = 0; attempt < kReadBacks; ++attempt) {
            if (attempt) {
                bus.settle();
                ++settles;
            }
            uint8_t value = static_cast<uint8_t>(~want);
            if (bus.control(byte, false, &value)) continue;
            lastRead[byte - kSdPerfCacheControl] = value;
            lastReads = attempt + 1;
            if ((value & mask) == want) return true;
        }
        lastReads = kReadBacks;
        return false;
    }
    // CMD49 00h, then read back until the enable bit is clear. False when it never is. r16-sdcq: a CMD49 that failed on
    // the bus is sent once more (00h is idempotent): a lazy card that has not shown its enable yet reads 00h either way,
    // so only a write the card answered makes that 00h mean out (if the second fails too, read back anyway: the card may
    // have taken one).
    bool off(Bus &bus)
    {
        uint8_t value = kDisabled;
        if (bus.control(kSdPerfCqControl, true, &value)) {
            value = kDisabled;
            (void)bus.control(kSdPerfCqControl, true, &value);
        }
        return readBack(bus, kSdPerfCqControl, kSdCqEnable, 0);
    }
};
