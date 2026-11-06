// SPDX-License-Identifier: GPL-2.0-only
// DPAUX1; see TegraDpaux1.h. Linux drivers/gpu/drm/tegra/dpaux.c (Copyright (C) 2013 NVIDIA Corporation):
// tegra_dpaux_resume (clock, PLL_DP at 270 MHz, 1 ms, reset), tegra_dpaux_pad_config (T210: CMH 2, DRVZ 4, DRVI 0x30),
// tegra_dpaux_probe's interrupt enables (HPD timing left at its reset values, as there), tegra_dpaux_transfer. PLL_DP: drivers/clk/tegra/clk-tegra210.c
// pll_dp_params and tegra210_plldp_set_defaults (38.4 MHz * (28 + 1/2 + SDM 0xf400) / 4 = 270 MHz, spread on).
#include "TegraDpaux1.h"

#include <IOKit/IOLib.h>

#include "SwitchDPHardware.h"

namespace {
constexpr uint64_t kDpaux1PA = 0x54040000;
constexpr unsigned kClockDpaux1 = 207, kClockSorSafe = 222;

enum : uint32_t {
    PLLDP_BASE = 0x590,
    PLLDP_MISC = 0x594,
    PLLDP_SS_CFG = 0x598,
    PLLDP_SS_CTRL1 = 0x59c,
    PLLDP_SS_CTRL2 = 0x5a0,
};

constexpr uint32_t kPllEnable = 1u << 30, kPllLock = 1u << 27;
constexpr uint32_t kPllDp270 = (3u << 19) | (28u << 8) | 1u;  // P = 4 (qlin code 3), N = 28, M = 1
constexpr uint32_t kPadAux = (2u << 12) | (4u << 8) | (0x30u << 2) | (1u << 1);
constexpr uint32_t kAllInterrupts = SWITCH_DPAUX_INTR_AUX_DONE | SWITCH_DPAUX_INTR_IRQ_EVENT |
                                    SWITCH_DPAUX_INTR_UNPLUG_EVENT | SWITCH_DPAUX_INTR_PLUG_EVENT;
}  // namespace

uint32_t TegraDpaux1::read(uint32_t word) const
{
    return switch_mmio_read32(base, word * 4u);
}

void TegraDpaux1::write(uint32_t word, uint32_t value)
{
    switch_mmio_write32(base, word * 4u, value);
}

bool TegraDpaux1::startPllDp(char *why, size_t size)
{
    if ((switch_car_read(PLLDP_BASE) & (kPllEnable | kPllLock)) == (kPllEnable | kPllLock)) {
        return true;
    }
    switch_car_write(PLLDP_MISC, 0x40000020);
    switch_car_write(PLLDP_SS_CFG, 0xc0000000);
    switch_car_write(PLLDP_SS_CTRL1, 0xf400f0da);
    switch_car_write(PLLDP_SS_CTRL2, 0x2004f400);
    switch_car_write(PLLDP_BASE, kPllDp270);  // IDDQ (bit 18) clear
    IODelay(5);
    switch_car_write(PLLDP_BASE, kPllDp270 | kPllEnable);
    if (!switch_mmio_wait32(switchcar::car(), PLLDP_BASE, kPllLock, kPllLock, 300)) {
        snprintf(why, size, "PLL_DP did not lock (base %#x)", switch_car_read(PLLDP_BASE));
        return false;
    }
    return true;
}

bool TegraDpaux1::powerUp(char *why, size_t size)
{
    if (base == nullptr) {
        base = switch_mmio_map(kDpaux1PA, 0x1000);
    }
    if (!startPllDp(why, size)) {
        return false;
    }
    if (!switch_car_clock_enable(kClockSorSafe) || !switch_car_clock_enable(kClockDpaux1)) {
        snprintf(why, size, "DPAUX1 or SOR_SAFE clock did not enable");
        return false;
    }
    IOSleep(1);
    switch_car_reset_deassert(kClockDpaux1);
    IOSleep(1);
    write(SWITCH_DPAUX_HYBRID_PADCTL, kPadAux);
    write(SWITCH_DPAUX_HYBRID_SPARE, read(SWITCH_DPAUX_HYBRID_SPARE) & ~1u);
    write(SWITCH_DPAUX_INTR_AUX, kAllInterrupts);
    write(SWITCH_DPAUX_INTR_EN_AUX, kAllInterrupts);
    return true;
}

void TegraDpaux1::powerDown()
{
    if (base == nullptr) {
        return;
    }
    write(SWITCH_DPAUX_INTR_EN_AUX, 0);
    write(SWITCH_DPAUX_INTR_AUX, kAllInterrupts);
    write(SWITCH_DPAUX_HYBRID_SPARE, read(SWITCH_DPAUX_HYBRID_SPARE) | 1u);
    switch_car_reset_assert(kClockDpaux1);
    switch_car_clock_disable(kClockDpaux1);
}

bool TegraDpaux1::hpdHigh() const
{
    return (read(SWITCH_DPAUX_AUXSTAT) & SWITCH_DPAUX_AUXSTAT_HPD) != 0;
}

uint32_t TegraDpaux1::auxstat() const
{
    return read(SWITCH_DPAUX_AUXSTAT);
}

uint32_t TegraDpaux1::takeInterrupt()
{
    const uint32_t cause = read(SWITCH_DPAUX_INTR_AUX);
    write(SWITCH_DPAUX_INTR_AUX, cause);
    return cause;
}

void TegraDpaux1::start(const struct switch_dp_aux_request &request)
{
    write(SWITCH_DPAUX_AUXADDR, request.address);
    const uint32_t control = switch_dp_auxctl(&request);
    write(SWITCH_DPAUX_AUXCTL, control);
    if (!switch_dp_aux_reads(request.op)) {
        uint32_t words[4];
        switch_dp_aux_fifo_words(&request, words);
        for (uint32_t i = 0; i < (request.size + 3u) / 4u; ++i) {
            write(SWITCH_DPAUX_AUXDATA_WRITE(i), words[i]);
        }
    }
    write(SWITCH_DPAUX_AUXCTL, control | SWITCH_DPAUX_AUXCTL_TRANSACTREQ);
}

enum switch_dp_aux_result TegraDpaux1::finish(struct switch_dp_aux_request &request)
{
    const uint32_t status = read(SWITCH_DPAUX_AUXSTAT);
    write(SWITCH_DPAUX_AUXSTAT, SWITCH_DPAUX_AUXSTAT_ERRORS);
    const enum switch_dp_aux_result result = switch_dp_aux_decode(status, &request);
    if (result == SWITCH_DP_AUX_ACK && switch_dp_aux_reads(request.op)) {
        for (uint32_t i = 0; i < (request.size + 3u) / 4u; ++i) {
            const uint32_t word = read(SWITCH_DPAUX_AUXDATA_READ(i));
            for (uint32_t b = 0; b < 4 && i * 4 + b < request.size; ++b) {
                request.data[i * 4 + b] = (uint8_t)(word >> (8 * b));
            }
        }
    }
    return result;
}
