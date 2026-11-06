// SPDX-License-Identifier: GPL-2.0-only
// SOR1 (0x54580000) as a DisplayPort source for display controller B, and the clocks behind it (PLL_D2 for the pixel
// clock, the SOR1 and DISP2 clock sources). Synchronous steps for SwitchDisplayPort's work loop, in Linux's order
// (drivers/gpu/drm/tegra/sor.c tegra_sor_dp_enable/_disable): powerUp, pixelClock, startLink, drive (once per training
// step), attach, then display controller B enables the SOR, then wake. The sequencer handshakes they wait on have no
// interrupt and finish in microseconds; each wait is bounded (10 ms), and one that runs out is a hardware fault: one
// line, and DisplayPort off.
#ifndef TEGRA_SOR1_H
#define TEGRA_SOR1_H

#include <stddef.h>
#include <stdint.h>

#include "SwitchDPLink.h"
#include "SwitchDPModes.h"
#include "SwitchDPTraining.h"

class TegraSor1 {
public:
    // vdd-hdmi-dp (MAX77620 LDO8, 1.0 V), the pads out of deep power down, the SOR power partition, SOR1's clock on
    // the safe source and out of reset.
    bool powerUp(char *why, size_t size);
    // PLL_D2 at twice `pixel_khz`; pll_d2_out0 then clocks DISP2 and SOR1's pixel side.
    bool pixelClock(uint32_t pixel_khz, char *why, size_t size);
    // SOR1's PLL, DP clocks, crossbar and protocol, termination calibration, link rate and lanes, lanes powered and
    // precharged: ready for the first training pattern.
    bool startLink(const struct switch_dp_link &link, bool enhanced, char *why, size_t size);
    void drive(const struct switch_dp_train &train, const struct switch_dp_train_out &out);
    // After training: transfer units, the head's timing, power, attach to display controller B.
    bool attach(const struct switch_dp_mode &mode, const struct switch_dp_tu &tu, char *why, size_t size);
    // After display controller B has SOR1 enabled: the head awake.
    bool wake(char *why, size_t size);
    // Teardown, around display controller B dropping SOR1: detachHead, then B lets go, then powerDown.
    void detachHead();
    void powerDown();

private:
    volatile uint8_t *base = nullptr;
    uint8_t lanes = 0;
    uint32_t read(uint32_t word) const;
    void write(uint32_t word, uint32_t value);
    void rmw(uint32_t word, uint32_t clear, uint32_t set);
    bool waitClear(uint32_t word, uint32_t bits, uint32_t us);
    void superUpdate();
    void update();
    bool lanesPower(uint8_t count, bool up);
    void terminationCalibrate();
};

#endif
