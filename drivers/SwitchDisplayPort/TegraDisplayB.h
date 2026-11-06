// SPDX-License-Identifier: GPL-2.0-only
// Display controller B (0x54240000, SPI 74 level-high) driving SOR1: its power partition and clock, the mode's timing,
// and window A scanning a pitch-linear BGRA surface by DMA through the SMMU (swgroup dcb), flipped at frame end.
#ifndef TEGRA_DISPLAY_B_H
#define TEGRA_DISPLAY_B_H

#include <stddef.h>
#include <stdint.h>

#include "SwitchDPModes.h"

#define TEGRA_DISPLAY_B_INTID (32u + 74u)

class TegraDisplayB {
public:
    // The DISB partition with DISP2 (already sourced from pll_d2_out0) and the controller's base configuration.
    bool powerUp(char *why, size_t size);
    // The mode's timing; the controller runs continuously from here, every window off. The frame-end interrupt is on
    // only while a flip waits to be taken: a still screen costs the CPU nothing.
    void setMode(const struct switch_dp_mode &mode);
    void enableSor(bool on);
    // Window A scans `width` x `height` BGRA at `iova` (rows of `stride` bytes) from the next frame on; its frame end
    // interrupt says it was taken.
    void scanout(uint32_t iova, uint32_t width, uint32_t height, uint32_t stride);
    void windowOff();
    // The interrupt's cause, cleared; the frame-end interrupt off again until the next scanout().
    uint32_t takeInterrupt();
    void powerDown();

private:
    volatile uint8_t *base = nullptr;
    uint32_t read(uint32_t word) const;
    void write(uint32_t word, uint32_t value);
    void commit(uint32_t update);
};

#endif
