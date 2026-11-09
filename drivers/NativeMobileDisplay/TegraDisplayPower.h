// SPDX-License-Identifier: BSD-2-Clause-Patent
// The built-in display across SC7, where the SoC rail is off and every display controller, DSI, MIPI-cal and clock register is
// lost: the display's suspend and resume hooks for the sleep code, and the cold power-up/power-down of the Switch's display
// pipeline they use (DSI A, MIPI pad calibration, display controller A's base configuration, the panel). The sequences are
// hekate's (bdk/display/di.c display_init and _display_panel_and_hw_end, register tables in di.inl, clock_enable_plld in
// bdk/soc/clock.c) with its T210 / T210B01 branches and its panel branches: the OLED's AMS699VC01 and the LCD Switches' JDI,
// InnoLux, AUO and Sharp panels.
#ifndef TEGRA_DISPLAY_POWER_H
#define TEGRA_DISPLAY_POWER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// The hooks (NativeLegacyDisplay.cpp). Thread context; they sleep; never from the display's work loop or with its gate held
// (the panel's DCS commands wait for the frame ends that loop delivers). Call them with SwitchBacklight idle, i.e. no display
// power request (display power) in flight. Each one panics with one line if the display never started, if it lacks the DC A
// interrupt, if SwitchBrightness is not linked, or if the panel is not one hekate knows (tegra_display_panel_supported).
//
// tegra_display_suspend: if the panel is lit, TegraPanel's power-off (brightness 0, display off, sleep in; what display power off
// does); DC A stopped with its interrupts masked and window A's registers kept; then tegra_display_power_down(). About 0.45 s lit.
// tegra_display_resume: tegra_display_power_up(), window A back as it was (single or double buffered, rotated or not), the DC A
// interrupt re-armed and, if the panel was lit at suspend, the scanout restarted and the panel woken with TegraPanel's power-on
// (the last brightness). About 0.4 s, plus 0.4 s for a lit panel. Needs back first: PLLP, the GPIO controller, and the GIC's
// configuration of INTID 105 (DC A, level high) and of the GPIO banks. A panel that was off (display power) stays off, the
// scanout stopped, exactly as display power left it; the next display power-on request lights it.
void tegra_display_suspend(void);
void tegra_display_resume(void);

// For the hooks only. hekate's decoded panel ID (bdk/display/di.h PANEL_*), which its L4T launcher leaves in PMC
// SECURE_SCRATCH113 bits 15:0 (bootloader/l4t/l4t.c); the PMC keeps it across SC7.
#define TEGRA_DISPLAY_PANEL_JDI_XXX062M 0x10
#define TEGRA_DISPLAY_PANEL_INL_P062CCA_AZ1 0x0F20
#define TEGRA_DISPLAY_PANEL_AUO_A062TAN01 0x0F30
#define TEGRA_DISPLAY_PANEL_INL_2J055IA_27A 0x1020
#define TEGRA_DISPLAY_PANEL_AUO_A055TAN01 0x1030
#define TEGRA_DISPLAY_PANEL_SHP_LQ055T1SW10 0x1040
#define TEGRA_DISPLAY_PANEL_AMS699VC01 0x2050
uint32_t tegra_display_panel_id(void);

static inline int tegra_display_panel_supported(uint32_t panel)
{
    return panel == TEGRA_DISPLAY_PANEL_AMS699VC01 || panel == TEGRA_DISPLAY_PANEL_JDI_XXX062M ||
           panel == TEGRA_DISPLAY_PANEL_INL_P062CCA_AZ1 || panel == TEGRA_DISPLAY_PANEL_AUO_A062TAN01 ||
           panel == TEGRA_DISPLAY_PANEL_INL_2J055IA_27A || panel == TEGRA_DISPLAY_PANEL_AUO_A055TAN01 ||
           panel == TEGRA_DISPLAY_PANEL_SHP_LQ055T1SW10;
}

// After the panel is asleep and DC A has stopped at a frame end: DC A unpowered with its interrupts off, DSI disabled with its
// pads powered down, panel reset asserted, MIPI-cal, DSI and DISP1 clocks gated and in reset. The MAX77620 LDO0 (DSI AVDD
// 1.2 V) has no FPS slot on this board (tegra210b01-nx-pmic.dtsi: active-fps-source NONE), so it stays as software leaves it
// across SC7; it is left on (L4T turns it off), which keeps PMIC I2C out of both directions.
void tegra_display_power_down(void);

// From cold (after SC7, or after tegra_display_power_down): HOST1X if gated, display clocks, DSI pads out of deep power down,
// PLLD, DC A's base configuration, DSI init, panel reset and the panel's LP init (hekate's sequence for the panel: the AMS699VC01's
// sleep out, color mode, brightness control, ELVSS, brightness 0; an LCD's level-2 registers and sleep out; on the LCD Switches
// the AVDD and WLED pins first), then sleep in again instead of display on, so the panel ends up as TegraPanel's
// power-off leaves it; DSI in HS video mode, MIPI pads calibrated, DC A timings set. DC A is left stopped with every window
// disabled and no interrupt enabled: the caller programs window A, restarts the scanout and wakes the panel.
void tegra_display_power_up(void);

#ifdef __cplusplus
}
#endif
#endif
