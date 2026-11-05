// SPDX-License-Identifier: BSD-2-Clause-Patent
// BH1730FVC lux and gain steps for SwitchALS.cpp, header-only so the host check (test_als.py) runs this exact code
// against a port of Linux's.
//
// Calibration: switchroot L4T tegra210-nx-sensor.dtsi bh1730fvc@29 (rohm,integration-cycle 38, rohm,lux-multiplier 3600)
// with the OLED's rohm,opt-win-coeff and rohm,gain-coeff from tegra210b01-fric.dts. Maths: CTCaer/switch-l4t-kernel-4.9
// drivers/iio/light/bh1730.c (GPL-2.0, Copyright (c) 2021 CTCaer) bh1730_get_lux and bh1730_adjust_gain, re-expressed:
//   * lux = (cv * DATA0 - ci * DATA1) * 100 ms * multiplier / (gain * integration time us * 1000), one division instead
//     of Linux's three (its first truncation cost up to 15% below 30 lux); (cv, ci) from the first optical-window row
//     whose rc exceeds 1000 * DATA1 / DATA0, none (0 lux) past the last. Never below Linux's value, at most 4 above.
//   * Gain: down when the larger channel passes the gain's fl, up when it falls under its cl, except that a step up is
//     taken only if the reading stays under the next gain's fl (the DT's 2x cl 1600 is 51200 at 64x, over its fl 50000,
//     which would flip between the two every sample).
// hekate bdk/input/als.c has an Aula table too (row 0 ci 30300, multiplier 1410) that its code never selects; the DT is
// what L4T and Android use on this console. Nothing here corrects for the panel's own light reaching the sensor.
#ifndef SWITCH_ALS_MATH_H
#define SWITCH_ALS_MATH_H
#include <stdint.h>

namespace switch_als {
constexpr unsigned kCycles = 38;                     // rohm,integration-cycle; TIMING = 256 - cycles (datasheet 0xda)
constexpr unsigned kIntegrationUS = 2700 * kCycles;  // bh1730.c BH1730_ITIME_TO_US: 102.6 ms (+ 2 ms ADC)
constexpr unsigned kLuxMultiplier = 3600, kLuxMax = 100000;
constexpr unsigned kGains = 4;
constexpr unsigned kGainFactor[kGains] = {1, 2, 64, 128};  // GAIN register 0..3

struct GainLimits {
    uint32_t cl, fl;
};
constexpr GainLimits kGainLimits[kGains] = {{2500, 0}, {1600, 7800}, {10, 50000}, {0, 1000}};  // tegra210b01-fric.dts

struct Window {
    uint32_t rc, cv, ci;
};
constexpr Window kWindow[] = {{231, 9697, 7502}, {993, 3333, 2778}, {1478, 1621, 1053}, {7500, 81, 10}};

inline uint32_t lux(uint32_t visible, uint32_t ir, unsigned gain)
{
    if (!visible || gain >= kGains) return 0;
    int64_t weighted = 0;
    for (const Window &w : kWindow) {
        if (1000ull * ir / visible < w.rc) {
            weighted = (int64_t)w.cv * visible - (int64_t)w.ci * ir;
            break;
        }
    }
    if (weighted <= 0) return 0;  // cannot happen inside a row's range; bh1730.c would wrap in u64
    const uint64_t value =
        (uint64_t)weighted * 100 * kLuxMultiplier / ((uint64_t)kGainFactor[gain] * kIntegrationUS * 1000);
    return value > kLuxMax ? kLuxMax : (uint32_t)value;
}

// The gain for the next conversion after one at `gain` read these channels.
inline unsigned nextGain(unsigned gain, uint32_t visible, uint32_t ir)
{
    const uint32_t highest = visible > ir ? visible : ir;
    if (gain > 0 && highest > kGainLimits[gain].fl) return gain - 1;
    if (gain + 1 < kGains && highest < kGainLimits[gain].cl &&
        (uint64_t)highest * kGainFactor[gain + 1] / kGainFactor[gain] < kGainLimits[gain + 1].fl) {
        return gain + 1;
    }
    return gain;
}
}  // namespace switch_als
#endif
