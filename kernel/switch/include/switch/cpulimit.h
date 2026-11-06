// SPDX-License-Identifier: GPL-2.0-only
/*
 * Blocking per-thread CPU limits (thread_set_cpulimit's THREAD_CPULIMIT_BLOCK: proc_setthread_cpupercent, a workloop's
 * cpupercent) are a share of the core iOS was tuned on. This identity's A10X core gets through about 2.5 times the work
 * of a Cortex-A57 at 1785 MHz in the same time, so the same work budget is 2.5 times the share here. A share of the
 * whole core or more cannot be exceeded and is not applied. Apple's own shares for SpringBoard's main thread and Core
 * Animation's display thread run out here during ordinary frames, and each time the thread sleeps to the refill (about
 * 2.4 ms) in the middle of the frame.
 * Plain C, without kernel types, so kernel/tests runs it on the host.
 */
#ifndef SWITCH_CPULIMIT_H
#define SWITCH_CPULIMIT_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_CPULIMIT_SCALE_NUM 5u
#define SWITCH_CPULIMIT_SCALE_DEN 2u

/* Scales *percentage to the share of a core here. True when that is the whole core: there is nothing to limit. */
static inline bool switch_cpulimit_lift(uint8_t *percentage)
{
    unsigned int share = ((unsigned int)*percentage * SWITCH_CPULIMIT_SCALE_NUM + SWITCH_CPULIMIT_SCALE_DEN - 1) /
                         SWITCH_CPULIMIT_SCALE_DEN;

    if (share >= 100) {
        return true;
    }
    *percentage = (uint8_t)share;
    return false;
}

#endif /* SWITCH_CPULIMIT_H */
