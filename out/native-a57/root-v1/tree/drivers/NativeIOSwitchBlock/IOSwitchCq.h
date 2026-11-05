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
// leaves CQ mode and keeps the queue off for the boot (`fail`). The only extension-register byte ever written is the CQ
// control byte, with 00h or 03h (Bus::control).
#pragma once

#include <stdint.h>

#include "IOSwitchSdCard.h"

struct IOSwitchCqTask {
    uint64_t lba;
    uint32_t sectors;
};

// Bus: int command(uint32_t index, uint32_t argument, bool busy, uint32_t *r1): CMD43 (busy) / CMD44 / CMD45, 0 or a
// nonzero controller error (no response, CRC, timeout); int control(bool write, uint8_t *value): CMD49 of *value / CMD48
// into *value of the CQ control byte, 0 or nonzero (a controller error or an R1 error); void settle(): about a millisecond
// (only between read-backs of a card that has not left CQ mode yet).
template <typename Bus> struct IOSwitchCqEngine {
    enum State : uint8_t { kOff, kIdle, kOn };
    static constexpr uint32_t kR1Errors = 0xfdf90008u;  // the driver's R1_ERRORS
    static constexpr uint8_t kEnabled = kSdCqEnable | kSdCqSequential, kDisabled = 0;
    // Read-backs of the control byte before a card is taken to be stuck in CQ mode (each after settle() but the first);
    // the Raspberry Pi kernel's mmc_sd_cmdq_switch allows such a card 10 ms too.
    static constexpr unsigned kLeaveTries = 10;
    static constexpr unsigned kMaxDepth = 32;
    State state = kOff;
    uint8_t depth = 0;
    unsigned queued = 0, next = 0;  // tasks assigned in the current group, and the next one to execute
    const char *why = nullptr;      // the first failure's reason
    bool stuck = false;             // the card would not leave CQ mode: only a power cycle gets it out
    uint64_t enters = 0, leaves = 0, groups = 0, tasks = 0, settles = 0;

    // The card offers a queue of `queueDepth` (2..32) and the driver may use it.
    void start(uint8_t queueDepth)
    {
        depth = queueDepth > kMaxDepth ? kMaxDepth : queueDepth;
        state = depth >= 2 ? kIdle : kOff;
    }
    // The card was powered off and initialized again (system sleep, or the driver's answer to `stuck`): CQ mode did not
    // survive it.
    void powerCycled()
    {
        if (state == kOn) state = kIdle;
        queued = next = 0;
        stuck = false;
    }

    // Into CQ mode (from kIdle): CMD49 03h, read back 03h. False (and off for the boot) when it does not take.
    bool enter(Bus &bus)
    {
        if (state == kOn) return true;
        if (state != kIdle) return false;
        uint8_t value = kEnabled;
        // A failure here may still have left the card in CQ mode: fail() takes it out either way.
        if (bus.control(true, &value)) return fail(bus, "CMD49 enabling the queue failed", true), false;
        value = kDisabled;
        if (bus.control(false, &value) || (value & 3) != kEnabled)
            return fail(bus, "the CQ control byte does not read back 03h", true), false;
        state = kOn;
        queued = next = 0;
        ++enters;
        return true;
    }

    // Out of CQ mode (from kOn, every task executed): CMD49 00h, read back 00h. True when the card is out of it; false
    // when it stays in (then off for the boot: the driver power-cycles the card, the only way left).
    bool leave(Bus &bus)
    {
        if (state != kOn) return true;
        if (queued != next) return fail(bus, "left with tasks still queued");
        if (!off(bus)) {
            why = why ? why : "the card stays in CQ mode";
            state = kOff;
            return false;
        }
        state = kIdle;
        queued = next = 0;
        ++leaves;
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
    bool fail(Bus &bus, const char *reason, bool maybeOn = false)
    {
        if (!why) why = reason;
        const bool wasOn = state == kOn || maybeOn;
        state = kOff;
        if (!wasOn) return true;
        if (queued != next) {
            uint32_t r1 = 0;
            (void)bus.command(43, ioswitchSdCmd43(kSdAbortQueue, 0), true, &r1);
        }
        queued = next = 0;
        return off(bus);
    }

private:
    // CMD49 00h, then read back until the enable bit is clear; `stuck` when it never is.
    bool off(Bus &bus)
    {
        uint8_t value = kDisabled;
        if (bus.control(true, &value)) {
            // The write may have failed on the bus after the card took it: read back anyway.
        }
        for (unsigned attempt = 0; attempt < kLeaveTries; ++attempt) {
            if (attempt) {
                bus.settle();
                ++settles;
            }
            value = kEnabled;
            if (!bus.control(false, &value) && !(value & kSdCqEnable)) return true;
        }
        stuck = true;
        return false;
    }
};
