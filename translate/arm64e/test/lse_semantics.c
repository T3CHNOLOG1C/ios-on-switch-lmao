/*
 * Runs each LSE/LDRA instruction natively (Apple silicon has LSE) and the a64x ARMv8.0 stub that replaces it, from
 * identical random states, and compares all registers, SP, NZCV, the target memory and the red zone. Also a 4-thread
 * atomicity check of translated LDADDAL loops.
 *
 *   lse_semantics_main(argc, argv): WORDS.txt [trials]   first column of each line: a hex word
 *                                   --gen [trials]       systematic encodings: every op/size/order, aliasing registers
 *                                   --random N [trials]  N random LSE encodings (all registers but x18)
 * A64X_TEST_BREAK in the environment flips a bit in every stub: the run must then fail.
 *
 * Built as plain arm64: JIT entry pointers need no signing.
 */
#include "../a64x/a64x.h"
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

struct ctx {
    uint64_t host_sp;   /* 0 */
    uint64_t in_nzcv;   /* 8 */
    uint64_t in_sp;     /* 16 */
    uint64_t in_x[31];  /* 24 */
    uint64_t out_x[31]; /* 272 */
    uint64_t out_nzcv;  /* 520 */
    uint64_t out_sp;    /* 528 */
};

#define OFF_HOSTSP 0
#define OFF_INNZCV 8
#define OFF_INSP   16
#define OFF_INX    24
#define OFF_OUTX   272
#define OFF_OUTNZ  520
#define OFF_OUTSP  528

static uint32_t *jit;
static size_t jit_size = 1 << 16;
#define TEST_SLOT 64   /* word index of the instruction under test */
#define STUB_AT   1024 /* word index of the stub */
#define LIT_AT    2000 /* literal: &ctx */

static struct ctx g;
static uint8_t stack_area[1 << 16] __attribute__((aligned(64)));

static void emit_frame(uint32_t *c)
{
    int k = 0;
    /* entry: x0 = &ctx */
    c[k++] = 0xA9BF7BFD; /* stp x29, x30, [sp, #-16]! */
    c[k++] = 0xA9BF73FB; /* stp x27, x28, [sp, #-16]! */
    c[k++] = 0xA9BF6BF9; /* stp x25, x26 */
    c[k++] = 0xA9BF63F7; /* stp x23, x24 */
    c[k++] = 0xA9BF5BF5; /* stp x21, x22 */
    c[k++] = 0xA9BF53F3; /* stp x19, x20 */
    c[k++] = 0x910003E1; /* mov x1, sp */
    c[k++] = 0xF9000001; /* str x1, [x0, #0] */
    c[k++] = 0xF9400401; /* ldr x1, [x0, #8] */
    c[k++] = 0xD51B4201; /* msr nzcv, x1 */
    c[k++] = 0xF9400801; /* ldr x1, [x0, #16] */
    c[k++] = 0x9100003F; /* mov sp, x1 */
    /* x1..x30 from in_x[1..30] at 24+8*i, then x0 */
    for (int i = 1; i < 31; i++) {
        c[k++] = 0xF9400000u | ((uint32_t)((OFF_INX + 8 * i) / 8) << 10) | (0u << 5) | (uint32_t)i;
    }
    c[k++] = 0xF9400000u | ((uint32_t)(OFF_INX / 8) << 10) | 0; /* ldr x0, [x0, #24] */
    while (k < TEST_SLOT)
        c[k++] = 0xD503201F;
    k = TEST_SLOT + 1;
    c[k++] = 0xA9205FE0 | 0; /* placeholder, rewritten below */
    k = TEST_SLOT + 1;
    /* stp x0, x1, [sp, #-512] : imm7 = -64 */
    c[k++] = 0xA9000000u | ((uint32_t)(-64 & 0x7F) << 15) | (1u << 10) | (31u << 5) | 0u;
    /* ldr x0, LIT : literal at LIT_AT */
    c[k] = 0x58000000u | ((uint32_t)((LIT_AT - k) & 0x7FFFF) << 5) | 0u;
    k++;
    for (int i = 2; i < 31; i++) {
        c[k++] = 0xF9000000u | ((uint32_t)((OFF_OUTX + 8 * i) / 8) << 10) | (0u << 5) | (uint32_t)i;
    }
    /* ldp x2, x3, [sp, #-512] -> out_x[0], out_x[1] */
    c[k++] = 0xA9400000u | ((uint32_t)(-64 & 0x7F) << 15) | (3u << 10) | (31u << 5) | 2u;
    c[k++] = 0xF9000000u | ((uint32_t)((OFF_OUTX) / 8) << 10) | 2u;
    c[k++] = 0xF9000000u | ((uint32_t)((OFF_OUTX + 8) / 8) << 10) | 3u;
    c[k++] = 0xD53B4202; /* mrs x2, nzcv */
    c[k++] = 0xF9000000u | ((uint32_t)(OFF_OUTNZ / 8) << 10) | 2u;
    c[k++] = 0x910003E2; /* mov x2, sp */
    c[k++] = 0xF9000000u | ((uint32_t)(OFF_OUTSP / 8) << 10) | 2u;
    c[k++] = 0xF9400002; /* ldr x2, [x0, #0] */
    c[k++] = 0x9100005F; /* mov sp, x2 */
    c[k++] = 0xA8C153F3; /* ldp x19, x20, [sp], #16 */
    c[k++] = 0xA8C15BF5;
    c[k++] = 0xA8C163F7;
    c[k++] = 0xA8C16BF9;
    c[k++] = 0xA8C173FB;
    c[k++] = 0xA8C17BFD; /* ldp x29, x30, [sp], #16 */
    c[k++] = 0xD65F03C0; /* ret */
    uint64_t a = (uint64_t)&g;
    memcpy(&c[LIT_AT], &a, 8);
}

