// SPDX-License-Identifier: BSD-2-Clause-Patent
// See BlockLinear.h.
#include "BlockLinear.h"
#include <string.h>

static inline const uint8_t *rowBase(const uint8_t *src, uint32_t pitch, uint32_t y)
{
    return src + (size_t)(y / 16) * pitch * 16 + ((y % 16) / 8) * 512 + ((y % 8) / 2) * 64 + (y % 2) * 16;
}

static inline uint32_t chunk(uint32_t x)   // the 16-byte sector row holding byte x, relative to rowBase
{
    return (x / 64) * 1024 + ((x % 64) / 32) * 256 + ((x % 32) / 16) * 32;
}

static void copyRow(uint8_t *dst, const uint8_t *row, uint32_t x0, uint32_t width)
{
    uint32_t x = x0, out = 0;
    if (x % 16) {                              // leading part of a sector
        const uint32_t n = 16 - x % 16 < width ? 16 - x % 16 : width;
        memcpy(dst, row + chunk(x) + x % 16, n);
        x += n, out += n;
    }
    for (; out + 16 <= width; x += 16, out += 16) memcpy(dst + out, row + chunk(x), 16);
    if (out < width) memcpy(dst + out, row + chunk(x), width - out);
}

void block_linear_copy(uint8_t *dst, size_t dstPitch, const uint8_t *src, uint32_t srcPitch, uint32_t x0, uint32_t y0,
                       uint32_t width, uint32_t height)
{
    uint32_t r = 0;
    if (y0 % 2 && height) {
        copyRow(dst, rowBase(src, srcPitch, y0), x0, width);
        r = 1;
    }
    for (; r + 2 <= height; r += 2) {           // an even/odd row pair shares every cache line it touches
        const uint8_t *even = rowBase(src, srcPitch, y0 + r);
        uint8_t *d0 = dst + (size_t)r * dstPitch, *d1 = d0 + dstPitch;
        if (!(x0 % 16) && !(width % 16)) {
            for (uint32_t x = x0, out = 0; out < width; x += 16, out += 16) {
                memcpy(d0 + out, even + chunk(x), 16);
                memcpy(d1 + out, even + chunk(x) + 16, 16);
            }
        } else {
            copyRow(d0, even, x0, width);
            copyRow(d1, even + 16, x0, width);
        }
    }
    if (r < height) copyRow(dst + (size_t)r * dstPitch, rowBase(src, srcPitch, y0 + r), x0, width);
}
