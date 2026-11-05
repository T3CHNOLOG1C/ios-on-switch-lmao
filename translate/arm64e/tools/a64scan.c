/*
 * a64scan: inventory of ARMv8.1+ instructions in arm64e code (dyld shared caches and Mach-O files).
 *
 *   a64scan [-u uniq.txt] [-s sample.txt] [-S N] [-i] dsc DIR/dyld_shared_cache_arm64e
 *   a64scan [-u uniq.txt] [-s sample.txt] [-S N] [-i] macho FILE...
 *
 * -u: every distinct flagged word with its class and count (for the LLVM oracle)
 * -s: a 1-in-N sample (-S, default 64) of all distinct code words (to look for classes we miss)
 * -i: per-image counts
 * -A: also scan plain arm64 slices (e.g. our own A57 kernel: everything flagged there is a build mistake)
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

#define S_ATTR_PURE_INSTRUCTIONS 0x80000000u
#define S_ATTR_SOME_INSTRUCTIONS 0x00000400u
#define LC_SEGMENT_64            0x19u

static uint64_t g_total[A64X_NCLASSES], g_words, g_pachint;
static int opt_images;
static FILE *f_uniq, *f_sample;
static unsigned sample_n = 64;

/* open-addressing set of words -> (class, count) */
struct ent {
    uint32_t w;
    uint32_t n;
};
static struct ent *uniq;
static size_t uniq_cap = 1u << 22, uniq_len;
static struct ent *samp;
static size_t samp_cap = 1u << 24, samp_len;

static void set_add(struct ent **tab, size_t *cap, size_t *len, uint32_t w)
{
    if (*len * 2 >= *cap) {
        size_t ncap = *cap * 2;
        struct ent *nt = calloc(ncap, sizeof(*nt));
        for (size_t i = 0; i < *cap; i++) {
            if ((*tab)[i].n) {
                size_t h = ((*tab)[i].w * 2654435761u) & (ncap - 1);
                while (nt[h].n) h = (h + 1) & (ncap - 1);
                nt[h] = (*tab)[i];
            }
        }
        free(*tab);
        *tab = nt;
        *cap = ncap;
    }
    size_t h = (w * 2654435761u) & (*cap - 1);
    while ((*tab)[h].n && (*tab)[h].w != w) h = (h + 1) & (*cap - 1);
    if (!(*tab)[h].n) {
        (*tab)[h].w = w;
        (*len)++;
    }
    (*tab)[h].n++;
}

/* per-16K-page statistics: pages holding baseline classes, and stub-needing sites per page */
static uintptr_t pg_cur = (uintptr_t)-1;
static unsigned pg_any, pg_stub;
static uint64_t pg_total, pg_with_any, pg_with_stub, pg_hist[8];
static unsigned pg_max_stub;
static void pg_flush(void)
{
    if (pg_cur == (uintptr_t)-1) return;
    pg_total++;
    if (pg_any) pg_with_any++;
    if (pg_stub) {
        pg_with_stub++;
        int b = pg_stub <= 1     ? 0
                : pg_stub <= 2   ? 1
                : pg_stub <= 4   ? 2
                : pg_stub <= 8   ? 3
                : pg_stub <= 16  ? 4
                : pg_stub <= 64  ? 5
                : pg_stub <= 256 ? 6
                                 : 7;
        pg_hist[b]++;
    }
    if (pg_stub > pg_max_stub) pg_max_stub = pg_stub;
    pg_any = pg_stub = 0;
}
static bool needs_stub(enum a64x_class c)
{
    return c == A64X_LSE_CAS || c == A64X_LSE_CASP || c == A64X_LSE_LDOP || c == A64X_LSE_SWP || c == A64X_FP16 ||
           c == A64X_FCMA || c == A64X_JSCVT || c == A64X_RDM;
}

static void scan_words(const uint8_t *p, uint64_t size, uint64_t *img)
{
    uint64_t n = size / 4;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t w;
        memcpy(&w, p + i * 4, 4);
        g_words++;
        uintptr_t pg = ((uintptr_t)(p + i * 4)) >> 14;
        if (pg != pg_cur) {
            pg_flush();
            pg_cur = pg;
        }
        enum a64x_class c = a64x_classify(w);
        if (c != A64X_OK && a64x_class_is_baseline(c)) {
            pg_any++;
            if (needs_stub(c)) pg_stub++;
        }
        if (c != A64X_OK) {
            g_total[c]++;
            if (img) img[c]++;
            if (f_uniq) set_add(&uniq, &uniq_cap, &uniq_len, w);
        } else if (a64x_is_pac_hint(w)) {
            g_pachint++;
        }
        if (f_sample && ((w * 2654435761u) >> 8) % sample_n == 0) {
            set_add(&samp, &samp_cap, &samp_len, w);
        }
    }
}

static void print_image(const char *name, const uint64_t *img)
{
    uint64_t t = 0;
    for (int c = 1; c < A64X_NCLASSES; c++) t += img[c];
    if (!t) return;
    printf("IMG %-60s", name);
    for (int c = 1; c < A64X_NCLASSES; c++) {
        if (img[c]) printf(" %s=%" PRIu64, a64x_class_name(c), img[c]);
    }
    printf("\n");
}

