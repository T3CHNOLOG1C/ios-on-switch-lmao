// SPDX-License-Identifier: GPL-2.0-only
// The link for a mode, SOR1's transfer-unit configuration for it, and PLL_D2's dividers for its pixel clock. Plain C
// for the host tests.
//
// switch_dp_tu_config is Linux drivers/gpu/drm/tegra/sor.c tegra_sor_compute_params/_config (Copyright (C) 2013
// NVIDIA Corporation, GPL-2.0-only). PLL_D2: drivers/clk/tegra/clk-tegra210.c (pll_d2_params: PLLSS, VCO 750-1500 MHz,
// the qlin post divider, SDM) and clk-pll.c (Fvco = Fref * (N + 1/2 + SDM_DIN / 8192) / M).
#ifndef SWITCH_DP_LINK_H
#define SWITCH_DP_LINK_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_DP_RATE_RBR  0x06u
#define SWITCH_DP_RATE_HBR  0x0Au
#define SWITCH_DP_RATE_HBR2 0x14u

// The link symbol clock (kHz) of a DPCD link rate code: 27 MHz units.
static inline uint32_t switch_dp_rate_khz(uint8_t rate)
{
    return rate * 27000u;
}

// Video payload of a link (kbit/s): one byte per symbol per lane after 8b/10b.
static inline uint64_t switch_dp_payload_kbps(uint8_t rate, uint8_t lanes)
{
    return (uint64_t)switch_dp_rate_khz(rate) * 8u * lanes;
}

// The fastest rate SOR1 and the sink share: Tegra drives RBR, HBR and HBR2.
static inline uint8_t switch_dp_rate_cap(uint8_t sink_max_rate)
{
    if (sink_max_rate >= SWITCH_DP_RATE_HBR2) {
        return SWITCH_DP_RATE_HBR2;
    }
    return sink_max_rate >= SWITCH_DP_RATE_HBR ? SWITCH_DP_RATE_HBR : SWITCH_DP_RATE_RBR;
}

static inline uint8_t switch_dp_rate_below(uint8_t rate)
{
    if (rate > SWITCH_DP_RATE_HBR) {
        return SWITCH_DP_RATE_HBR;
    }
    return rate > SWITCH_DP_RATE_RBR ? SWITCH_DP_RATE_RBR : 0;
}

struct switch_dp_link {
    uint8_t rate;
    uint8_t lanes;
};

// The slowest link that carries `pixel_khz` at 24 bpp: the lowest rate up to `max_rate`, then the fewest of 1, 2 or 4
// lanes up to `max_lanes`. False when none does.
static inline bool switch_dp_link_for(uint32_t pixel_khz, uint8_t max_rate, uint8_t max_lanes,
                                      struct switch_dp_link *link)
{
    static const uint8_t rates[] = {SWITCH_DP_RATE_RBR, SWITCH_DP_RATE_HBR, SWITCH_DP_RATE_HBR2};
    static const uint8_t lane_counts[] = {1, 2, 4};
    const uint64_t need = (uint64_t)pixel_khz * 24u;
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned l = 0; l < 3 && rates[r] <= max_rate; ++l) {
            if (lane_counts[l] <= max_lanes && need < switch_dp_payload_kbps(rates[r], lane_counts[l])) {
                link->rate = rates[r];
                link->lanes = lane_counts[l];
                return true;
            }
        }
    }
    return false;
}

struct switch_dp_tu {
    uint32_t tu_size, active_count, active_frac, active_polarity, watermark;
    uint32_t hblank_symbols, vblank_symbols;
};

struct switch_dp_tu_params {
    uint32_t num_clocks;
    uint64_t ratio, precision;
    uint32_t active_polarity, active_count, active_frac, tu_size;
    uint64_t error;
};

static inline bool switch_dp_tu_try(struct switch_dp_tu_params *params, uint32_t tu_size)
{
    const uint64_t f = params->precision;
    const uint64_t active_sym = params->ratio * tu_size;
    const uint64_t active_count = active_sym / f * f;
    uint64_t frac = active_sym - active_count;
    uint32_t active_polarity = 0, active_frac = 0;
    if (frac >= f / 2) {
        active_polarity = 1;
        frac = f - frac;
    }
    if (frac != 0) {
        frac = f * f / frac;
        if (frac <= 15 * f) {
            active_frac = (uint32_t)(frac / f);
            if (active_polarity != 0) {
                ++active_frac;
            }
        } else {
            active_frac = active_polarity != 0 ? 1 : 15;
        }
    }
    if (active_frac == 1) {
        active_polarity = 0;
    }
    uint64_t approx = active_count;
    if (active_polarity == 1) {
        approx = active_frac != 0 ? (active_count + (active_frac * (f - 1)) * f) / (active_frac * f) : active_count + f;
    } else if (active_frac != 0) {
        approx = active_count + f / active_frac;
    }
    const int64_t error = ((int64_t)active_sym - (int64_t)approx) / (int64_t)tu_size * (int64_t)params->num_clocks;
    const uint64_t magnitude = (uint64_t)(error < 0 ? -error : error);
    if (error <= 0 && magnitude < params->error) {
        params->active_count = (uint32_t)(active_count / f);
        params->active_polarity = active_polarity;
        params->active_frac = active_frac;
        params->error = magnitude;
        params->tu_size = tu_size;
        return error == 0;
    }
    return false;
}

