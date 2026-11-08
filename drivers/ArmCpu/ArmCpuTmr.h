// SPDX-License-Identifier: BSD-2-Clause-Patent
// The per-core Tegra timers TMR10-13, C7's wake source (ArmCpuIdle.cpp): the clock they count and the count they take.
#pragma once
#include <stdint.h>

namespace armcpu::tmr {
constexpr uint32_t kPtvEnable = 1U << 31;
// The count field is 29 bits in the n + 1 scheme.
constexpr uint64_t kTicksMax = UINT64_C(1) << 29;

// TMR0-9 count TIMERUS's 1 MHz; TMR10-13 count the timer block's own clock, clk_m (upstream Linux timer-tegra.c
// tegra_rate_for_timer, L4T tegra210_timer.c). TIMERUS_USEC_CFG divides clk_m down to 1 MHz by (bits 7:0 + 1) /
// (bits 15:8 + 1), so it names clk_m's rate: 0x045f is 19.2 MHz.
constexpr uint64_t clockHz(uint32_t usecConfig)
{
    return UINT64_C(1000000) * ((usecConfig & 0xffU) + 1) / (((usecConfig >> 8) & 0xffU) + 1);
}

// TMR_PTV for a wake `ticks` (1..kTicksMax) clk_m ticks from now.
constexpr uint32_t ptv(uint64_t ticks)
{
    return kPtvEnable | static_cast<uint32_t>(ticks - 1);
}
}  // namespace armcpu::tmr