struct map {
    uint64_t addr, size, off;
    uint32_t prot;
    int file;
};
static struct map maps[4096];
static int nmaps;
static const uint8_t *fbase[512];
static uint64_t fsize[512];
static int nfiles;

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
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    *sz = st.st_size;
    return p;
}

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

static const uint8_t *vm2ptr(uint64_t addr, uint64_t len)
{
    for (int i = 0; i < nmaps; i++) {
        if (addr >= maps[i].addr && addr + len <= maps[i].addr + maps[i].size) {
            return fbase[maps[i].file] + maps[i].off + (addr - maps[i].addr);
        }
    }
    return NULL;
}

static void add_cache_file(const char *path)
{
    uint64_t sz;
    const uint8_t *b = mapfile(path, &sz);
    if (memcmp(b, "dyld_v1", 7)) {
        fprintf(stderr, "%s: not a cache\n", path);
        exit(1);
    }
    int fi = nfiles++;
    fbase[fi] = b;
    fsize[fi] = sz;
    uint32_t moff = u32(b, 0x10), mcnt = u32(b, 0x14);
    for (uint32_t i = 0; i < mcnt; i++) {
        const uint8_t *e = b + moff + 32 * i;
        maps[nmaps++] = (struct map){u64(e, 0), u64(e, 8), u64(e, 16), u32(e, 28), fi};
    }
}

static void scan_dsc(const char *mainpath)
{
    add_cache_file(mainpath);
    const uint8_t *m = fbase[0];
    uint32_t sub_off = u32(m, 0x188), sub_cnt = u32(m, 0x18C);
    char path[4096];
    for (uint32_t i = 0; i < sub_cnt; i++) {
        const char *suffix = (const char *)(m + sub_off + 56 * i + 24);
        snprintf(path, sizeof(path), "%s%.32s", mainpath, suffix);
        add_cache_file(path);
    }
    /* TEXT_STUBS mappings (iOS 26: cross-image stubs) from mappingWithSlide */
    uint64_t stub_words = 0;
    uint64_t stubimg[A64X_NCLASSES] = {0};
    for (int fi = 0; fi < nfiles; fi++) {
        const uint8_t *b = fbase[fi];
        uint32_t so = u32(b, 0x138), sc = u32(b, 0x13C);
        for (uint32_t i = 0; i < sc; i++) {
            const uint8_t *e = b + so + 56 * i;
            uint64_t flags = u64(e, 40);
            if (flags & (1u << 3)) { /* DYLD_CACHE_MAPPING_TEXT_STUBS */
                scan_words(b + u64(e, 16), u64(e, 8), stubimg);
                stub_words += u64(e, 8) / 4;
            }
        }
    }
    if (opt_images) print_image("[TEXT_STUBS mappings]", stubimg);
    uint32_t img_off = u32(m, 0x1C0), img_cnt = u32(m, 0x1C4);
    fprintf(stderr, "%d cache files, %d mappings, %u images, %" PRIu64 " stub words\n", nfiles, nmaps, img_cnt,
            stub_words);
    for (uint32_t i = 0; i < img_cnt; i++) {
        const uint8_t *e = m + img_off + 32 * i;
        uint64_t addr = u64(e, 0);
        const char *name = (const char *)(m + u32(e, 24));
        const uint8_t *mh = vm2ptr(addr, 32);
        if (!mh || u32(mh, 0) != 0xFEEDFACF) {
            fprintf(stderr, "bad image %s\n", name);
            continue;
        }
        uint32_t ncmds = u32(mh, 16);
        const uint8_t *lc = mh + 32;
        uint64_t img[A64X_NCLASSES] = {0};
        for (uint32_t c = 0; c < ncmds; c++) {
            uint32_t cmd = u32(lc, 0), csz = u32(lc, 4);
            if (cmd == LC_SEGMENT_64) {
                uint32_t nsects = u32(lc, 64);
                for (uint32_t s = 0; s < nsects; s++) {
                    const uint8_t *sp = lc + 72 + 80 * s;
                    uint64_t saddr = u64(sp, 32), ssize = u64(sp, 40);
                    uint32_t flags = u32(sp, 64);
                    if ((flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) && ssize) {
                        const uint8_t *p = vm2ptr(saddr, ssize);
                        if (!p) {
                            fprintf(stderr, "%s: section not mapped\n", name);
                            continue;
                        }
                        scan_words(p, ssize, img);
                    }
                }
            }
            lc += csz;
        }
        if (opt_images) print_image(name, img);
    }
}

