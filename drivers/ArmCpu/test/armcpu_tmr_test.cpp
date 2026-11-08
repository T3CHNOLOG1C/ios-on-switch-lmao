// ArmCpuTmr.h: the count C7 programs for a deadline, played out on a TMR10-13 that counts clk_m.
#include "drivers/ArmCpu/ArmCpuTmr.h"
#include "kits/SwitchIdle/switch_idle_governor.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

constexpr uint64_t kTimebaseHz = 19'200'000;
constexpr uint32_t kSwitchUsecConfig = 0x045f;
constexpr uint64_t kMs = SWITCH_IDLE_TICKS_US(1000);

// When a one-shot TMR loaded with `ptv` fires, in nanoseconds, counting at `hz`.
uint64_t firesAfterNs(uint32_t ptv, uint64_t hz)
{
    return ((ptv & 0x1fffffffU) + UINT64_C(1)) * 1'000'000'000 / hz;
}

TEST(ArmCpuTmr, TimerusConfigNamesClkM)
{
    // Linux timer-tegra.c's table: each clk_m rate and the TIMERUS_USEC_CFG it writes for it.
    EXPECT_EQ(armcpu::tmr::clockHz(0x000b), 12'000'000u);
    EXPECT_EQ(armcpu::tmr::clockHz(0x043f), 12'800'000u);
    EXPECT_EQ(armcpu::tmr::clockHz(0x000c), 13'000'000u);
    EXPECT_EQ(armcpu::tmr::clockHz(0x0453), 16'800'000u);
    EXPECT_EQ(armcpu::tmr::clockHz(0x0019), 26'000'000u);
    EXPECT_EQ(armcpu::tmr::clockHz(0x04bf), 38'400'000u);
    EXPECT_EQ(armcpu::tmr::clockHz(0x002f), 48'000'000u);
    EXPECT_EQ(armcpu::tmr::clockHz(kSwitchUsecConfig), kTimebaseHz);
}

TEST(ArmCpuTmr, TheWakeFiresAtTheGovernorsWakeTime)
{
    const uint64_t clkM = armcpu::tmr::clockHz(kSwitchUsecConfig);
    for (const uint64_t deadline : {SWITCH_IDLE_C7_RESIDENCY, 5 * kMs, 100 * kMs, 27'000 * kMs}) {
        const uint64_t now = 77 * kMs;
        const uint64_t wakeAt = switch_idle_c7_wake_at(now + deadline);
        const uint64_t ticks = switch_idle_timer_ticks(now, wakeAt, armcpu::tmr::kTicksMax);
        const uint32_t ptv = armcpu::tmr::ptv(ticks);
        EXPECT_TRUE(ptv & armcpu::tmr::kPtvEnable);
        EXPECT_EQ(firesAfterNs(ptv, clkM), (wakeAt - now) * 1'000'000'000 / kTimebaseHz) << deadline;
    }
}

TEST(ArmCpuTmr, OneMillisecondIsOneMillisecond)
{
    const uint32_t ptv = armcpu::tmr::ptv(kMs);
    EXPECT_EQ(ptv, 0x80004affu);
    EXPECT_EQ(firesAfterNs(ptv, armcpu::tmr::clockHz(kSwitchUsecConfig)), 1'000'000u);
}

TEST(ArmCpuTmr, TheLongestWakeFillsTheCountField)
{
    EXPECT_EQ(armcpu::tmr::ptv(armcpu::tmr::kTicksMax), 0x9fffffffu);
    EXPECT_EQ(armcpu::tmr::ptv(1), armcpu::tmr::kPtvEnable);
    EXPECT_EQ(firesAfterNs(armcpu::tmr::ptv(armcpu::tmr::kTicksMax), kTimebaseHz), 27'962'026'666u);
}

}  // namespace
