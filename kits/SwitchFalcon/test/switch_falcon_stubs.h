// Host stand-ins for what kits/SwitchFalcon/SwitchFalcon.cpp takes from the kernel: the clock and IOLib's snprintf.
#ifndef SWITCH_TEST_SWITCH_FALCON_STUBS_H
#define SWITCH_TEST_SWITCH_FALCON_STUBS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NSEC_PER_USEC 1000u
uint64_t mach_absolute_time(void);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result);
uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t size);

#ifdef __cplusplus
}
#endif

#endif
