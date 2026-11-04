// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_NVRAM_H
#define SWITCH_NVRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registers the NVRAM controller with IODTNVRAM. Call once after the platform expert has configured (IODTNVRAM
// exists), before launchd. Returns 0 on success; later calls return the first result.
int ioswitch_nvram_publish(void);

// kern_exec.c load_init_program, once Data is mounted and before launchd: restores the NVRAM saved in
// /private/var/ioswitch/nvram.bin and starts saving it there. Returns how many variables were restored.
uint32_t ioswitch_nvram_data_mounted(void);

// Writes the NVRAM to the SD card now and returns once it is there (0, or the errno; 0 with nothing to write or before
// Data is mounted). Thread context, no spinlock held, SD card still up (before ioswitch_block_suspend); one 12 KiB
// write and an fsync. The driver already calls it itself on system sleep and before the unmount at shutdown.
int switch_nvram_flush(void);

#ifdef __cplusplus
}
#endif

#endif
