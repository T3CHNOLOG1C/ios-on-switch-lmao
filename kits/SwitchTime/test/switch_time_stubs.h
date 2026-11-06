// XNU's clock for kits/SwitchTime on the host: the console's 19.2 MHz timebase (625/12 ns per tick).
#ifndef SWITCH_TEST_SWITCH_TIME_STUBS_H
#define SWITCH_TEST_SWITCH_TIME_STUBS_H

#include <stdint.h>

#define NSEC_PER_USEC 1000ull
#define NSEC_PER_MSEC 1000000ull

#ifdef __cplusplus
extern "C" {
#endif

extern uint64_t gSwitchTimeNow;

static inline uint64_t mach_absolute_time(void)
{
    return gSwitchTimeNow;
}

static inline void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns)
{
    *ns = abs * 625 / 12;
}

static inline void nanoseconds_to_absolutetime(uint64_t ns, uint64_t *abs)
{
    *abs = ns * 12 / 625;
}

static inline void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = gSwitchTimeNow + (uint64_t)interval * scale * 12 / 625;
}

#ifdef __cplusplus
}
#endif

#endif
