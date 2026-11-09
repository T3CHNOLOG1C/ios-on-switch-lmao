// SPDX-License-Identifier: BSD-2-Clause-Patent
// PWM0 as the LCD Switches' backlight; see TegraBacklightPwm.h.
#include <IOKit/IOLib.h>
#include "TegraBacklightPwm.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr unsigned kClockPwm = 17;  // CLK_L bit 17 (hekate CLK_L_PWM)

volatile uint8_t *gPwm;

// A clock-gated or reset APB block stalls the bus: the clock is checked before every register access.
bool running()
{
    if (!gPwm)
        gPwm = switch_mmio_map(TEGRA_BACKLIGHT_PWM_PA, 0x100);
    if (!gPwm || !switch_car_clock_enabled(kClockPwm) || switch_car_reset_asserted(kClockPwm))
        return false;
    return tegra_backlight_pwm_csr_enabled(switch_mmio_read32(gPwm, TEGRA_BACKLIGHT_PWM_CSR_0)) != 0;
}
}  // namespace

extern "C" int tegra_backlight_pwm_duty(void)
{
    if (!running())
        return -1;
    return static_cast<int>(tegra_backlight_pwm_csr_duty(switch_mmio_read32(gPwm, TEGRA_BACKLIGHT_PWM_CSR_0)));
}

extern "C" int tegra_backlight_pwm_set(unsigned duty)
{
    if (duty > TEGRA_BACKLIGHT_PWM_DUTY_MAX)
        duty = TEGRA_BACKLIGHT_PWM_DUTY_MAX;
    const int now = tegra_backlight_pwm_duty();
    if (now < 0)
        return -1;
    unsigned at = static_cast<unsigned>(now);
    while (at != duty) {
        at += at < duty ? 1 : -1;
        switch_mmio_write32(gPwm, TEGRA_BACKLIGHT_PWM_CSR_0, tegra_backlight_pwm_csr(at));
        (void)switch_mmio_read32(gPwm, TEGRA_BACKLIGHT_PWM_CSR_0);
        IOSleep(1);
    }
    return 0;
}
