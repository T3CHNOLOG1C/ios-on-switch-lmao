// SPDX-License-Identifier: BSD-2-Clause-Patent
// The CPU governor's kernel side (SwitchPerfGovernor.cpp) as SwitchThermal drives it. SwitchThermal owns the DFLL and
// the thermal state; this side owns the scheduler's perfcontrol callouts, the load accounting, the hints and the
// decisions (SwitchGovernor.h). A decision that changes the clock (or a periodic log line that is due) calls the
// owner's `signal` from whatever context made it (the scheduler's callout, an interrupt filter, a thread); the owner
// then calls take() on its work loop, sets the DFLL to that point and reports it with applied().
#ifndef SWITCH_PERF_GOVERNOR_H
#define SWITCH_PERF_GOVERNOR_H

#include <stddef.h>
#include <stdint.h>

namespace switchperf {

using Signal = void (*)(void *ctx);

// The boot window's end if System Boot Complete never comes, in seconds of uptime (r8a's slow boot had it at 74 s).
constexpr unsigned kBootFallbackSeconds = 120;

// Starts governing over `count` points (kHz ascending, with their CVB mV), the clock now at `startIndex`, the rows in
// `held` held down from the start (bits: the rows above the top the DFLL measured right at enable, tried again from
// below at the first periodic window), with the boot window in force until SWITCH_PERF_HINT_BOOT_DONE (System Boot
// Complete) or kBootFallbackSeconds of uptime: the CPU at its ceiling, the GPU's idle clock held off. A boot window that has
// ended does not open again on a restart. Registers the perfcontrol callouts and arms the first window. False (with
// `why`) if the table is empty or the governor already runs. Work loop only.
bool start(const uint32_t *khz, const uint32_t *mv, unsigned count, unsigned startIndex, uint32_t held, Signal signal,
           void *ctx, char *why, size_t len);

// Stops: callouts unregistered, no signal after it returns, hints ignored. Idempotent. Work loop only.
void stop();

bool running();

// Work loop, after a signal: the index the clock should run at now. `line` receives the periodic os_log line when one
// is due ("" otherwise), and the accounting restarts.
unsigned take(char *line, size_t len);

// What the DFLL now runs (kHz of that index), for hints, kprof and the next decision's raise test.
void applied(unsigned index);

// Thermal limits in mV (the DFLL's floor and cap now in force): the lowest point is the highest row at or below the
// floor, the highest the highest row at or below the cap. Applied at once; signals if the clock must move.
void limits(unsigned floorMv, unsigned capMv);

uint32_t khzAt(unsigned index);

// Rows held down (SwitchGovernor.h switch_gov_rows), the ceiling below the DFLL's thermal cap and thermalmonitord's
// ceiling skipping them. hold(): the ceiling row `index` measured high with the clock on it even when re-asked: that row
// only is held, the ceiling the next row down at once; returns the periodic lines until it is tried again, `again`
// whether it was on trial. landed(): the ceiling row `index` measured right with the clock on it; true if it was on
// trial (now restored). retryHeld(): once per periodic line, after its measurement of the ceiling: held rows whose wait
// is over go back on trial (returned as bits), the ceiling up at once; the governor gets there from below and every
// move to the ceiling is measured. ceiling(): the highest index in force now. indexAt: the highest index at or below
// `khz`. Work loop only.
unsigned hold(unsigned index, bool *again);
bool landed(unsigned index);
uint32_t retryHeld();
unsigned ceiling();
unsigned indexAt(uint32_t khz);

// The context-switch callout's last parameter: xnu-12377 passes a timeout the callout may set, xnu-10063 nothing.
#if XNU_12377_121_6
typedef uint64_t *CswTimeout;
#else
typedef void *CswTimeout;
#endif

}  // namespace switchperf

#endif
