// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_NVRAM_H
#define SWITCH_NVRAM_H

#ifdef __cplusplus
extern "C" {
#endif

// Registers the RAM-backed IONVRAMController with IODTNVRAM. Call once after
// the platform expert has configured (IODTNVRAM exists), before launchd.
// Returns 0 on success; later calls return the first result.
int ioswitch_nvram_publish(void);

#ifdef __cplusplus
}
#endif

#endif
