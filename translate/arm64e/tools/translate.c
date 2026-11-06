#include "commands.h"

#include "../macho/macho.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SLICES 16

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }
    uint8_t *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n >= 0 && fseek(f, 0, SEEK_SET) == 0) {
            buf = malloc((size_t)n + 1);
            if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
                free(buf);
                buf = NULL;
            }
            *size = (size_t)n;
        }
    }
    fclose(f);
    if (!buf)
        fprintf(stderr, "%s: cannot read\n", path);
    return buf;
}

static int spill(const char *path, const void *buf, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, size, f) != size || fclose(f)) {
        perror(path);
        return -1;
    }
    return 0;
}

static void report(const struct a64x_macho *m, const struct a64x_image_xlate *x, long sites)
{
    printf("%s subtype 0x%x: ", a64x_kind_name(x->kind), m->cpusubtype);
    if (x->kind == A64X_KIND_NATIVE || x->kind == A64X_KIND_MARKED) {
        printf("mapped as is\n");
        return;
    }
    printf("%" PRIu64 " words changed, %u stub words for %ld sites at +0x%" PRIx64 " (address 0x%" PRIx64 "), %" PRIu64
           " data words\n",
           x->changed, x->nstubs, sites, (uint64_t)x->stub_delta, m->exec_vmaddr + (uint64_t)x->stub_delta,
           x->island_words);
    for (int c = 1; c < A64X_NCLASSES; c++) {
        if (x->stats.rewrite1[c] || x->stats.stubs[c] || x->stats.left[c])
            printf("  %-16s 1:1 %-8" PRIu64 " stub %-6" PRIu64 " left %" PRIu64 "\n",
                   a64x_class_name((enum a64x_class)c), x->stats.rewrite1[c], x->stats.stubs[c], x->stats.left[c]);
    }
}

int a64x_cmd_translate(int argc, char **argv)
{
    uint32_t map_page = 4096, flags = 0;
    int ch;
    while ((ch = getopt(argc, argv, "km:")) != -1) {
        if (ch == 'k')
            flags |= A64X_MAP_KERNEL;
        else if (ch == 'm')
            map_page = (uint32_t)strtoul(optarg, NULL, 0);
        else
            return 2;
    }
    if (argc - optind < 2 || argc - optind > 3) {
        fprintf(stderr, "usage: arm64e translate [-k] [-m MAP_PAGE] IN OUT [STUBS]\n");
        return 2;
    }
    const char *in = argv[optind], *outp = argv[optind + 1], *stubp = argc - optind == 3 ? argv[optind + 2] : NULL;
    size_t size = 0;
    uint8_t *file = slurp(in, &size);
    if (!file)
        return 1;
    struct a64x_slice slices[MAX_SLICES];
    int n = a64x_macho_slices(file, size, slices, MAX_SLICES);
    if (n < 0) {
        fprintf(stderr, "%s: not a Mach-O\n", in);
        free(file);
        return 1;
    }
    uint8_t *out = malloc(size ? size : 1);
    struct a64x_macho *m = malloc(sizeof(*m));
    int rc = out && m ? 0 : 1;
    if (out)
        memcpy(out, file, size);
    int with_stubs = 0;
    for (int i = 0; i < n && !rc; i++) {
        char err[256];
        struct a64x_image_xlate x;
        if (a64x_macho_parse(file + slices[i].offset, slices[i].size, m, err, sizeof(err)) ||
            a64x_translate_image(m, map_page, flags, out + slices[i].offset, &x, err, sizeof(err))) {
            fprintf(stderr, "%s: slice %d: %s\n", in, i, err);
            rc = 1;
            break;
        }
        long sites = a64x_verify_image(m, out + slices[i].offset, &x, err, sizeof(err));
        if (sites < 0) {
            fprintf(stderr, "%s: slice %d: verification failed: %s\n", in, i, err);
            rc = 1;
        } else {
            report(m, &x, sites);
        }
        if (!rc && x.nstubs) {
            if (!stubp || with_stubs++) {
                fprintf(stderr, "%s: slice %d has %u stub words: give one STUBS file for one such slice\n", in, i,
                        x.nstubs);
                rc = 1;
            } else {
                rc = spill(stubp, x.stubs, (size_t)x.nstubs * 4) ? 1 : 0;
            }
        }
        a64x_image_xlate_free(&x);
    }
    if (!rc)
        rc = spill(outp, out, size) ? 1 : 0;
    if (!rc && memcmp(out, file, size))
        printf("note: the code signature no longer matches the translated pages (re-sign, or load the original "
               "through the a64x kernel)\n");
    free(m);
    free(out);
    free(file);
    return rc;
}

int a64x_cmd_fixups(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: arm64e fixups IN [OUT]\n");
        return 2;
    }
    size_t size = 0;
    uint8_t *file = slurp(argv[1], &size);
    if (!file)
        return 1;
    struct a64x_slice slices[MAX_SLICES];
    int n = a64x_macho_slices(file, size, slices, MAX_SLICES);
    uint8_t *out = malloc(size ? size : 1);
    struct a64x_macho *m = malloc(sizeof(*m));
    int rc = n >= 0 && out && m ? 0 : 1;
    if (out)
        memcpy(out, file, size);
    for (int i = 0; i < n && !rc; i++) {
        char err[256];
        struct a64x_fixup_stats st;
        if (a64x_macho_parse(file + slices[i].offset, slices[i].size, m, err, sizeof(err)) ||
            a64x_fixups_strip_auth(m, out + slices[i].offset, &st, err, sizeof(err))) {
            fprintf(stderr, "%s: slice %d: %s\n", argv[1], i, err);
            rc = 1;
            break;
        }
        printf("slice %d: %" PRIu64 " rebases (%" PRIu64 " authenticated), %" PRIu64 " binds (%" PRIu64
               " authenticated), %" PRIu64 " written plain\n",
               i, st.rebases, st.auth_rebases, st.binds, st.auth_binds, st.stripped);
    }
    if (!rc && argc == 3)
        rc = spill(argv[2], out, size) ? 1 : 0;
    free(m);
    free(out);
    free(file);
    return rc;
}
