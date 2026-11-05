// SPDX-License-Identifier: BSD-2-Clause-Patent
// On-demand CPU clock for the A57 cluster on the DFLL (SwitchThermal's governed mode, the default with switch-perf=1).
//
// What iOS does on Apple silicon (CLPC, the performance controller behind XNU's sched_perfcontrol_* callouts) and what
// Horizon does by hand with its boost mode: clocks up when there is work, down when there is none.
//   * Load: XNU's context-switch callout (sched_perfcontrol_csw_t, osfmk/arm/machine_routines_common.c
//     machine_switch_perfcontrol_context, called by thread_dispatch on every switch with interrupts masked) marks each
//     CPU busy or idle (IDLE = switching to the idle thread) and adds up its busy time, per CPU, with a sequence count
//     so another CPU can read it. No sampling, no timer of ours.
//   * Tick: XNU's perfcontrol deadline (sched_perfcontrol_update_callback_deadline, the deadline_passed callout),
//     which the scheduler checks for free in sched_timeshare_consider_maintenance: at every context switch, quantum
//     expiry and interrupt wake-up of an idle CPU. While the system runs it fires within a context switch of each
//     kCpuWindowUs; a fully idle system has no ticks and needs none (its clock was already lowered, or is lowered at
//     the first wake-up after the hold).
//   * Decision (SwitchGovernor.h, host-tested): a window's busy share is the busiest CPU's, or the sum over all CPUs
//     capped at one CPU, whichever is larger (the cluster shares one clock, and a thread that migrated between CPUs in
//     the window shows up in the sum). At or above kCpuUpPermille: the top point at once; else the lowest point whose
//     kCpuTargetPermille share covers the work; down only after kCpuDownHoldUs of lower targets.
//   * Boosts (switch_perf_hint, SwitchPower.h): a touch (SwitchTouch's interrupt), a frame that took two or more
//     refresh periods (NativeLegacyDisplay's present stamps), an exec (XNU exec_activate_image), each holding the top
//     point for a while, applied at once from the hinting context; and the boot itself, held at the top until IOKit's
//     System Boot Complete (XNU's boot band lift) or kBootFloorUs.
//   * Thermal: SwitchThermal's DFLL floor/cap indices (L4T's T210B01 trips) become the governor's lowest/highest
//     points (limits()), applied at once. The DFLL's own voltage cap stays in force underneath in hardware.
//   * iPadOS's thermal ceiling: thermalmonitord (CLTM) caps the CPU with the DVD factor it writes on ApplePMGR
//     (kits/qemu/ShengPlatformPower.cpp hands it to switch_perf_cltm_ceiling, SwitchPower.h): the highest point is
//     also at or below that share of the top clock (SwitchGovernor.h switch_gov_index_at_fraction), whichever ceiling
//     is lower. Applied at once, from thermalmonitord's thread; kept across a governor restart.
// The callouts and hints only decide; when the clock must move they wake SwitchThermal's work loop (its software
// interrupt source), which owns the DFLL and calls take()/applied(). Every callout path is a few loads and stores under
// one spin lock taken with interrupts masked (the perfcontrol rules, machine_routines.h), and never blocks.
//
// Points: the DFLL's own OPP table (L4T cvb.c from this chip's CPUB01_CVB_TABLE row, via tegra_dfll_cpu_opps), from the
// highest row at the CVB Vmin (918 MHz at 620 mV on speedo 1659; the rows below share its voltage and would run on the
// DFLL's pulse skipper) to Horizon's boost clock, 1785 MHz, never above (TegraDFLL refuses more).
//
// Periodic line (os_log only, every kLogWindowUs while the system runs): residency per point, steps, boosts, the
// window's busy share, and the DFLL's target and measured rate, to check kprof's probe ("core MHz") against.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <os/log.h>
#include "SwitchGovernor.h"
#include "SwitchPerfGovernor.h"
#include "SwitchPower.h"
#include "../TegraDFLL/TegraDFLL.h"
#include "../TegraGM20B/TegraGM20BGpu.h"     // tegra_gm20b_perf_hint, tegra_gm20b_idle_tick: the GPU's idle clock
#include "../SwitchBattery/SwitchBattery.h"  // switch_battery_avg_ma: the draw beside the clocks

