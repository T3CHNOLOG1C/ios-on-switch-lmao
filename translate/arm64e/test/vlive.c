/*
 * test_vlive: native check of a64x_vregs() (the V-register read/full-write decoder behind FP stub temporaries).
 *
 * Every distinct SIMD&FP data-processing word in this Mac's own dyld cache is executed natively from a random state,
 * then again with one V register changed. For every V register the decoder says the word does not read, nothing
 * else in the result may change; for every one it says the word fully writes, that register's result may not change
 * either. A miss is an unsound claim. Words that fault (unallocated, or not on this core) are skipped.
 *
 * usage: test_vlive CACHE [max_words]
 */
#include "../a64x/a64x.h"
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

bool a64x_vregs_public(uint32_t w, uint32_t *vr, uint32_t *vw);

struct state {
    uint64_t x[31];
    uint64_t nzcv;
    uint64_t q[32][2];
} __attribute__((aligned(16)));

static uint32_t *jit;
#define JIT_WORDS 4096

typedef void (*fn_t)(struct state *);

static int emit_frame(uint32_t *c, const uint32_t *body, int nbody)
{
    int n = 0;
    c[n++] = 0xA9BF7BFDu; /* stp x29, x30, [sp, #-16]! */
    c[n++] = 0xD10283FFu; /* sub sp, sp, #160 */
    for (int i = 0; i < 5; i++)
        c[n++] = 0xA9000000u | ((uint32_t)(2 * i) << 15) | ((20u + 2 * i) << 10) | (31u << 5) |
                 (19u + 2 * i); /* stp x19+2i.. */
    for (int i = 0; i < 4; i++)
        c[n++] = 0x6D000000u | ((uint32_t)(10 + 2 * i) << 15) | ((9u + 2 * i) << 10) | (31u << 5) |
                 (8u + 2 * i); /* stp d8+2i.. */
    c[n++] = 0xF9004BE0u;      /* str x0, [sp, #144] */
    for (uint32_t i = 0; i < 32; i++)
        c[n++] = 0x3DC00000u | ((256u / 16 + i) << 10) | i; /* ldr qi, [x0, #256+16i] */
    c[n++] = 0xF9407C01u;                                   /* ldr x1, [x0, #248] */
    c[n++] = 0xD51B4201u;                                   /* msr nzcv, x1 */
    for (uint32_t i = 1; i < 31; i++)
        if (i != 18)
            c[n++] = 0xF9400000u | (i << 10) | i; /* ldr xi, [x0, #8i] */
    c[n++] = 0xF9400000u;                         /* ldr x0, [x0] */
    memcpy(c + n, body, (size_t)nbody * 4);
    n += nbody;
    c[n++] = 0xA9BF07E0u; /* stp x0, x1, [sp, #-16]! */
    c[n++] = 0xF94053E0u; /* ldr x0, [sp, #160] (state) */
    for (uint32_t i = 2; i < 31; i++)
        if (i != 18)
            c[n++] = 0xF9000000u | (i << 10) | i; /* str xi, [x0, #8i] */
    c[n++] = 0xD53B4201u;                         /* mrs x1, nzcv */
    c[n++] = 0xF9007C01u;                         /* str x1, [x0, #248] */
    c[n++] = 0xA8C10FE2u;                         /* ldp x2, x3, [sp], #16 */
    c[n++] = 0xF9000002u;                         /* str x2, [x0] */
    c[n++] = 0xF9000403u;                         /* str x3, [x0, #8] */
    for (uint32_t i = 0; i < 32; i++)
        c[n++] = 0x3D800000u | ((256u / 16 + i) << 10) | i; /* str qi */
    for (int i = 0; i < 5; i++)
        c[n++] = 0xA9400000u | ((uint32_t)(2 * i) << 15) | ((20u + 2 * i) << 10) | (31u << 5) | (19u + 2 * i);
    for (int i = 0; i < 4; i++)
        c[n++] = 0x6D400000u | ((uint32_t)(10 + 2 * i) << 15) | ((9u + 2 * i) << 10) | (31u << 5) | (8u + 2 * i);
    c[n++] = 0x910283FFu; /* add sp, sp, #160 */
    c[n++] = 0xA8C17BFDu; /* ldp x29, x30, [sp], #16 */
    c[n++] = 0xD65F03C0u; /* ret */
    return n;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static void random_state(struct state *s, int kind)
{
    (void)kind;
    for (int i = 0; i < 31; i++)
        s->x[i] = rnd();
    s->x[18] = 0;
    s->nzcv = (rnd() & 0xF) << 28;
    for (int i = 0; i < 32; i++) {
        s->q[i][0] = rnd();
        s->q[i][1] = rnd();
        if (i & 1) { /* plausible floats too: small exponents in every lane size */
            s->q[i][0] &= 0xBFFFBFFFBFFFBFFFull;
            s->q[i][1] &= 0x3FF3FFFF3FFF3FFFull;
        }
    }
}

static sigjmp_buf jb;

static void on_sig(int s)
{
    (void)s;
    siglongjmp(jb, 1);
}

static uint32_t *words;
static size_t nwords, capwords;
static bool mem_mode;
#define MEMSZ (160 * 1024) /* base at +32 KB: LDR/STR Q offsets reach +65520 */
static uint8_t *mem, *mem0;
static uint8_t *seen;

static void collect(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat sb;
    if (fd < 0 || fstat(fd, &sb))
        return;
    const uint8_t *b = mmap(NULL, (size_t)sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    uint32_t moff, mcnt;
    memcpy(&moff, b + 0x10, 4);
    memcpy(&mcnt, b + 0x14, 4);
    for (uint32_t i = 0; i < mcnt; i++) {
        uint64_t addr, size, off;
        uint32_t maxp, initp;
        memcpy(&addr, b + moff + 32 * i, 8);
        memcpy(&size, b + moff + 32 * i + 8, 8);
        memcpy(&off, b + moff + 32 * i + 16, 8);
        memcpy(&maxp, b + moff + 32 * i + 24, 4);
        memcpy(&initp, b + moff + 32 * i + 28, 4);
        if (!(initp & 4))
            continue;
        for (uint64_t k = 0; k + 4 <= size; k += 4) {
            uint32_t w;
            memcpy(&w, b + off + k, 4);
            bool dp = ((w >> 25) & 7) == 7, ls = mem_mode && ((w >> 25) & 5) == 4 && ((w >> 26) & 1);
            if (mem_mode ? !ls : !dp)
                continue; /* SIMD&FP data processing (or SIMD&FP loads/stores) */
            if (mem_mode && ((w >> 5) & 31) == 31)
                continue; /* SP-based: would write the harness's own frame */
            if (seen[w >> 3] & (1 << (w & 7)))
                continue;
            seen[w >> 3] |= 1 << (w & 7);
            if (nwords == capwords) {
                capwords = capwords ? capwords * 2 : 1 << 20;
                words = realloc(words, capwords * 4);
            }
            words[nwords++] = w;
        }
    }
    munmap((void *)b, (size_t)sb.st_size);
    close(fd);
}

int main(int argc, char **argv)
{
    size_t maxw = argc > 2 ? (size_t)atol(argv[2]) : 400000;
    mem_mode = getenv("VLIVE_MEM") != NULL;
    mem = malloc(MEMSZ);
    mem0 = malloc(MEMSZ);
    seen = calloc(1u << 29, 1);
    collect(argv[1]);
    for (int i = 1; i < 100; i++) {
        char p[1024];
        snprintf(p, sizeof(p), "%s.%02d", argv[1], i);
        collect(p);
    }
    printf("distinct SIMD&FP words: %zu\n", nwords);
    jit = mmap(NULL, JIT_WORDS * 4, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    signal(SIGILL, on_sig);
    signal(SIGSEGV, on_sig);
    signal(SIGBUS, on_sig);
    signal(SIGFPE, on_sig);
    signal(SIGTRAP, on_sig);
    uint64_t tested = 0, faulted = 0, bad_read = 0, bad_write = 0, claims_dead = 0, unknown = 0;
    size_t step = nwords > maxw ? nwords / maxw : 1;
    for (size_t wi = 0; wi < nwords; wi += step) {
        uint32_t w = words[wi], vr, vw;
        if (!a64x_vregs_public(w, &vr, &vw))
            unknown++;
        pthread_jit_write_protect_np(0);
        int n = emit_frame(jit, &w, 1);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(jit, (size_t)n * 4);
        fn_t f = (fn_t)(uintptr_t)jit;
        struct state s0, a, b;
        random_state(&s0, 3);
        if (mem_mode) {
            for (int k = 0; k < MEMSZ; k++)
                mem0[k] = (uint8_t)rnd();
            for (int k = 0; k < 31; k++)
                s0.x[k] = (uint64_t)(uintptr_t)(mem + 32 * 1024);
            s0.x[18] = 0;
            memcpy(mem, mem0, MEMSZ);
        }
        a = s0;
        static uint8_t *mem_a;
        if (!mem_a)
            mem_a = malloc(MEMSZ);
        if (sigsetjmp(jb, 1)) {
            faulted++;
            continue;
        }
        f(&a);
        if (mem_mode)
            memcpy(mem_a, mem, MEMSZ);
        tested++;
        for (uint32_t v = 0; v < 32; v++) {
            if (vr & (1u << v))
                continue;
            for (int t = 0; t < 2; t++) {
                b = s0;
                b.q[v][0] = rnd();
                b.q[v][1] = rnd();
                if (mem_mode)
                    memcpy(mem, mem0, MEMSZ);
                if (sigsetjmp(jb, 1))
                    break;
                f(&b);
                if (mem_mode && memcmp(mem, mem_a, MEMSZ)) {
                    if (bad_read++ < 10)
                        printf("BAD not-read claim (memory differs): %08x v%u (vr %08x vw %08x)\n", w, v, vr, vw);
                    break;
                }
                struct state a2 = a, b2 = b;
                if (!(vw & (1u << v))) {
                    a2.q[v][0] = a2.q[v][1] = 0;
                    b2.q[v][0] = b2.q[v][1] = 0;
                }
                if (memcmp(&a2, &b2, sizeof(a2))) {
                    bool only_v = true;
                    for (uint32_t k = 0; k < 32; k++)
                        if (k != v && memcmp(a2.q[k], b2.q[k], 16))
                            only_v = false;
                    if (memcmp(a2.x, b2.x, sizeof(a2.x)) || a2.nzcv != b2.nzcv)
                        only_v = false;
                    if (only_v) {
                        if (bad_write++ < 10)
                            printf("BAD full-write claim: %08x v%u (vr %08x vw %08x)\n", w, v, vr, vw);
                    } else if (bad_read++ < 10)
                        printf("BAD not-read claim: %08x v%u (vr %08x vw %08x)\n", w, v, vr, vw);
                    break;
                }
            }
            if (vw & (1u << v))
                claims_dead++;
        }
    }
    printf("tested %llu words (%llu faulted, %llu unknown to the decoder); full-write claims %llu; unsound: %llu read "
           "misses, %llu write claims\n",
           (unsigned long long)tested, (unsigned long long)faulted, (unsigned long long)unknown,
           (unsigned long long)claims_dead, (unsigned long long)bad_read, (unsigned long long)bad_write);
    return bad_read || bad_write;
}
