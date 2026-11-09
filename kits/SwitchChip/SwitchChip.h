// SPDX-License-Identifier: BSD-2-Clause-Patent
// Which Tegra X1 this is: T210 (Erista, the 2017 Switch) or T210B01 (Mariko: the 2019 Switch, Lite and OLED), from
// APB_MISC_GP_HIDREV (L4T tegra-apbmisc.c: chip id bits 15:8 = 0x21 for both, major revision bits 7:4 = 1 / 2).
// Both SoCs run the same kernel; the blocks only the B01 has (SE2, the BPMP EMC clock, ...) ask here.
#ifndef SWITCH_CHIP_H
#define SWITCH_CHIP_H
#include <stdbool.h>
#include <stdint.h>

#define SWITCH_APB_MISC_PA UINT64_C(0x70000800)
#define SWITCH_HIDREV_OFFSET 0x4u
#define SWITCH_HIDREV_CHIPID_T210 0x21u
#define SWITCH_HIDREV_MAJOR_T210 1u
#define SWITCH_HIDREV_MAJOR_T210B01 2u

static inline uint32_t switch_hidrev_chipid(uint32_t hidrev)
{
    return (hidrev >> 8) & 0xffu;
}

static inline uint32_t switch_hidrev_major(uint32_t hidrev)
{
    return (hidrev >> 4) & 0xfu;
}

static inline bool switch_hidrev_is_t210b01(uint32_t hidrev)
{
    return switch_hidrev_chipid(hidrev) == SWITCH_HIDREV_CHIPID_T210 &&
           switch_hidrev_major(hidrev) == SWITCH_HIDREV_MAJOR_T210B01;
}

#ifdef KERNEL
#include "../SwitchMMIO/SwitchMMIOKernel.h"

static inline uint32_t switch_chip_hidrev(void)
{
    const volatile uint8_t *apbmisc = switch_mmio_map(SWITCH_APB_MISC_PA, 0x1000);
    return apbmisc ? switch_mmio_read32(apbmisc, SWITCH_HIDREV_OFFSET) : 0;
}

static inline bool switch_chip_is_t210b01(void)
{
    return switch_hidrev_is_t210b01(switch_chip_hidrev());
}
#endif
#endif