namespace {
// Tunables, in microseconds (test/switch_governor_test.cpp reads these lines).
constexpr uint32_t kCpuWindowUs = 8000;       // two windows per 60 Hz frame
constexpr uint32_t kCpuUpPermille = 900;      // ondemand's up_threshold is 80-95 %
constexpr uint32_t kCpuTargetPermille = 750;  // schedutil aims at 80 % (util x 1.25)
constexpr uint32_t kCpuDownHoldUs = 80000;    // five frames
constexpr uint32_t kBoostTouchUs = 600000;    // refreshed by every touch event while a finger moves
constexpr uint32_t kBoostFrameUs = 1000000;   // after a frame that took two refresh periods or more
constexpr uint32_t kBoostLaunchUs = 1500000;  // after an exec
constexpr uint64_t kBootFloorUs = 90000000;   // the boot's floor if System Boot Complete never comes
constexpr uint64_t kLogWindowUs = 10000000;   // the periodic line, as kprof's window
constexpr unsigned kMaxCpus = 8;
constexpr unsigned kSeqTries = 4;

struct alignas(64) CpuLoad {
    uint32_t seq;        // odd while its CPU's callout writes it
    uint32_t busy;       // the CPU runs something other than its idle thread
    uint64_t since;      // when `busy` was last set (mach time)
    uint64_t busyTotal;  // busy mach time before `since`
};

CpuLoad gLoad[kMaxCpus];
IOSimpleLock *gLock;  // everything below; taken with interrupts masked
switch_gov gGov;
uint32_t gMv[SWITCH_GOV_MAX_OPPS];
uint64_t gPrevTotal[kMaxCpus], gPrevNow;
unsigned gCpus;
bool gRunning;         // atomic loads outside the lock
uint32_t gPending;     // a signal is outstanding (atomic)
uint32_t gAppliedKhz;  // atomic: what the DFLL runs
unsigned gApplied;     // its index
switchperf::Signal gSignal;
void *gCtx;
uint64_t gWindow, gBootUntil, gLogStart, gLogDue;
uint32_t gFloorMv, gCapMv;  // the DFLL's thermal floor and cap last given to limits() (0: none)
uint32_t gCltm = 0x10000u;  // thermalmonitord's CPU ceiling, a 16.16 share of the top clock (atomic loads)
unsigned gTopCap = SWITCH_GOV_MAX_OPPS - 1;  // capTop(): the top this boot measured at or under 1785 MHz
unsigned gDerate;                            // derate(): rows every ceiling sits below (a ceiling that measured high)
// per log window
uint32_t gWindows, gBusyPeak, gHints[5];
uint64_t gBusySum;

uint64_t absUs(uint64_t us)
{
    uint64_t abs = 0;
    nanoseconds_to_absolutetime(us * 1000, &abs);
    return abs;
}

// The scheduler's context-switch callout (interrupts masked, the switching CPU's own slot only).
void csw(perfcontrol_event event, uint32_t cpu, uint64_t timestamp, uint32_t, struct perfcontrol_thread_data *,
         struct perfcontrol_thread_data *, struct perfcontrol_cpu_counters *, void *)
{
    if (cpu >= kMaxCpus) return;
    CpuLoad &l = gLoad[cpu];
    __atomic_store_n(&l.seq, l.seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    if (timestamp > l.since) {
        if (l.busy) __atomic_store_n(&l.busyTotal, l.busyTotal + (timestamp - l.since), __ATOMIC_RELAXED);
        __atomic_store_n(&l.since, timestamp, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&l.busy, event == IDLE ? 0u : 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&l.seq, l.seq + 1, __ATOMIC_RELEASE);
}

// A CPU's busy time up to `now`, read from another CPU (a few tries if its callout is writing it).
uint64_t busyUpTo(unsigned cpu, uint64_t now)
{
    const CpuLoad &l = gLoad[cpu];
    uint64_t total = 0, since = 0;
    uint32_t busy = 0;
    for (unsigned i = 0; i < kSeqTries; ++i) {
        const uint32_t s1 = __atomic_load_n(&l.seq, __ATOMIC_ACQUIRE);
        total = __atomic_load_n(&l.busyTotal, __ATOMIC_RELAXED);
        since = __atomic_load_n(&l.since, __ATOMIC_RELAXED);
        busy = __atomic_load_n(&l.busy, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (!(s1 & 1) && s1 == __atomic_load_n(&l.seq, __ATOMIC_RELAXED)) break;
    }
    return total + (busy && now > since ? now - since : 0);
}

// Under gLock: one window's busy share, in permille.
uint32_t windowBusy(uint64_t now)
{
    const uint64_t span = now - gPrevNow;
    uint64_t sum = 0, peak = 0;
    for (unsigned c = 0; c < gCpus; ++c) {
        const uint64_t total = busyUpTo(c, now), done = total > gPrevTotal[c] ? total - gPrevTotal[c] : 0;
        gPrevTotal[c] = total;
        uint64_t permille = span ? done * 1000 / span : 0;
        if (permille > 1000) permille = 1000;
        sum += permille;
        if (permille > peak) peak = permille;
    }
    gPrevNow = now;
    if (sum > 1000) sum = 1000;
    return (uint32_t)(peak > sum ? peak : sum);
}

// Under gLock: whether the owner must be woken (the clock moves, or the log line is due), claiming the signal.
bool claimSignal(uint64_t now)
{
    if (gGov.cur == gApplied && now < gLogDue) return false;
    return __atomic_exchange_n(&gPending, 1u, __ATOMIC_ACQ_REL) == 0;
}

// Under gLock: the limits in force from the DFLL's thermal floor/cap and thermalmonitord's ceiling. Returns whether the
// clock must move (the signal claimed).
bool applyLimits(uint64_t now)
{
    const unsigned lo = switch_gov_index_at_mv(gMv, gGov.count, gFloorMv);
    unsigned hi = gCapMv ? switch_gov_index_at_mv(gMv, gGov.count, gCapMv) : gGov.count - 1;
    const unsigned cltm = switch_gov_index_at_fraction(gGov.khz, gGov.count, __atomic_load_n(&gCltm, __ATOMIC_RELAXED));
    if (cltm < hi) hi = cltm;
    if (gTopCap < hi) hi = gTopCap;        // the top this boot measured at or under 1785 MHz
    hi = hi > gDerate ? hi - gDerate : 0;  // every ceiling a row lower once one measured high
    switch_gov_limit(&gGov, lo, hi, now);
    return gRunning && claimSignal(now);
}

// XNU's perfcontrol deadline: the window's tick (interrupts masked; context-switch, quantum or idle-wake context).
void deadlinePassed(uint64_t)
{
    if (!__atomic_load_n(&gRunning, __ATOMIC_ACQUIRE)) return;
    const uint64_t now = mach_absolute_time();
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    bool wake = false;
    if (gRunning) {
        uint32_t busy = windowBusy(now);
        ++gWindows;
        gBusySum += busy;
        if (busy > gBusyPeak) gBusyPeak = busy;
        if (now < gBootUntil) busy = 1000;  // the boot runs at the top point
        switch_gov_step(&gGov, now, busy);
        wake = claimSignal(now);
        sched_perfcontrol_update_callback_deadline(now + gWindow);
    }
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
    if (wake) gSignal(gCtx);
    tegra_gm20b_idle_tick();  // the GPU's idle clock takes this tick too (loads; a work item once it is idle)
}

struct sched_perfcontrol_callbacks gCallbacks = {
    .version = SCHED_PERFCONTROL_CALLBACKS_VERSION_5,
    .deadline_passed = deadlinePassed,
    .csw = csw,
};

}  // namespace

bool switchperf::start(const uint32_t *khz, const uint32_t *mv, unsigned count, unsigned startIndex, Signal signal,
                       void *ctx, char *why, size_t len)
{
    if (__atomic_load_n(&gRunning, __ATOMIC_ACQUIRE)) {
        snprintf(why, len, "already running");
        return false;
    }
    if (!count || !khz || !mv || !signal) {
        snprintf(why, len, "no operating points");
        return false;
    }
    if (!gLock && !(gLock = IOSimpleLockAlloc())) {
        snprintf(why, len, "no lock");
        return false;
    }
    if (count > SWITCH_GOV_MAX_OPPS) count = SWITCH_GOV_MAX_OPPS;
    const uint64_t now = mach_absolute_time();
    gWindow = absUs(kCpuWindowUs);
    gCpus = ml_get_cpu_count();
    if (gCpus > kMaxCpus) gCpus = kMaxCpus;
    if (!gCpus) gCpus = 1;
    const switch_gov_tunables t = {kCpuUpPermille, kCpuTargetPermille, absUs(kCpuDownHoldUs)};
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    switch_gov_init(&gGov, khz, count, startIndex, t, now);
    for (unsigned i = 0; i < SWITCH_GOV_MAX_OPPS; ++i) gMv[i] = i < count ? mv[i] : 0;
    for (unsigned c = 0; c < kMaxCpus; ++c) {
        gLoad[c].seq = 0;
        gLoad[c].busy = 0;
        gLoad[c].since = now;
        gLoad[c].busyTotal = 0;
        gPrevTotal[c] = 0;
    }
    gPrevNow = now;
    gFloorMv = gCapMv = 0;
    gApplied = gGov.cur;
    gDerate = 0;  // gTopCap is the caller's (capTop before start), kept
    __atomic_store_n(&gAppliedKhz, gGov.khz[gGov.cur], __ATOMIC_RELEASE);
    gSignal = signal;
    gCtx = ctx;
    gBootUntil = now + absUs(kBootFloorUs);
    gLogStart = now;
    gLogDue = now + absUs(kLogWindowUs);
    gWindows = gBusyPeak = 0;
    gBusySum = 0;
    for (uint32_t &h : gHints) h = 0;
    __atomic_store_n(&gPending, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gRunning, true, __ATOMIC_RELEASE);
    const bool wake = applyLimits(now);  // a ceiling thermalmonitord set before this start: the clock moves down
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
    if (wake) gSignal(gCtx);
    sched_perfcontrol_register_callbacks(&gCallbacks, 0);
    sched_perfcontrol_update_callback_deadline(now + gWindow);
    return true;
}

void switchperf::stop()
{
    if (!gLock) return;
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    const bool was = gRunning;
    __atomic_store_n(&gRunning, false, __ATOMIC_RELEASE);
    __atomic_store_n(&gAppliedKhz, 0u, __ATOMIC_RELEASE);
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
    if (!was) return;
    sched_perfcontrol_update_callback_deadline(0);
    sched_perfcontrol_register_callbacks(nullptr, 0);  // XNU's defaults again
}

bool switchperf::running()
{
    return __atomic_load_n(&gRunning, __ATOMIC_ACQUIRE);
}

uint32_t switchperf::khzAt(unsigned index)
{
    return index < gGov.count ? gGov.khz[index] : 0;
}

unsigned switchperf::take(char *line, size_t len)
{
    if (line && len) line[0] = 0;
    if (!gLock) return 0;
    const uint64_t now = mach_absolute_time();
    uint64_t residency[SWITCH_GOV_MAX_OPPS], span = 0, busySum = 0;
    uint32_t hints[5], ups = 0, downs = 0, jumps = 0, boosted = 0, clamped = 0, windows = 0, peak = 0;
    unsigned count = 0, lo = 0, hi = 0;
    bool logDue = false, boot = false;
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    __atomic_store_n(&gPending, 0u, __ATOMIC_RELEASE);
    const unsigned index = gGov.cur;
    if (gRunning && now >= gLogDue) {
        logDue = true;
        switch_gov_account(&gGov, now);
        count = gGov.count;
        lo = gGov.lo;
        hi = gGov.hi;
        for (unsigned i = 0; i < SWITCH_GOV_MAX_OPPS; ++i) {
            residency[i] = gGov.residency[i];
            gGov.residency[i] = 0;
        }
        ups = gGov.ups, downs = gGov.downs, jumps = gGov.jumps, boosted = gGov.boosted, clamped = gGov.clamped;
        gGov.ups = gGov.downs = gGov.jumps = gGov.boosted = gGov.clamped = 0;
        for (unsigned i = 0; i < 5; ++i) {
            hints[i] = gHints[i];
            gHints[i] = 0;
        }
        windows = gWindows, peak = gBusyPeak, busySum = gBusySum;
        gWindows = gBusyPeak = 0;
        gBusySum = 0;
        span = now - gLogStart;
        boot = now < gBootUntil;
        gLogStart = now;
        gLogDue = now + absUs(kLogWindowUs);
    }
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
    if (!logDue || !line || !len) return index;

    uint64_t spanUs = 0;
    absolutetime_to_nanoseconds(span, &spanUs);
    spanUs /= 1000;
    size_t n = (size_t)snprintf(line, len, "SwitchPerf governor: %llu ms:", spanUs / 1000);
    for (unsigned i = 0; i < count && n < len; ++i) {
        if (!residency[i] && (i < lo || i > hi)) continue;
        const uint64_t pct = span ? residency[i] * 1000 / span : 0;
        n += (size_t)snprintf(line + n, len - n, " %u %llu.%llu%%", gGov.khz[i] / 1000, pct / 10, pct % 10);
    }
    unsigned long target = 0, measured = 0;
    unsigned vdd = 0;
    tegra_dfll_cpu_status(&target, &measured, &vdd);
    if (n < len) {
        n += (size_t)snprintf(line + n, len - n,
                              "; %u up %u down (%u jumps, %u boosted, %u clamped); busy mean %llu%% peak %u%% over %u "
                              "windows; boosts touch %u frame %u launch %u%s; range %u-%u MHz; now %u MHz (DFLL "
                              "target %lu, measured %lu MHz, VDD_CPU %u mV)",
                              ups, downs, jumps, boosted, clamped, windows ? busySum / windows / 10 : 0, peak / 10,
                              windows, hints[1], hints[2], hints[3], boot ? ", boot floor" : "", gGov.khz[lo] / 1000,
                              gGov.khz[hi] / 1000, gGov.khz[index] / 1000, target / 1000000, measured / 1000000, vdd);
    }
    bool external = false;
    const int ma = switch_battery_avg_ma(&external);
    if (ma != INT32_MIN && n < len)
        n += (size_t)snprintf(line + n, len - n, "; battery %d mA%s", ma, external ? " (external power in)" : "");
    return index;
}

void switchperf::applied(unsigned index)
{
    if (!gLock || index >= gGov.count) return;
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    gApplied = index;
    __atomic_store_n(&gAppliedKhz, gGov.khz[index], __ATOMIC_RELEASE);
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
}

void switchperf::limits(unsigned floorMv, unsigned capMv)
{
    if (!gLock || !running()) return;
    const uint64_t now = mach_absolute_time();
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    gFloorMv = floorMv;
    gCapMv = capMv;
    const bool wake = applyLimits(now);
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
    if (wake) gSignal(gCtx);
}

unsigned switchperf::indexAt(uint32_t khz)
{
    unsigned i = 0;
    while (i + 1 < gGov.count && gGov.khz[i + 1] <= khz) ++i;
    return i;
}

unsigned switchperf::ceiling()
{
    if (!gLock) return 0;
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    const unsigned hi = gGov.hi;
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
    return hi;
}

void switchperf::capTop(unsigned index)
{
    if (!gLock) {  // before the first start: nothing else runs yet
        gTopCap = index;
        return;
    }
    const uint64_t now = mach_absolute_time();
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    gTopCap = index;
    const bool wake = applyLimits(now);
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
    if (wake) gSignal(gCtx);
}

void switchperf::derate()
{
    if (!gLock || !running()) return;
    const uint64_t now = mach_absolute_time();
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    if (gDerate + 1 < gGov.count) ++gDerate;
    const bool wake = applyLimits(now);
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
    if (wake) gSignal(gCtx);
}

extern "C" unsigned int switch_perf_cltm_ceiling(uint32_t fraction16)
{
    if (fraction16 > 0x10000u) fraction16 = 0x10000u;
    __atomic_store_n(&gCltm, fraction16, __ATOMIC_RELAXED);
    if (!gLock || !__atomic_load_n(&gRunning, __ATOMIC_ACQUIRE)) return 0;
    const uint64_t now = mach_absolute_time();
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    bool wake = false;
    unsigned khz = 0;
    if (gRunning) {
        wake = applyLimits(now);
        khz = gGov.khz[gGov.hi];
    }
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
    if (wake) gSignal(gCtx);
    return khz;
}

extern "C" void switch_perf_hint(unsigned int hint)
{
    tegra_gm20b_perf_hint(hint);  // the GPU back at its full clock if it idles low
    if (!__atomic_load_n(&gRunning, __ATOMIC_ACQUIRE) || hint >= 5 || !hint) return;
    const uint64_t now = mach_absolute_time();
    const uint32_t us = hint == SWITCH_PERF_HINT_TOUCH        ? kBoostTouchUs
                        : hint == SWITCH_PERF_HINT_FRAME_MISS ? kBoostFrameUs
                                                              : kBoostLaunchUs;
    const uint64_t until = now + absUs(us);
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(gLock);
    bool wake = false;
    if (gRunning) {
        ++gHints[hint];
        if (hint == SWITCH_PERF_HINT_BOOT_DONE) {
            gBootUntil = 0;
        } else {
            switch_gov_boost(&gGov, gGov.count - 1, until, now);
            wake = gGov.cur > gApplied && claimSignal(now);
        }
    }
    IOSimpleLockUnlockEnableInterrupt(gLock, state);
    if (wake) gSignal(gCtx);
}

extern "C" unsigned int switch_perf_cpu_khz(void)
{
    return __atomic_load_n(&gAppliedKhz, __ATOMIC_ACQUIRE);
}
