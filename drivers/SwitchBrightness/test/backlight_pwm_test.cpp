// TegraBacklightPwm.h on the Mac: PWM_CSR_0 as hekate writes it (PWM_CSR_EN | duty << 16) and TegraPanel's 0..2047
// level against the 0..255 duty, both ways.
#include <gtest/gtest.h>

#include "../TegraBacklightPwm.h"

namespace {
constexpr unsigned kLevelMax = 2047;  // TEGRA_PANEL_BRIGHTNESS_MAX

TEST(BacklightPwm, CsrIsHekatesEnableAndDuty)
{
    EXPECT_EQ(tegra_backlight_pwm_csr(0), 0x80000000u);
    EXPECT_EQ(tegra_backlight_pwm_csr(100), 0x80640000u);
    EXPECT_EQ(tegra_backlight_pwm_csr(255), 0x80FF0000u);
    EXPECT_EQ(tegra_backlight_pwm_csr(300), 0x80FF0000u);
    EXPECT_EQ(tegra_backlight_pwm_csr_duty(0x80640000u), 100u);
    EXPECT_EQ(tegra_backlight_pwm_csr_enabled(0x80640000u), 1);
    EXPECT_EQ(tegra_backlight_pwm_csr_enabled(0x00640000u), 0);
}

TEST(BacklightPwm, LevelEndsMapToDutyEnds)
{
    EXPECT_EQ(tegra_backlight_pwm_duty_from_level(0, kLevelMax), 0u);
    EXPECT_EQ(tegra_backlight_pwm_duty_from_level(kLevelMax, kLevelMax), 255u);
    EXPECT_EQ(tegra_backlight_pwm_duty_from_level(kLevelMax + 10, kLevelMax), 255u);
    EXPECT_EQ(tegra_backlight_pwm_level_from_duty(0, kLevelMax), 0u);
    EXPECT_EQ(tegra_backlight_pwm_level_from_duty(255, kLevelMax), kLevelMax);
    EXPECT_EQ(tegra_backlight_pwm_level_from_duty(255 + 10, kLevelMax), kLevelMax);
}

TEST(BacklightPwm, EveryDutySurvivesTheRoundTrip)
{
    for (unsigned duty = 0; duty <= 255; ++duty) {
        const unsigned level = tegra_backlight_pwm_level_from_duty(duty, kLevelMax);
        EXPECT_EQ(tegra_backlight_pwm_duty_from_level(level, kLevelMax), duty) << "duty " << duty;
    }
}

TEST(BacklightPwm, LevelsAreMonotonicInDuty)
{
    unsigned last = 0;
    for (unsigned level = 0; level <= kLevelMax; ++level) {
        const unsigned duty = tegra_backlight_pwm_duty_from_level(level, kLevelMax);
        EXPECT_GE(duty, last);
        last = duty;
    }
}
}  // namespace
