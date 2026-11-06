// SPDX-License-Identifier: MIT
// The user side of the frame timeline (debug builds only): a kdebug event at each step of a command buffer, for ktrace, and one
// "SwitchFrameUser:" line per slow command buffer, for the pulled log archive. Times are mach ticks, the kernel's "SwitchFrame:"
// clock; tools/frames joins the two by time.
#pragma once

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#define SWITCH_FRAME_TIMELINE 1
#else
#define SWITCH_FRAME_TIMELINE 0
#endif

#if SWITCH_FRAME_TIMELINE
#include <mach/mach_time.h>
#include <os/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <atomic>

extern "C" int kdebug_trace(uint32_t debugid, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4);

namespace switch_frame {

// KDBG_CODE(DBG_APPS, 'S', code): r24-gpu-kernel's assignment, arg1 the command buffer's sequence number
enum Code : uint32_t {
    kCommit = 1,
    kRawEncode = 2,
    kReplay = 3,
    kReplayCommitted = 4,
    kOrderWait = 5,
    kFence = 6,
    kHandlersRun = 7,
    kM2vCommit = 8,
    kScheduled = 9,
    kCompleted = 10,
};

enum Phase : uint32_t {
    kPoint = 0,
    kStart = 1,
    kEnd = 2
};

inline void trace(Code code, Phase phase, uint64_t seq, uint64_t arg2 = 0)
{
    kdebug_trace(33u << 24 | uint32_t('S') << 16 | uint32_t(code) << 2 | phase, seq, arg2, 0, 0);
}

enum Field : unsigned {
    Previous,  // this thread's commit before this one
    Commit,
    Back,  // the commit call returned
    EncodeStart,
    EncodeEnd,
    ReplayStart,
    ReplayEnd,
    Returned,  // the replay's m2v commit returned
    OrderWaitStart,
    OrderWaitEnd,
    SlotStart,
    SlotEnd,
    Twins,  // ticks spent making m2v twins during the replay
    Fence,
    Done,
    FieldCount
};

struct Stamps {
    uint64_t seq = 0;
    std::atomic<uint64_t> at[FieldCount] = {};

    void set(Field f, uint64_t ticks)
    {
        at[f].store(ticks, std::memory_order_relaxed);
    }

    uint64_t mark(Field f)
    {
        const uint64_t t = mach_absolute_time();
        set(f, t);
        return t;
    }

    uint64_t get(Field f) const
    {
        return at[f].load(std::memory_order_relaxed);
    }
};

inline uint64_t ticksPerMs()
{
    static const uint64_t perMs = [] {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        return tb.numer ? 1000000ull * tb.denom / tb.numer : 24000ull;
    }();
    return perMs;
}

// At the commit call's entry: the command buffer's sequence number and the stamps that start it. `lastCommit` is the caller's
// own per-thread stamp: the raw facade's and the m2v frontend's commits interleave on one thread when a replay commits.
inline void committed(Stamps &s, uint64_t &lastCommit)
{
    static std::atomic<uint64_t> sequence{0};
    s.seq = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t t = s.mark(Commit);
    s.set(Previous, lastCommit);
    lastCommit = t;
    trace(kCommit, kPoint, s.seq);
}

// Slow: a commit 20-250 ms after this thread's previous one (past 250 ms the thread was idle), a commit call over 8 ms, or a
// replay on m2v
inline bool slow(const Stamps &s)
{
    const uint64_t ms = ticksPerMs(), commit = s.get(Commit), previous = s.get(Previous), back = s.get(Back);
    const uint64_t gap = previous && commit > previous ? commit - previous : 0;
    return (gap > 20 * ms && gap < 250 * ms) || (back > commit && back - commit > 8 * ms) || s.get(ReplayStart);
}

// After the completed handlers ran: the kdebug event, and the line when the command buffer was slow (at most 20 a second,
// the ones over that counted into the next line's "dropped")
inline void finished(Stamps &s)
{
    s.mark(Done);
    trace(kHandlersRun, kPoint, s.seq);
    if (!slow(s))
        return;
    static std::atomic<uint64_t> windowStart{0};
    static std::atomic<uint32_t> inWindow{0}, dropped{0};
    const uint64_t now = s.get(Done);
    uint64_t start = windowStart.load(std::memory_order_relaxed);
    if (now - start > 1000 * ticksPerMs() && windowStart.compare_exchange_strong(start, now, std::memory_order_relaxed))
        inWindow.store(0, std::memory_order_relaxed);
    if (inWindow.fetch_add(1, std::memory_order_relaxed) >= 20) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    static os_log_t log = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "frame");
    auto g = [&s](Field f) {
        return (unsigned long long)s.get(f);
    };
    os_log(
        log,
        "SwitchFrameUser: %{public}s cb %llu commit %llu enc %llu %llu replay %llu %llu ret %llu owait %llu %llu fence "
        "%llu done %llu back %llu slot %llu %llu twins %llu prev %llu dropped %u",
        getprogname(), (unsigned long long)s.seq, g(Commit), g(EncodeStart), g(EncodeEnd), g(ReplayStart), g(ReplayEnd),
        g(Returned), g(OrderWaitStart), g(OrderWaitEnd), g(Fence), g(Done), g(Back), g(SlotStart), g(SlotEnd), g(Twins),
        g(Previous), dropped.exchange(0, std::memory_order_relaxed));
}

}  // namespace switch_frame
#endif
