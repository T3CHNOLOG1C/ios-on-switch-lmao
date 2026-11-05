#ifndef SWITCH_SD_DIAGNOSTIC_H
#define SWITCH_SD_DIAGNOSTIC_H
#include <stdint.h>
/* Configuration only: early console setup precedes CPU registration. */
static inline int switch_native_cpu_profile_matches(int cpus, int root_boot, int sheng_smp, int native_smp)
{
    return cpus == 1 || (cpus == 4 && root_boot == 1 && sheng_smp == 1 && native_smp == 1);
}
/* One-shot diagnostic only; no filesystem or general block-driver interface. */
void switch_sd_probe(void);
extern volatile uint64_t switch_sd_record[33];
extern uint8_t switch_sd_bytes[512];
extern uint8_t switch_sd_reference_bytes[512];
#define IOSW_SD_DIAG_MAGIC UINT64_C(0x4953575344524431)
#endif