// SOR1's DP_LINKCTL/DP_CONFIG/blanking symbols for a mode over a link. False when the link cannot carry the mode.
static inline bool switch_dp_tu_config(uint32_t pixel_khz, uint32_t hactive, uint32_t htotal, uint8_t rate,
                                       uint8_t lanes, bool enhanced_framing, struct switch_dp_tu *tu)
{
    const uint64_t f = 100000, link_rate = (uint64_t)switch_dp_rate_khz(rate) * 1000u;
    const uint64_t pclk = (uint64_t)pixel_khz * 1000u, bpp = 24;
    if (link_rate == 0 || lanes == 0 || pclk == 0 || htotal <= hactive) {
        return false;
    }
    const uint64_t input = pclk * bpp, output = link_rate * 8u * lanes;
    if (input >= output) {
        return false;
    }
    struct switch_dp_tu_params params = {
        (uint32_t)(link_rate * hactive / pclk), input * f / output, f, 0, 0, 0, 64, 64 * f};
    for (uint32_t size = 64; size >= 32; --size) {
        if (switch_dp_tu_try(&params, size)) {
            break;
        }
    }
    if (params.active_frac == 0) {
        tu->active_polarity = 0;
        tu->active_count = params.active_polarity == 0 ? params.active_count - 1 : params.active_count;
        tu->active_frac = 1;
    } else {
        tu->active_polarity = params.active_polarity;
        tu->active_count = params.active_count;
        tu->active_frac = params.active_frac;
    }
    tu->tu_size = params.tu_size;
    uint64_t watermark = params.ratio * tu->tu_size * (f - params.ratio) / f;
    watermark = (watermark + params.error) / f;
    tu->watermark = (uint32_t)watermark + (uint32_t)(bpp / 8) + 2;
    const uint64_t syms_per_line = hactive * bpp * (lanes * 8u);
    if (tu->watermark > 30) {
        tu->watermark = 30;
    } else if (tu->watermark > syms_per_line) {
        tu->watermark = (uint32_t)syms_per_line;
    }
    uint64_t hblank = (htotal - hactive - 7) * link_rate / pclk;
    hblank -= enhanced_framing ? 3 : 0;
    hblank -= 12u / lanes;
    tu->hblank_symbols = (uint32_t)hblank;
    const uint64_t vblank = (hactive - 25) * link_rate / pclk;
    tu->vblank_symbols = (uint32_t)(vblank - (36u / lanes + 4));
    return true;
}

struct switch_dp_plld2 {
    uint32_t n, m, pdiv, pdiv_hw;
    uint16_t sdm_din;
    uint64_t out_hz;  // PLL_D2 as programmed; pll_d2_out0 (the pixel clock) is half of it
};

// PLL_D2 for a pixel clock: pll_d2_out0 = PLL_D2 / 2 feeds both DC B (DISP2) and SOR1's pixel clock, so PLL_D2 runs at
// twice it, from the 38.4 MHz oscillator with M = 1 and the SDM's fraction for clocks the integer N cannot hit.
static inline bool switch_dp_plld2_for(uint32_t pixel_khz, uint32_t ref_khz, struct switch_dp_plld2 *pll)
{
    static const uint8_t pdivs[17][2] = {{1, 0},   {2, 1},   {3, 2},   {4, 3},   {5, 4},   {6, 5},
                                         {8, 6},   {9, 7},   {10, 8},  {12, 9},  {15, 10}, {16, 11},
                                         {18, 12}, {20, 13}, {24, 14}, {30, 15}, {32, 16}};
    const uint64_t target_hz = (uint64_t)pixel_khz * 2000u, ref_hz = (uint64_t)ref_khz * 1000u;
    if (target_hz == 0 || ref_hz == 0) {
        return false;
    }
    for (unsigned i = 0; i < 17; ++i) {
        const uint64_t vco = target_hz * pdivs[i][0];
        if (vco < 750000000u) {
            continue;
        }
        if (vco > 1500000000u) {
            return false;
        }
        const uint64_t scaled = (vco * 8192u + ref_hz / 2) / ref_hz;  // (N + 1/2 + SDM / 8192) * 8192
        const uint64_t n = (scaled - 4096u) / 8192u;
        const uint64_t din = scaled - 4096u - n * 8192u;
        // din 0 means "SDM off" to the hardware; -1/8192 is the nearest value it takes instead.
        const uint16_t sdm = din == 0 ? (uint16_t)0xFFFFu : (uint16_t)din;
        const int64_t effective = (int64_t)(n * 8192u + 4096u) + (int16_t)sdm;
        pll->n = (uint32_t)n;
        pll->m = 1;
        pll->pdiv = pdivs[i][0];
        pll->pdiv_hw = pdivs[i][1];
        pll->sdm_din = sdm;
        pll->out_hz = (uint64_t)effective * ref_hz / 8192u / pdivs[i][0];
        return n > 0 && n < 256;
    }
    return false;
}

#endif
