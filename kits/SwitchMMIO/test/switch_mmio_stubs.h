// Host stand-ins for SwitchMMIOKernel.h: a clock the test advances (each look at it costs one tick) and ml_io_map.
#ifndef SWITCH_TEST_SWITCH_MMIO_STUBS_H
#define SWITCH_TEST_SWITCH_MMIO_STUBS_H

#include <stddef.h>
#include <stdint.h>

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
