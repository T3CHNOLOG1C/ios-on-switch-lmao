// SPDX-License-Identifier: BSD-2-Clause-Patent
// The LCD Switches' backlight (every panel but the OLED's AMS699VC01): a white-LED driver fed by PWM0 on pin LCD_BL_PWM
// (hekate di.c display_backlight_pwm_init: PWM clock on, CSR_0 enabled at 25 kHz PFM, the pin in PWM0 mode). Brightness
// is the duty in PWM_CSR_0 bits 23:16, 0..255, changed one step a millisecond as hekate ramps it
// (_display_pwm_backlight_brightness). The boot chain leaves the PWM running at its own level; LCD_BL_EN (PV1) stays
// high throughout, as hekate's display_end leaves it: off is duty 0.
#ifndef TEGRA_BACKLIGHT_PWM_H
#define TEGRA_BACKLIGHT_PWM_H
#include <stdint.h>

#define TEGRA_BACKLIGHT_PWM_PA 0x7000a000u
#define TEGRA_BACKLIGHT_PWM_CSR_0 0x00u
#define TEGRA_BACKLIGHT_PWM_DUTY_MAX 255u

static inline uint32_t tegra_backlight_pwm_csr(unsigned duty)
{
    return (1u << 31) | ((duty > TEGRA_BACKLIGHT_PWM_DUTY_MAX ? TEGRA_BACKLIGHT_PWM_DUTY_MAX : duty) << 16);
}

static inline unsigned tegra_backlight_pwm_csr_duty(uint32_t csr)
{
    return (csr >> 16) & 0xffu;
}

static inline int tegra_backlight_pwm_csr_enabled(uint32_t csr)
{
    return (csr >> 31) & 1u;
}

// TegraPanel's 0..2047 level to a duty and back, rounded to nearest so a level survives the round trip.
static inline unsigned tegra_backlight_pwm_duty_from_level(unsigned level, unsigned level_max)
{
    if (level > level_max)
        level = level_max;
    return (level * TEGRA_BACKLIGHT_PWM_DUTY_MAX + level_max / 2) / level_max;
}

static inline unsigned tegra_backlight_pwm_level_from_duty(unsigned duty, unsigned level_max)
{
    if (duty > TEGRA_BACKLIGHT_PWM_DUTY_MAX)
        duty = TEGRA_BACKLIGHT_PWM_DUTY_MAX;
    return (duty * level_max + TEGRA_BACKLIGHT_PWM_DUTY_MAX / 2) / TEGRA_BACKLIGHT_PWM_DUTY_MAX;
}

#ifdef KERNEL
#ifdef __cplusplus
extern "C" {
#endif
// The duty the PWM runs at, 0..255, or -1 when the PWM block is clock-gated, in reset or not enabled (then the boot
// chain did not set the backlight up: an OLED, or a boot without hekate's display init). Thread context.
int tegra_backlight_pwm_duty(void);

// Ramps the duty to `duty`, a step a millisecond, as hekate does; 0, or -1 when the PWM is not running. Thread
// context; sleeps up to 255 ms.
int tegra_backlight_pwm_set(unsigned duty);
#ifdef __cplusplus
}
#endif
#endif
#endif
