/*
 * a64holes: how much dead space arm64e text has once it is translated for ARMv8.0, and how far each stub-needing
 * site is from enough of it. Scans every executable mapping of a dyld cache (contiguous text, ~110 MB each).
 *
 *   a64holes DIR/dyld_shared_cache_arm64e
 */
#include "../a64x/a64x.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t u32(const uint8_t *p, uint64_t o)
{
    uint32_t v;
    memcpy(&v, p + o, 4);
    return v;
}

static uint64_t u64(const uint8_t *p, uint64_t o)
{
    uint64_t v;
    memcpy(&v, p + o, 8);
    return v;
}

static const uint8_t *mapfile(const char *path, uint64_t *sz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        exit(1);
    }
    struct stat st;
    fstat(fd, &st);
    void *p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    *sz = st.st_size;
    return p;
}

static bool is_auth_check_brk(uint32_t w)
{
    /* brk #0xc470..#0xc473 (ptrauth failure traps emitted after an AUT check) */
    return (w & 0xFFE0001F) == 0xD4200000 && ((w >> 5) & 0xFFFC) == 0xC470;
}

static bool needs_stub(enum a64x_class c)
{
    return c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP || c == A64X_FP16 ||
           c == A64X_FCMA || c == A64X_JSCVT || c == A64X_RDM;
}

/* code-word bitmaps per cache file, from the images' instruction sections */
struct fmap {
    uint64_t addr, size, off;
    int file;
};
static struct fmap fm[4096];
static int nfm;
static uint8_t *codebits[512];

static void mark_code(const uint8_t *m, const uint8_t **fb)
{
    uint32_t img_off = u32(m, 0x1C0), img_cnt = u32(m, 0x1C4);
    for (uint32_t i = 0; i < img_cnt; i++) {
        uint64_t addr = u64(m + img_off + 32 * i, 0);
        int hf = -1;
        uint64_t hoff = 0;
        for (int q = 0; q < nfm; q++)
            if (addr >= fm[q].addr && addr < fm[q].addr + fm[q].size) {
                hf = fm[q].file;
                hoff = fm[q].off + addr - fm[q].addr;
            }
        if (hf < 0)
            continue;
        const uint8_t *mh = fb[hf] + hoff;
        uint32_t ncmds = u32(mh, 16);
        const uint8_t *lc = mh + 32;
        for (uint32_t c = 0; c < ncmds; c++) {
            if (u32(lc, 0) == 0x19) {
                uint32_t ns = u32(lc, 64);
                for (uint32_t k = 0; k < ns; k++) {
                    const uint8_t *sp = lc + 72 + 80 * k;
                    uint64_t sa = u64(sp, 32), ss = u64(sp, 40);
                    if (!(u32(sp, 64) & 0x80000400) || !ss)
                        continue;
                    for (int q = 0; q < nfm; q++)
                        if (sa >= fm[q].addr && sa + ss <= fm[q].addr + fm[q].size) {
                            uint64_t fo = fm[q].off + sa - fm[q].addr;
                            for (uint64_t x = fo / 4; x < (fo + ss) / 4; x++)
                                codebits[fm[q].file][x >> 3] |= 1 << (x & 7);
                        }
                }
            }
            lc += u32(lc, 4);
        }
    }
}

static inline bool iscode(int f, uint64_t fileoff)
{
    uint64_t x = fileoff / 4;
    return codebits[f][x >> 3] & (1 << (x & 7));
}