static sigjmp_buf jb;
static volatile int faulted;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)si;
    (void)uc;
    faulted = sig;
    siglongjmp(jb, 1);
}

static int run(uint32_t slot_word, const uint32_t *stub, int nstub)
{
    pthread_jit_write_protect_np(0);
    jit[TEST_SLOT] = slot_word;
    for (int i = 0; i < nstub; i++)
        jit[STUB_AT + i] = stub[i];
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(jit, jit_size);
    faulted = 0;
    if (sigsetjmp(jb, 1)) {
        return -1;
    }
    ((void (*)(struct ctx *))jit)(&g);
    return 0;
}

static uint64_t rs64;

static uint64_t rnd(void)
{
    rs64 ^= rs64 << 13;
    rs64 ^= rs64 >> 7;
    rs64 ^= rs64 << 17;
    return rs64;
}

#define MEM_OFF 0x8000 /* target memory inside stack_area: stub spills go below SP, room is needed */
static uint8_t *memp;

struct operands {
    uint32_t rs, rt, rn;
    int size;
    int kind; /* 0 ldop/swp, 1 cas, 2 casp, 3 ldra */
};

static bool decode(uint32_t w, struct operands *o)
{
    enum a64x_class c = a64x_classify(w);
    o->rs = (w >> 16) & 31;
    o->rt = w & 31;
    o->rn = (w >> 5) & 31;
    o->size = (int)(w >> 30);
    switch (c) {
    case A64X_LSE_LDOP:
    case A64X_LSE_SWP:
        o->kind = 0;
        return true;
    case A64X_LSE_CAS:
        o->kind = 1;
        return true;
    case A64X_LSE_CASP:
        o->kind = 2;
        o->size = (w >> 30) & 1 ? 3 : 2;
        return true;
    case A64X_PAC_LDRA:
        o->kind = 3;
        o->size = 3;
        return true;
    default:
        return false;
    }
}

static uint64_t failures, checks, stubs_spill, stubs_dead, words_tested, skipped;
static uint64_t stub_words_total, stub_words_n;

static void setup_state(uint32_t w, const struct operands *o, struct ctx *s, uint8_t *mem_init)
{
    for (int i = 0; i < 31; i++)
        s->in_x[i] = rnd();
    s->in_x[18] = 0; /* x18 is reserved on Darwin */
    s->in_nzcv = (rnd() & 0xF) << 28;
    for (int i = 0; i < 256; i++)
        mem_init[i] = (uint8_t)rnd();
    uint64_t target = (uint64_t)(memp + 64), addr = target;
    if (o->kind == 3) {
        int32_t imm = (int32_t)(((w >> 22) & 1) << 9 | ((w >> 12) & 0x1FF));
        if (imm & 0x200)
            imm -= 0x400;
        addr = target - (int64_t)imm * 8; /* Xn + offset == target */
    }
    uint64_t mv[2];
    memcpy(mv, mem_init + 64, 16);
    int bytes = 1 << (o->size > 3 ? 3 : o->size);
    uint64_t mask = bytes == 8 ? ~0ULL : ((1ULL << (8 * bytes)) - 1);
    if (o->kind == 1 && (rnd() & 1) && o->rs != 31) {
        s->in_x[o->rs] = (rnd() & ~mask) | (mv[0] & mask); /* compare succeeds; garbage above the operand */
    }
    if (o->kind == 2 && (rnd() & 1)) {
        if (o->size == 3) {
            s->in_x[o->rs] = mv[0];
            s->in_x[o->rs + 1] = mv[1];
        } else {
            s->in_x[o->rs] = (uint32_t)mv[0];
            s->in_x[o->rs + 1] = (uint32_t)(mv[0] >> 32);
        }
    }
    if (o->rn == 31) {
        s->in_sp = addr;
    } else {
        s->in_x[o->rn] = addr; /* registers aliasing Rn keep the address */
        s->in_sp = (uint64_t)(memp - 0x1000);
    }
}

