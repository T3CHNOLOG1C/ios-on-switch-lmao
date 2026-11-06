// SPDX-License-Identifier: BSD-2-Clause-Patent
// The first client frame against the panel hekate left in the boot framebuffer (debug kernels): from hekate's boot logo
// to backboardd's, no pixel may change.
#pragma once
#include <stdint.h>

// Debug kernels only: the check reads the 3.6 MB boot framebuffer once through its uncached view.
#if defined(DEVELOPMENT) && DEVELOPMENT
#define SWITCH_BOOT_LOGO_CHECK 1
#else
#define SWITCH_BOOT_LOGO_CHECK 0
#endif

struct BootLogoDifference {
    uint64_t pixels;
    uint32_t column, row;
    uint32_t boot, frame;
};

// Colours only: window A scans opaque, so alpha never reaches the panel. A null frame is a blank swap (black).
static inline struct BootLogoDifference boot_logo_compare(const volatile uint32_t *boot, uint64_t bootStride,
                                                          const uint32_t *frame, uint64_t frameStride, uint32_t width,
                                                          uint32_t height)
{
    struct BootLogoDifference found = {0, 0, 0, 0, 0};
    for (uint32_t row = 0; row < height; ++row) {
        const volatile uint32_t *bootRow = (const volatile uint32_t *)((uintptr_t)boot + (uint64_t)row * bootStride);
        const uint32_t *frameRow = frame ? (const uint32_t *)((uintptr_t)frame + (uint64_t)row * frameStride) : 0;
        for (uint32_t column = 0; column < width; ++column) {
            const uint32_t was = bootRow[column] & 0xffffffU, now = frameRow ? frameRow[column] & 0xffffffU : 0;
            if (was == now)
                continue;
            if (!found.pixels++) {
                found.column = column;
                found.row = row;
                found.boot = was;
                found.frame = now;
            }
        }
    }
    return found;
}