static void scan_macho_slice(const char *name, const uint8_t *mh, uint64_t len)
{
    if (len < 32 || u32(mh, 0) != 0xFEEDFACF) return;
    uint32_t ncmds = u32(mh, 16);
    const uint8_t *lc = mh + 32;
    uint64_t img[A64X_NCLASSES] = {0};
    for (uint32_t c = 0; c < ncmds; c++) {
        uint32_t cmd = u32(lc, 0), csz = u32(lc, 4);
        if (cmd == LC_SEGMENT_64) {
            uint32_t nsects = u32(lc, 64);
            for (uint32_t s = 0; s < nsects; s++) {
                const uint8_t *sp = lc + 72 + 80 * s;
                uint64_t ssize = u64(sp, 40);
                uint32_t soff = u32(sp, 48), flags = u32(sp, 64);
                if ((flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) && ssize && soff &&
                    soff + ssize <= len) {
                    scan_words(mh + soff, ssize, img);
                }
            }
        }
        lc += csz;
    }
    if (opt_images) print_image(name, img);
}

static int opt_any_arm64;

static void scan_macho(const char *path)
{
    uint64_t sz;
    const uint8_t *b = mapfile(path, &sz);
    if (sz < 32) return;
    uint32_t magic = u32(b, 0);
    if (magic == 0xBEBAFECA) { /* FAT_CIGAM: big-endian header */
        uint32_t n = __builtin_bswap32(u32(b, 4));
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *a = b + 8 + 20 * i;
            uint32_t cputype = __builtin_bswap32(u32(a, 0)), sub = __builtin_bswap32(u32(a, 4));
            uint32_t off = __builtin_bswap32(u32(a, 8)), len = __builtin_bswap32(u32(a, 12));
            if (cputype == 0x0100000C && ((sub & 0xFF) == 2 || opt_any_arm64) && off + (uint64_t)len <= sz) {
                scan_macho_slice(path, b + off, len);
            }
        }
    } else if (magic == 0xFEEDFACF && u32(b, 4) == 0x0100000C && ((u32(b, 8) & 0xFF) == 2 || opt_any_arm64)) {
        scan_macho_slice(path, b, sz);
    }
}

static int cmp_ent(const void *a, const void *b)
{
    const struct ent *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : (x->w > y->w) - (x->w < y->w);
}

int a64x_cmd_scan(int argc, char **argv)
{
    int ch;
    while ((ch = getopt(argc, argv, "u:s:S:iA")) != -1) {
        switch (ch) {
        case 'u':
            f_uniq = fopen(optarg, "w");
            uniq = calloc(uniq_cap, sizeof(*uniq));
            break;
        case 's':
            f_sample = fopen(optarg, "w");
            samp = calloc(samp_cap, sizeof(*samp));
            break;
        case 'S': sample_n = atoi(optarg); break;
        case 'i': opt_images = 1; break;
        case 'A': opt_any_arm64 = 1; break;
        default: return 2;
        }
    }
    argc -= optind;
    argv += optind;
    if (argc < 2) {
        fprintf(stderr, "usage: a64scan [-u F] [-s F] [-S N] [-i] dsc MAIN | macho FILE...\n");
        return 2;
    }
    if (!strcmp(argv[0], "dsc")) {
        scan_dsc(argv[1]);
    } else {
        for (int i = 1; i < argc; i++) scan_macho(argv[i]);
    }
    pg_flush();
    printf("TOTAL words=%" PRIu64 " pac_hints(nop on v8.0)=%" PRIu64 "\n", g_words, g_pachint);
    printf("PAGES16K code=%" PRIu64 " with_baseline_class=%" PRIu64 " with_stub_sites=%" PRIu64
           " max_stub_sites_per_page=%u\n",
           pg_total, pg_with_any, pg_with_stub, pg_max_stub);
    printf("STUBHIST 1:%" PRIu64 " 2:%" PRIu64 " 3-4:%" PRIu64 " 5-8:%" PRIu64 " 9-16:%" PRIu64 " 17-64:%" PRIu64
           " 65-256:%" PRIu64 " >256:%" PRIu64 "\n",
           pg_hist[0], pg_hist[1], pg_hist[2], pg_hist[3], pg_hist[4], pg_hist[5], pg_hist[6], pg_hist[7]);
    for (int c = 1; c < A64X_NCLASSES; c++) {
        if (g_total[c])
            printf("CLASS %-16s %10" PRIu64 "%s\n", a64x_class_name(c), g_total[c],
                   a64x_class_is_baseline(c) ? "  [A12 baseline]" : "  [A13+, needs a feature check]");
    }
    if (f_uniq) {
        struct ent *v = malloc(uniq_len * sizeof(*v));
        size_t k = 0;
        for (size_t i = 0; i < uniq_cap; i++)
            if (uniq[i].n) v[k++] = uniq[i];
        qsort(v, k, sizeof(*v), cmp_ent);
        for (size_t i = 0; i < k; i++)
            fprintf(f_uniq, "%08x %s %u\n", v[i].w, a64x_class_name(a64x_classify(v[i].w)), v[i].n);
        fclose(f_uniq);
    }
    if (f_sample) {
        for (size_t i = 0; i < samp_cap; i++)
            if (samp[i].n) fprintf(f_sample, "%08x %u\n", samp[i].w, samp[i].n);
        fclose(f_sample);
    }
    return 0;
}
