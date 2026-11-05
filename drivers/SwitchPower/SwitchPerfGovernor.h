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

// Starts governing over `count` points (kHz ascending, with their CVB mV), the clock now at `startIndex`, with the
// boot floor in force until SWITCH_PERF_HINT_BOOT_DONE (or kBootFloorUs). Registers the perfcontrol callouts and arms
// the first window. False (with `why`) if the table is empty or the governor already runs. Work loop only.
bool start(const uint32_t *khz, const uint32_t *mv, unsigned count, unsigned startIndex, Signal signal, void *ctx,
           char *why, size_t len);

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

// The highest index the governor may use from now on (the top this boot measured at or under 1785 MHz), on top of the
// DFLL's thermal cap and thermalmonitord's ceiling; applied at once, or kept for start() when called before it.
// derate(): every ceiling one row lower for the rest of the boot (a ceiling that measured high even when re-asked).
// ceiling(): the highest index in force now. indexAt: the highest index at or below `khz`. Work loop only.
void capTop(unsigned index);
void derate();
unsigned ceiling();
unsigned indexAt(uint32_t khz);

}  // namespace switchperf

#endif
