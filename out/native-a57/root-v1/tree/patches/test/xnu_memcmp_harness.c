// r17-launch: XNU's arm64 memcmp/bcmp (osfmk/arm64/ioswitch_memcmp.h, from @xnu, the patched source) against the
// portable byte loops they replace (osfmk/device/subrs.c), exhaustively for every first difference and length up to 80
// bytes at every alignment, plus random inputs; each buffer ends right before an unreadable page, so a load past n
// faults. xnu_memcmp_test.cpp runs the two halves; each returns how many cases differed.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ioswitch_memcmp.h"
#include "xnu_memcmp_harness.h"

static int reference_memcmp(const void *s1, const void *s2, size_t n)
{
    if (n != 0) {
        const unsigned char *p1 = s1, *p2 = s2;
        do {
            if (*p1++ != *p2++) return *--p1 - *--p2;
        } while (--n != 0);
    }
    return 0;
}

static int reference_bcmp(const void *pa, const void *pb, size_t len)
{
    const char *a = (const char *)pa, *b = (const char *)pb;
    if (len == 0) return 0;
    do {
        if (*a++ != *b++) break;
    } while (--len);
    if ((len & 0xFFFFFFFF00000000ULL) && !(len & 0x00000000FFFFFFFFULL)) return ~0;
    return (int)len;
}

// The two kernel functions exactly as subrs.c now defines them on arm64.
static int fast_memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = s1, *p2 = s2;
    const size_t i = ioswitch_cmp_first_difference(p1, p2, n);
    return i < n ? p1[i] - p2[i] : 0;
}

static int fast_bcmp(const void *pa, const void *pb, size_t len)
{
    len -= ioswitch_cmp_first_difference((const unsigned char *)pa, (const unsigned char *)pb, len);
    if ((len & 0xFFFFFFFF00000000ULL) && !(len & 0x00000000FFFFFFFFULL)) return ~0;
    return (int)len;
}

// A region whose last readable byte is right before a PROT_NONE page.
static unsigned char *guarded(size_t page)
{
    unsigned char *p = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED || mprotect(p + page, page, PROT_NONE)) {
        perror("guard page");
        exit(2);
    }
    return p + page;  // one past the end of the readable page
}

static unsigned long failures;

static void check(const unsigned char *a, const unsigned char *b, size_t n)
{
    const int m = fast_memcmp(a, b, n), rm = reference_memcmp(a, b, n);
    const int c = fast_bcmp(a, b, n), rc = reference_bcmp(a, b, n);
    if (m != rm || c != rc) {
        if (failures++ < 10) printf("FAIL n=%zu memcmp %d vs %d, bcmp %d vs %d\n", n, m, rm, c, rc);
    }
}

// Random contents eight bytes per step (xorshift64*: random() per byte made the random half take a minute).
static uint64_t contents = 0x9e3779b97f4a7c15ull;
static void fill(unsigned char *a, unsigned char *b, size_t n)
{
    for (size_t k = 0; k < n; k += 8) {
        contents ^= contents >> 12, contents ^= contents << 25, contents ^= contents >> 27;
        const uint64_t x = contents * 0x2545f4914f6cdd1dull;
        for (size_t j = 0; j < 8 && k + j < n; j++) a[k + j] = b[k + j] = (unsigned char)(x >> (8 * j));
    }
}

unsigned long xnu_memcmp_exhaustive(unsigned long *cases)
{
    const size_t page = (size_t)getpagesize();
    unsigned char *enda = guarded(page), *endb = guarded(page);
    failures = 0;

    // Every length, both buffers ending at the guard page (so any over-read faults) and at every misalignment, with
    // the first difference at every position, every sign, and none.
    for (size_t n = 0; n <= 80; n++) {
        for (size_t mis = 0; mis < 8; mis++) {
            unsigned char *a = enda - n - mis, *b = endb - n;
            for (size_t diff = 0; diff <= n; diff++) {
                for (int sign = 0; sign < 3; sign++) {
                    for (size_t k = 0; k < n; k++) a[k] = b[k] = (unsigned char)(0x41 + (k * 7) % 23);
                    if (diff < n) {
                        b[diff] = sign == 0 ? 0x00 : sign == 1 ? 0xff : (unsigned char)(a[diff] ^ 0x80);
                        if (a[diff] == b[diff]) b[diff] ^= 1;
                        // Later bytes differ the other way, so only the first difference may decide.
                        for (size_t k = diff + 1; k < n; k++) b[k] = (unsigned char)~a[k];
                    }
                    check(a, b, n);
                    check(b, a, n);
                    *cases += 2;
                }
            }
        }
    }
    return failures;
}

unsigned long xnu_memcmp_random(unsigned seed, int rounds, unsigned long *cases)
{
    // Random contents, lengths up to 3 pages, random alignments, mostly equal prefixes.
    const size_t page = (size_t)getpagesize();
    unsigned char *big_a = malloc(4 * page + 16), *big_b = malloc(4 * page + 16);
    failures = 0;
    srandom(seed);
    for (int round = 0; round < rounds; round++) {
        size_t n = (size_t)random() % (round & 1 ? 64 : 3 * page);
        unsigned char *a = big_a + random() % 8, *b = big_b + random() % 8;
        fill(a, b, n);
        if (n && random() % 4) b[(size_t)random() % n] = (unsigned char)random();
        check(a, b, n);
        (*cases)++;
    }
    free(big_a);
    free(big_b);
    return failures;
}