static void test_word(uint32_t w, int trials)
{
    struct operands o;
    if (!decode(w, &o)) {
        skipped++;
        return;
    }
    if (o.kind == 2 && ((o.rs & 1) || (o.rt & 1))) {
        skipped++;
        return;
    } /* odd CASP pair: unallocated (data) */
    uint32_t one;
    if (o.kind == 3 && a64x_rewrite1(w, &one)) {
        /* one-word LDRA rewrite: test that one word in place */
    }
    words_tested++;
    for (int t = 0; t < trials; t++) {
        /* mode 0: nothing known dead (spill); mode 1: a random set of uninvolved registers is dead */
        for (int mode = 0; mode < 2; mode++) {
            uint32_t dead = 0;
            if (mode) {
                for (int r = 0; r < 29; r++) {
                    if (r == 18 || r == (int)o.rs || r == (int)o.rt || r == (int)o.rn)
                        continue;
                    if (o.kind == 2 && (r == (int)o.rs + 1 || r == (int)o.rt + 1))
                        continue;
                    if (rnd() & 1)
                        dead |= 1u << r;
                }
            }
            struct ctx s0;
            uint8_t mem_init[256];
            memset(&s0, 0, sizeof(s0));
            setup_state(w, &o, &s0, mem_init);
            /* original */
            uint8_t *rz = (uint8_t *)s0.in_sp - 128;
            memset(memp - 0x1400, 0xA5, 0x1400);
            memcpy(memp, mem_init, 256);
            g = s0;
            int r0 = run(w, NULL, 0);
            struct ctx o0 = g;
            uint8_t mem0[256], rz0[128];
            memcpy(mem0, memp, 256);
            memcpy(rz0, rz, 128);
            /* translated */
            uint32_t stub[A64X_STUB_MAX], site_word;
            int n;
            uint64_t site_pc = (uint64_t)&jit[TEST_SLOT], stub_pc = (uint64_t)&jit[STUB_AT];
            if (o.kind == 3 && a64x_rewrite1(w, &one)) {
                site_word = one;
                n = 0;
            } else {
                n = a64x_stub(w, site_pc, stub_pc, dead, stub, A64X_STUB_MAX);
                if (n < 0 || n > A64X_STUB_MAX) {
                    printf("NOSTUB %08x n=%d\n", w, n);
                    failures++;
                    return;
                }
                site_word = a64x_branch(site_pc, stub_pc);
                if (getenv("A64X_TEST_BREAK")) {
                    /* mutation check: corrupt the first data-processing word of the stub */
                    for (int i = 0; i < n; i++)
                        if ((stub[i] & 0x1F000000u) == 0x0A000000u || (stub[i] & 0x1F200000u) == 0x0B000000u) {
                            stub[i] ^= 1u << 16;
                            break;
                        }
                }
                stub_words_total += (uint64_t)n;
                stub_words_n++;
                if (mode)
                    stubs_dead++;
                else
                    stubs_spill++;
            }
            memset(memp - 0x1400, 0xA5, 0x1400);
            memcpy(memp, mem_init, 256);
            g = s0;
            int r1 = run(site_word, stub, n);
            struct ctx o1 = g;
            checks++;
            bool bad = r0 != r1;
            const char *why = bad ? "fault" : "";
            for (int i = 0; i < 31 && !bad; i++) {
                if (dead & (1u << i))
                    continue;
                if (o0.out_x[i] != o1.out_x[i]) {
                    bad = true;
                    why = "reg";
                }
            }
            if (!bad && o0.out_nzcv != o1.out_nzcv) {
                bad = true;
                why = "nzcv";
            }
            if (!bad && o0.out_sp != o1.out_sp) {
                bad = true;
                why = "sp";
            }
            if (!bad && memcmp(mem0, memp, 256)) {
                bad = true;
                why = "mem";
            }
            /* below the target (SP == target when Rn is SP): only the red zone must be intact; the spill
			 * frame below it may be scribbled on */
            if (!bad && memcmp(rz0, rz, 128)) {
                bad = true;
                why = "redzone";
            }
            if (bad) {
                failures++;
                if (failures < 30) {
                    printf("FAIL %08x mode=%d why=%s dead=%08x n=%d r0=%d r1=%d\n", w, mode, why, dead, n, r0, r1);
                    for (int i = 0; i < 31; i++)
                        if (o0.out_x[i] != o1.out_x[i] && !(dead & (1u << i)))
                            printf("   x%d orig=%016llx xl=%016llx in=%016llx\n", i, o0.out_x[i], o1.out_x[i],
                                   s0.in_x[i]);
                    if (o0.out_nzcv != o1.out_nzcv)
                        printf("   nzcv %llx %llx\n", o0.out_nzcv, o1.out_nzcv);
                    printf("   stub:");
                    for (int i = 0; i < n; i++)
                        printf(" %08x", stub[i]);
                    printf("\n");
                }
                return;
            }
        }
    }
}