int a64x_cmd_holes(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: arm64e holes DIR/dyld_shared_cache_arm64e\n");
        return 2;
    }
    const char *mainpath = argv[1];
    uint64_t msz;
    const uint8_t *m = mapfile(mainpath, &msz);
    uint32_t sub_off = u32(m, 0x188), sub_cnt = u32(m, 0x18C);
    uint64_t total_words = 0, brk_checks = 0, check_seq = 0, nop_runs[8] = {0}, zero_runs[8] = {0}, free_words = 0;
    uint64_t sites = 0, far_sites = 0, lr_checks = 0, chunk_sites = 0, chunk_ok = 0, chunks = 0, chunks_short = 0;
    static const uint8_t *fb[512];
    static uint64_t fsz[512];
    for (uint32_t f = 0; f <= sub_cnt; f++) {
        char path[4096];
        if (f == 0)
            snprintf(path, sizeof(path), "%s", mainpath);
        else
            snprintf(path, sizeof(path), "%s%.32s", mainpath, (const char *)(m + sub_off + 56 * (f - 1) + 24));
        fb[f] = mapfile(path, &fsz[f]);
        codebits[f] = calloc(fsz[f] / 32 + 1, 1);
        uint32_t moff = u32(fb[f], 0x10), mcnt = u32(fb[f], 0x14);
        for (uint32_t i = 0; i < mcnt; i++) {
            const uint8_t *e = fb[f] + moff + 32 * i;
            fm[nfm++] = (struct fmap){u64(e, 0), u64(e, 8), u64(e, 16), (int)f};
        }
    }
    mark_code(m, fb);
    for (uint32_t f = 0; f <= sub_cnt; f++) {
        char path[4096];
        if (f == 0)
            snprintf(path, sizeof(path), "%s", mainpath);
        else
            snprintf(path, sizeof(path), "%s%.32s", mainpath, (const char *)(m + sub_off + 56 * (f - 1) + 24));
        uint64_t sz;
        const uint8_t *b = mapfile(path, &sz);
        uint32_t moff = u32(b, 0x10), mcnt = u32(b, 0x14);
        for (uint32_t i = 0; i < mcnt; i++) {
            const uint8_t *e = b + moff + 32 * i;
            if (!(u32(e, 28) & 4))
                continue;
            uint64_t off = u64(e, 16), size = u64(e, 8);
            uint64_t n = size / 4;
            const uint8_t *t = b + off;
            /* free[] marks words usable for stubs after translation */
            uint8_t *freew = calloc(n, 1);
            for (uint64_t k = 0; k < n; k++) {
                uint32_t w = u32(t, k * 4);
                if (!iscode((int)f, off + k * 4))
                    continue;
                total_words++;
                if (is_auth_check_brk(w)) {
                    brk_checks++;
                    /* [autibsp|autiasp] ; eor x16,x30,x30,lsl #1 ; tbz x16,#62,+8 ; brk -> B ok + 3 free words */
                    if (k >= 3) {
                        uint32_t a = u32(t, (k - 3) * 4), e2 = u32(t, (k - 2) * 4), tb = u32(t, (k - 1) * 4);
                        if ((a == 0xD50323FF || a == 0xD50323BF) && e2 == 0xCA1E07D0 && tb == 0xB6F00050) {
                            lr_checks++;
                            freew[k - 2] = freew[k - 1] = freew[k] = 1;
                            free_words += 3;
                        }
                    }
                    /* aut* x16,x17 ; mov x17,x16 ; xpac* x17 ; cmp x16,x17 ; b.eq +8 ; brk  -> 3 free words */
                    if (k >= 5) {
                        uint32_t a = u32(t, (k - 5) * 4), mv = u32(t, (k - 4) * 4), xp = u32(t, (k - 3) * 4);
                        uint32_t cmp = u32(t, (k - 2) * 4), beq = u32(t, (k - 1) * 4);
                        if (a64x_classify(a) == A64X_PAC_DP && a64x_classify(xp) == A64X_PAC_XPAC &&
                            (mv & 0xFFE0FFE0) == 0xAA0003E0 && (cmp & 0xFFE0FC1F) == 0xEB00001F && beq == 0x54000040) {
                            check_seq++;
                            freew[k - 2] = freew[k - 1] = freew[k] = 1; /* after reordering: mov, cmp, b L, [3 free] */
                            free_words += 3;
                        }
                    }
                }
            }
            /* runs of NOP (executed: B over costs a word) and of 0 / UDF (never executed) */
            for (uint64_t k = 0; k < n;) {
                uint32_t w = u32(t, k * 4);
                if ((w == 0xD503201F || w == 0) && iscode((int)f, off + k * 4)) {
                    uint64_t j = k;
                    while (j < n && u32(t, j * 4) == w && iscode((int)f, off + j * 4))
                        j++;
                    uint64_t len = j - k;
                    int bkt = len < 2      ? 0
                              : len < 4    ? 1
                              : len < 8    ? 2
                              : len < 16   ? 3
                              : len < 64   ? 4
                              : len < 256  ? 5
                              : len < 4096 ? 6
                                           : 7;
                    (w ? nop_runs : zero_runs)[bkt]++;
                    uint64_t usable = w ? (len >= 2 ? len - 1 : 0) : len;
                    if (usable >= 3) {
                        for (uint64_t q = j - usable; q < j; q++)
                            freew[q] = 1;
                        free_words += usable;
                    }
                    k = j;
                } else {
                    k++;
                }
            }
            /* distance from each stub site to the nearest free run of >= 3 words */
            int64_t last_free = -1000000000;
            int64_t *next_free = malloc(n * sizeof(int64_t));
            int64_t nf = 1000000000000LL;
            for (int64_t k = (int64_t)n - 1; k >= 0; k--) {
                if (freew[k])
                    nf = k;
                next_free[k] = nf;
            }
            for (uint64_t k = 0; k < n; k++) {
                if (freew[k])
                    last_free = (int64_t)k;
                if (!iscode((int)f, off + k * 4))
                    continue;
                enum a64x_class c = a64x_classify(u32(t, k * 4));
                if (needs_stub(c)) {
                    sites++;
                    int64_t d1 = (int64_t)k - last_free, d2 = next_free[k] - (int64_t)k;
                    int64_t d = d1 < d2 ? d1 : d2;
                    if (d * 4 > (1 << 14))
                        far_sites++;
                }
            }
            /* per 16 KB chunk (by file offset): do the chunk's own free words cover its sites' stubs? */
            for (uint64_t c0 = 0; c0 < n; c0 += 4096) {
                uint64_t c1 = c0 + 4096 < n ? c0 + 4096 : n, fw = 0, need = 0, ns = 0;
                for (uint64_t k = c0; k < c1; k++) {
                    if (freew[k])
                        fw++;
                    if (!iscode((int)f, off + k * 4))
                        continue;
                    enum a64x_class c = a64x_classify(u32(t, k * 4));
                    if (needs_stub(c)) {
                        ns++;
                        need += (c == A64X_FP16 || c == A64X_FCMA) ? 14 : c == A64X_JSCVT ? 24 : 12;
                    }
                }
                if (!ns)
                    continue;
                chunks++;
                chunk_sites += ns;
                /* holes come in 3-word pieces: 2 usable words + a branch each */
                uint64_t usable = fw * 2 / 3;
                if (usable >= need)
                    chunk_ok += ns;
                else {
                    chunks_short++;
                    chunk_ok += ns * usable / need;
                }
            }
            free(next_free);
            free(freew);
        }
    }
    printf("words %" PRIu64 "  auth-check brk %" PRIu64 "  full check sequences %" PRIu64 "\n", total_words, brk_checks,
           check_seq);
    printf("nop runs   1:%" PRIu64 " 2-3:%" PRIu64 " 4-7:%" PRIu64 " 8-15:%" PRIu64 " 16-63:%" PRIu64 " 64-255:%" PRIu64
           " 256-4095:%" PRIu64 " >=4096:%" PRIu64 "\n",
           nop_runs[0], nop_runs[1], nop_runs[2], nop_runs[3], nop_runs[4], nop_runs[5], nop_runs[6], nop_runs[7]);
    printf("zero runs  1:%" PRIu64 " 2-3:%" PRIu64 " 4-7:%" PRIu64 " 8-15:%" PRIu64 " 16-63:%" PRIu64 " 64-255:%" PRIu64
           " 256-4095:%" PRIu64 " >=4096:%" PRIu64 "\n",
           zero_runs[0], zero_runs[1], zero_runs[2], zero_runs[3], zero_runs[4], zero_runs[5], zero_runs[6],
           zero_runs[7]);
    printf("free words (runs >= 3) %" PRIu64 " (%.1f MB)\n", free_words, free_words * 4.0 / 1048576);
    printf("LR-check sequences %" PRIu64 "\n", lr_checks);
    printf("stub sites %" PRIu64 ", farther than 16 KB from a free run: %" PRIu64 "\n", sites, far_sites);
    printf("16K chunks with sites %" PRIu64 " (short of space: %" PRIu64
           "); sites placeable in their own chunk %" PRIu64 " of %" PRIu64 " (%.1f%%)\n",
           chunks, chunks_short, chunk_ok, chunk_sites, 100.0 * chunk_ok / (chunk_sites ? chunk_sites : 1));
    return 0;
}
