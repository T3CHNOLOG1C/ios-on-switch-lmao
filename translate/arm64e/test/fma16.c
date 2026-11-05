/*
 * test_fma16: half-precision fused multiply-add through double (what the FP16 stubs emit for FMADD/FMLA) against the
 * native FEAT_FP16 instructions, exhaustively over all a, b for a few addends c, plus the vector path's round-to-odd
 * narrowing (FCVTXN then FCVTN) against the scalar double -> half FCVT.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static inline uint16_t native(uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t r;
    __asm__ volatile("fmov s1, %w1\n fmov s2, %w2\n fmov s3, %w3\n fmadd h0, h1, h2, h3\n fmov %w0, s0"
                     : "=r"(r)
                     : "r"((uint32_t)a), "r"((uint32_t)b), "r"((uint32_t)c)
                     : "v0", "v1", "v2", "v3");
    return r;
}
static inline uint16_t scalar_emul(uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t r;
    __asm__ volatile("fmov s1, %w1\n fmov s2, %w2\n fmov s3, %w3\n fcvt d1, h1\n fcvt d2, h2\n fcvt d3, h3\n"
                     " fmadd d1, d1, d2, d3\n fcvt h0, d1\n fmov %w0, s0"
                     : "=r"(r)
                     : "r"((uint32_t)a), "r"((uint32_t)b), "r"((uint32_t)c)
                     : "v0", "v1", "v2", "v3");
    return r;
}
static inline uint16_t vector_emul(uint16_t a, uint16_t b, uint16_t c)
{
    uint16_t r;
    __asm__ volatile(
        "fmov s1, %w1\n fmov s2, %w2\n fmov s3, %w3\n fcvtl v1.4s, v1.4h\n fcvtl v2.4s, v2.4h\n fcvtl v3.4s, v3.4h\n"
        " fcvtl v1.2d, v1.2s\n fcvtl v2.2d, v2.2s\n fcvtl v3.2d, v3.2s\n fmla v3.2d, v1.2d, v2.2d\n"
        " fcvtxn v0.2s, v3.2d\n fcvtn v0.4h, v0.4s\n umov %w0, v0.h[0]"
        : "=r"(r)
        : "r"((uint32_t)a), "r"((uint32_t)b), "r"((uint32_t)c)
        : "v0", "v1", "v2", "v3");
    return r;
}
static int is_nan(uint16_t h)
{
    return (h & 0x7C00) == 0x7C00 && (h & 0x3FF);
}
static int is_snan(uint16_t h)
{
    return is_nan(h) && !(h & 0x200);
}

static const uint16_t cs[] = {0x3C00, 0xBE00, 0x0400, 0x3555, 0x7BFF, 0x0001, 0x8000, 0xD3FF};
struct job {
    int t;
    uint64_t bad_s, bad_v, n;
};
static void *run(void *p)
{
    struct job *j = p;
    for (uint32_t a = (uint32_t)j->t; a < 65536; a += 8) {
        if (is_snan((uint16_t)a)) continue;
        for (uint32_t b = 0; b < 65536; b++) {
            if (is_snan((uint16_t)b)) continue;
            for (unsigned k = 0; k < sizeof(cs) / 2; k++) {
                uint16_t c = cs[k], x = native((uint16_t)a, (uint16_t)b, c);
                uint16_t s = scalar_emul((uint16_t)a, (uint16_t)b, c), v = vector_emul((uint16_t)a, (uint16_t)b, c);
                j->n++;
                if (s != x && !(is_nan(s) && is_nan(x) && s == x)) j->bad_s++;
                if (v != x) j->bad_v++;
                if ((s != x || v != x) && j->bad_s + j->bad_v < 4)
                    printf("a=%04x b=%04x c=%04x native %04x scalar %04x vector %04x\n", a, b, c, x, s, v);
            }
        }
    }
    return NULL;
}
int main(void)
{
    pthread_t th[8];
    struct job j[8] = {0};
    for (int t = 0; t < 8; t++) {
        j[t].t = t;
        pthread_create(&th[t], NULL, run, &j[t]);
    }
    uint64_t bs = 0, bv = 0, n = 0;
    for (int t = 0; t < 8; t++) {
        pthread_join(th[t], NULL);
        bs += j[t].bad_s;
        bv += j[t].bad_v;
        n += j[t].n;
    }
    printf("fma16: %llu cases, scalar-path mismatches %llu, vector-path mismatches %llu\n", (unsigned long long)n,
           (unsigned long long)bs, (unsigned long long)bv);
    return bs || bv;
}