static void gen_words(uint32_t *out, int *n)
{
    int k = 0;
    const uint32_t regsets[][3] = {
        /* rs, rt, rn */
        {1, 2, 3}, {4, 4, 5}, {6, 7, 7}, {8, 31, 9}, {31, 10, 11}, {12, 13, 31}, {16, 17, 15}, {0, 0, 0}, {14, 31, 31},
    };
    for (unsigned rsi = 0; rsi < sizeof(regsets) / sizeof(regsets[0]); rsi++) {
        uint32_t rs = regsets[rsi][0], rt = regsets[rsi][1], rn = regsets[rsi][2];
        for (uint32_t size = 0; size < 4; size++) {
            for (uint32_t ar = 0; ar < 4; ar++) {
                /* LD<op> and SWP */
                for (uint32_t op = 0; op < 9; op++) {
                    uint32_t o3 = op == 8, opc = op == 8 ? 0 : op;
                    out[k++] = (size << 30) | 0x38200000u | ((ar >> 1) << 23) | ((ar & 1) << 22) | (rs << 16) |
                               (o3 << 15) | (opc << 12) | (rn << 5) | rt;
                }
                /* CAS: L=bit22 (acquire), o0=bit15 (release) */
                out[k++] =
                    (size << 30) | 0x08A07C00u | ((ar >> 1) << 22) | ((ar & 1) << 15) | (rs << 16) | (rn << 5) | rt;
            }
        }
        /* CASP (even pairs only) */
        if (!(rs & 1) && !(rt & 1) && rs != 30 && rt != 30 && rs != 31 && rt != 31) {
            for (uint32_t sz = 0; sz < 2; sz++)
                for (uint32_t ar = 0; ar < 4; ar++)
                    out[k++] =
                        (sz << 30) | 0x08207C00u | ((ar >> 1) << 22) | ((ar & 1) << 15) | (rs << 16) | (rn << 5) | rt;
        }
    }
    /* LDRA: every offset class, both keys, with and without writeback */
    const int32_t offs[] = {0, 8, 56, 248, 256, 4088, -8, -256, -264, -4096};
    for (unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        for (uint32_t wb = 0; wb < 2; wb++) {
            for (uint32_t m = 0; m < 2; m++) {
                uint32_t imm = (uint32_t)(offs[i] / 8) & 0x3FF;
                uint32_t rnn = wb ? 9 : 31, rtt = 10;
                out[k++] = 0xF8200400u | (m << 23) | ((imm >> 9) << 22) | ((imm & 0x1FF) << 12) | (wb << 11) |
                           (rnn << 5) | rtt;
                out[k++] = 0xF8200400u | (m << 23) | ((imm >> 9) << 22) | ((imm & 0x1FF) << 12) | (wb << 11) |
                           (8u << 5) | (wb ? 11u : 8u);
            }
        }
    }
    *n = k;
}

static volatile uint64_t shared_counter __attribute__((aligned(64)));
static uint32_t *mt_code;

static void *mt_worker(void *arg)
{
    void (*fn)(volatile uint64_t *, uint64_t) = (void (*)(volatile uint64_t *, uint64_t))arg;
    fn(&shared_counter, 1000000);
    return NULL;
}

