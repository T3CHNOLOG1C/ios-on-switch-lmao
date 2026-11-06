/* XNU video_console.c's rotated-console wide helpers (vc_rotate3_wide/store4/copy4, cut out of @xnu's video_console.c
 * into helpers.h by BUILD.bazel) against the 32-bit loops they replaced: scroll up and down for every region
 * and shift step, clears, and glyph columns written four pixels at a time over a previous glyph. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Checked in every build (never compiled out): the first difference fails the run. */
#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                              \
        }                                                         \
    } while (0)
typedef int boolean_t;
#define TRUE  1
#define FALSE 0
#include "helpers.h"
#define ROWS  1280
#define WIDTH 720 /* pixels per portrait row */
#define LINE  16
static uint32_t a[ROWS * WIDTH] __attribute__((aligned(64))), b[ROWS * WIDTH] __attribute__((aligned(64)));

static void fill(void)
{
    for (unsigned i = 0; i < ROWS * WIDTH; ++i)
        a[i] = b[i] = (uint32_t)rand();
}

int main(void)
{
    srand(1104);
    unsigned checks = 0;
    for (unsigned top = 0; top < 45; top += 7)
        for (unsigned bottom = top + 2; bottom <= 45; bottom += 9)
            for (unsigned num = 1; num < bottom - top; num += 5) {
                unsigned first = top * LINE, last = bottom * LINE, shift = num * LINE;
                fill();
                for (unsigned x = 0; x < ROWS; x += 37) { /* scroll up */
                    volatile uint32_t *p = a + x * WIDTH, *q = b + x * WIDTH;
                    CHECK(vc_rotate3_wide(q + first, q + first + shift, last - shift - first));
                    for (unsigned y = first; y < last - shift; ++y)
                        p[y] = p[y + shift];
                    for (unsigned y = first; y < last - shift; y += 4)
                        vc_rotate3_copy4(q + y, q + y + shift);
                }
                CHECK(!memcmp(a, b, sizeof a));
                ++checks;
                fill();
                for (unsigned x = 0; x < ROWS; x += 37) { /* scroll down */
                    volatile uint32_t *p = a + x * WIDTH, *q = b + x * WIDTH;
                    CHECK(vc_rotate3_wide(q + first + shift, q + first, last - shift - first));
                    for (unsigned y = last; y > first + shift; --y)
                        p[y - 1] = p[y - 1 - shift];
                    for (unsigned y = last; y > first + shift; y -= 4)
                        vc_rotate3_copy4(q + y - 4, q + y - 4 - shift);
                }
                CHECK(!memcmp(a, b, sizeof a));
                ++checks;
                fill();
                const uint32_t back = 0x11223344;
                const uint64_t wide = (uint64_t)back << 32 | back;
                for (unsigned x = 0; x < ROWS; x += 37) { /* clear */
                    volatile uint32_t *p = a + x * WIDTH, *q = b + x * WIDTH;
                    for (unsigned y = first; y < last; ++y)
                        p[y] = back;
                    for (unsigned y = first; y < last; y += 4)
                        vc_rotate3_store4(q + y, wide, wide);
                }
                CHECK(!memcmp(a, b, sizeof a));
                ++checks;
            }
    /* Glyph column over a previous glyph with the same colours: 4-pixel writes wherever any pixel changed equal the
     * original per-pixel writes, given the screen holds the previous glyph drawn in those colours. */
    for (unsigned trial = 0; trial < 20000; ++trial) {
        uint32_t fore = (uint32_t)rand(), backc = (uint32_t)rand(), prev[16], cur[16],
                 screen1[16] __attribute__((aligned(16))), screen2[16] __attribute__((aligned(16)));
        for (int i = 0; i < 16; ++i) {
            prev[i] = rand() & 1 ? 0xffffffffu : 0;
            cur[i] = rand() % 3 ? prev[i] : ~prev[i];
            screen1[i] = screen2[i] = (backc & ~prev[i]) | (fore & prev[i]);
        }
        uint32_t out[16], changed = 0;
        for (int i = 0; i < 16; ++i) {
            if (cur[i] != prev[i])
                changed |= 1u << i;
            out[i] = (backc & ~cur[i]) | (fore & cur[i]);
        }
        for (int i = 0; i < 16; ++i)
            if (cur[i] != prev[i])
                screen1[i] = out[i]; /* original */
        for (int i = 0; i < 16; i += 4)
            if (changed & (0xfu << i))
                vc_rotate3_store4(screen2 + i, (uint64_t)out[i + 1] << 32 | out[i],
                                  (uint64_t)out[i + 3] << 32 | out[i + 2]);
        CHECK(!memcmp(screen1, screen2, sizeof screen1));
    }
    CHECK(!vc_rotate3_wide(a + 1, a, 16) && !vc_rotate3_wide(a, a, 15));
    printf("PASS %u scroll/clear cases, 20000 glyph columns\n", checks);
    return 0;
}
