// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test: block_linear_copy against the per-byte GOB formula, for aligned and unaligned windows.
// Build: cc -O2 -o blocklinear_host blocklinear_host.c ../../AVD/BlockLinear.c (blocklinear_host_test)
#include <stdio.h>
#include <stdlib.h>
#include "../../AVD/BlockLinear.h"

static uint32_t offset(uint32_t x, uint32_t y, uint32_t pitch)
{
    const uint32_t gob = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
    return (y / 16) * pitch * 16 + (x / 64) * 1024 + ((y % 16) / 8) * 512 + gob;
}

int main(void)
{
    const uint32_t pitch = 1920, rows = 1088;
    // out is written at a pitch of width + 5 (an unaligned destination), so it is sized for that.
    uint8_t *bl = malloc((size_t)pitch * rows), *lin = malloc((size_t)pitch * rows),
            *out = malloc((size_t)(pitch + 5) * rows);
    for (uint32_t i = 0; i < pitch * rows; i++)
        lin[i] = (uint8_t)(rand() >> 7);
    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = 0; x < pitch; x++)
            bl[offset(x, y, pitch)] = lin[y * pitch + x];
    static const uint32_t cases[][4] = {{0, 0, 1920, 1080}, {0, 0, 1920, 1088}, {8, 2, 1900, 1070}, {3, 1, 17, 5},
                                        {16, 15, 33, 2},    {62, 7, 70, 17},    {0, 0, 1, 1}};
    int bad = 0;
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const uint32_t x0 = cases[c][0], y0 = cases[c][1], w = cases[c][2], h = cases[c][3];
        block_linear_copy(out, w + 5, bl, pitch, x0, y0, w, h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                if (out[y * (w + 5) + x] != lin[(y0 + y) * pitch + x0 + x]) {
                    if (!bad++)
                        printf("mismatch case %u at %u,%u\n", c, x, y);
                }
    }
    printf("%s\n", bad ? "FAIL" : "OK");
    return bad != 0;
}