static int mt_test(uint32_t lse_word, const char *name)
{
    /* fn(x0 = &counter, x1 = n): loop { w2 = 1; <lse x2, x3, [x0]> ; } -- built with the stub */
    uint32_t *c = mt_code;
    pthread_jit_write_protect_np(0);
    int k = 0;
    c[k++] = 0xD2800022; /* mov x2, #1 */
    int loop = k;
    int site = k;
    c[k++] = 0;                                                   /* site */
    c[k++] = 0xF1000421;                                          /* subs x1, x1, #1 */
    c[k] = 0x54000001u | (((uint32_t)(loop - k) & 0x7FFFF) << 5); /* b.ne loop */
    k++;
    c[k++] = 0xD65F03C0;
    uint32_t stub[A64X_STUB_MAX];
    uint64_t site_pc = (uint64_t)&c[site], stub_pc = (uint64_t)&c[256];
    int n = a64x_stub(lse_word, site_pc, stub_pc, 0, stub, A64X_STUB_MAX);
    for (int i = 0; i < n; i++)
        c[256 + i] = stub[i];
    c[site] = a64x_branch(site_pc, stub_pc);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(c, 4096);
    shared_counter = 0;
    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, mt_worker, (void *)c);
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);
    printf("MT %-28s stub %2d words: counter %llu (expect 4000000) %s\n", name, n, shared_counter,
           shared_counter == 4000000 ? "ok" : "WRONG");
    return shared_counter == 4000000 ? 0 : 1;
}

static void random_words(uint32_t *out, int n)
{
    for (int k = 0; k < n;) {
        uint32_t rs = (uint32_t)rnd() % 31, rt = (uint32_t)rnd() % 32, rn = (uint32_t)rnd() % 32,
                 size = (uint32_t)rnd() & 3;
        uint32_t ar = (uint32_t)rnd() & 3, pick = (uint32_t)rnd() % 11;
        if (rs == 18 || rt == 18 || rn == 18)
            continue;
        if (pick < 9) {
            uint32_t o3 = pick == 8, opc = pick == 8 ? 0 : pick;
            out[k++] = (size << 30) | 0x38200000u | ((ar >> 1) << 23) | ((ar & 1) << 22) | (rs << 16) | (o3 << 15) |
                       (opc << 12) | (rn << 5) | rt;
        } else if (pick == 9) {
            out[k++] = (size << 30) | 0x08A07C00u | ((ar >> 1) << 22) | ((ar & 1) << 15) | (rs << 16) | (rn << 5) | rt;
        } else if (!(rs & 1) && !(rt & 1) && rs < 30 && rt < 30 && rs != 18 - 1 && rt != 18 - 1) {
            out[k++] =
                ((size & 1) << 30) | 0x08207C00u | ((ar >> 1) << 22) | ((ar & 1) << 15) | (rs << 16) | (rn << 5) | rt;
        }
    }
}

int lse_semantics_main(int argc, char **argv)
{
    rs64 = 0x9E3779B97F4A7C15ULL;
    jit = mmap(NULL, jit_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    mt_code = mmap(NULL, 1 << 14, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (jit == MAP_FAILED || mt_code == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memp = stack_area + MEM_OFF;
    pthread_jit_write_protect_np(0);
    for (size_t i = 0; i < jit_size / 4; i++)
        jit[i] = 0xD4200000; /* brk #0 */
    emit_frame(jit);
    pthread_jit_write_protect_np(1);
    stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16, .ss_flags = 0};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);

    int trials = argc > 2 ? atoi(argv[2]) : 8;
    if (argc > 1 && !strcmp(argv[1], "--gen")) {
        static uint32_t words[20000];
        int n;
        gen_words(words, &n);
        for (int i = 0; i < n; i++)
            test_word(words[i], trials);
    } else if (argc > 2 && !strcmp(argv[1], "--random")) {
        static uint32_t words[20000];
        int n = atoi(argv[2]);
        if (n > 20000)
            n = 20000;
        trials = argc > 3 ? atoi(argv[3]) : 4;
        random_words(words, n);
        for (int i = 0; i < n; i++)
            test_word(words[i], trials);
    } else if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        char line[256];
        while (f && fgets(line, sizeof(line), f)) {
            uint32_t w = (uint32_t)strtoul(line, NULL, 16);
            test_word(w, trials);
        }
    }
    printf("words %llu, checks %llu (spill %llu, dead-reg %llu), failures %llu, skipped %llu, mean stub %.1f words\n",
           words_tested, checks, stubs_spill, stubs_dead, failures, skipped,
           stub_words_n ? (double)stub_words_total / stub_words_n : 0.0);
    int mt = 0;
    mt += mt_test(0xF8E20003u /* ldaddal x2, x3, [x0] */, "ldaddal x2,x3,[x0]");
    mt += mt_test(0xF8220003u /* ldadd x2, x3, [x0] */, "ldadd x2,x3,[x0] (relaxed)");
    mt += mt_test(0xB8E20003u /* ldaddal w2, w3, [x0] */, "ldaddal w2,w3,[x0]");
    return failures || mt ? 1 : 0;
}
