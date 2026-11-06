// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tiled landscape -> portrait copy for the Switch panel, a drop-in for the rotate branch of
// drivers/NativeMobileDisplay/NativeScanout.h native_legacy_scanout_copy (same pixel mapping: source (x, y) lands in
// destination row width-1-x, column y). Kernel code is built with -mgeneral-regs-only, so no NEON: 16x16 tiles keep
// the 16 source rows of a tile in L1 and give every destination row whole 64-byte runs (the panel stride
// 2880 = 45 * 64), which a write-combined destination turns into full bursts. Callers keep their own bounds checks
// (native_legacy_scanout_layout) and the trailing dsb.
#pragma once
#include <stdint.h>

static inline void tegra_scanout_rotate_tiled(void *destination, uint64_t destinationStride, const void *source,
                                              uint64_t sourceStride, uint32_t width, uint32_t height)
{
    const uint32_t tile = 16;
    for (uint32_t y0 = 0; y0 < height; y0 += tile) {
        const uint32_t y1 = height - y0 < tile ? height : y0 + tile;
        for (uint32_t x0 = 0; x0 < width; x0 += tile) {
            const uint32_t x1 = width - x0 < tile ? width : x0 + tile;
            for (uint32_t x = x0; x < x1; ++x) {
                uint32_t *out =
                    (uint32_t *)((uintptr_t)destination + (uint64_t)(width - 1 - x) * destinationStride) + y0;
                const unsigned char *in = (const unsigned char *)source + (uint64_t)y0 * sourceStride + (uint64_t)x * 4;
                uint32_t y = y0;
                // Two pixels per 64-bit store when the destination is 8-byte aligned (always, for even y0 and stride 2880).
                if (!((uintptr_t)out & 7))
                    for (; y + 1 < y1; y += 2, in += 2 * sourceStride, out += 2) {
                        uint32_t lo, hi;
                        __builtin_memcpy(&lo, in, 4);
                        __builtin_memcpy(&hi, in + sourceStride, 4);
                        *(uint64_t *)out = (uint64_t)lo | ((uint64_t)hi << 32);
                    }
                for (; y < y1; ++y, in += sourceStride, ++out)
                    __builtin_memcpy(out, in, 4);
            }
        }
    }
}
