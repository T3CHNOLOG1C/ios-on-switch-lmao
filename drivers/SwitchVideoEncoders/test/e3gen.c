// SPDX-License-Identifier: BSD-2-Clause-Patent
// Generates drivers/TegraNVENC/selftest/E3Vectors.h, the kernel's E3 test vectors, from the code that runs on the
// device, so E3 proves what the plug-in will send:
//   * the two NVENC submits (a 64x64 IDR, then a P picture predicting from it) exactly as AVE's H264NVENC.c builds
//     them, High profile, CABAC, 8x8 transform, QP 26, for the pitch-linear input layout and the OFFSET chroma mode
//     (E3 patches the input layout and chroma mode it is trying);
//   * the two NVDEC submits that decode those pictures, as AVD's H264NVDEC.c builds them from this stream's own SPS
//     and PPS (h264enc_write_sps/pps) and slice headers (h264enc_slice_header), with the buffers they name;
//   * the buffer sizes of both.
// Buffer handles: NVENC 1 input, 2 and 3 reconstructions, 4 bitstream, 5 aux; NVDEC's are its own allocation order,
// numbered from 16. E3 fills each job's stream length and slice offsets in itself.
// Build: cc -I.. -o e3gen e3gen.c ../AVE/H264NVENC.c ../../SwitchVideoDecoders/AVD/H264NVDEC.c
//        ../../SwitchVideoDecoders/AVD/H264Syntax.c; run with no arguments (test:e3gen_test compares
// its output with the committed file).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../AVE/H264NVENC.h"
#include "../../SwitchVideoDecoders/AVD/H264NVDEC.h"
#include "../../TegraNVENC/TegraNVENCCheck.h"

enum { NVDEC_FIRST_HANDLE = 16, MAX_BUFFERS = 16 };

typedef struct {
    uint64_t size[MAX_BUFFERS];
    uint8_t *va[MAX_BUFFERS];
    unsigned count;
    TegraNVDECSubmit submits[2];
    unsigned submitted;
} Fake;

static int fakeAlloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    Fake *f = ctx;
    if (f->count == MAX_BUFFERS) return -1;
    f->size[f->count] = size;
    f->va[f->count] = calloc(1, size);
    *va = f->va[f->count];
    *handle = NVDEC_FIRST_HANDLE + f->count++;
    return 0;
}

static void fakeFree(void *ctx, uint32_t handle)
{
    (void)ctx, (void)handle;
}

static int fakeSubmit(void *ctx, const TegraNVDECSubmit *s, uint32_t *fence)
{
    Fake *f = ctx;
    if (f->submitted < 2) f->submits[f->submitted] = *s;
    *fence = ++f->submitted;
    return 0;
}

static int fakeWait(void *ctx, const TegraNVDECWait *w)
{
    (void)ctx, (void)w;
    return 0;
}

static void *fakeDecoded(void *ctx, const H264Picture *p)
{
    (void)ctx, (void)p;
    return NULL;
}

static void fakeEmit(void *ctx, void *item)
{
    (void)ctx, (void)item;
}

static void dumpBytes(const char *name, const void *data, size_t n)
{
    const uint8_t *p = data;
    printf("static const uint8_t %s[%zu] = {", name, n);
    for (size_t i = 0; i < n; i++) printf("%s0x%02x,", i % 16 ? " " : "\n    ", p[i]);
    printf("\n};\n");
}

static void dumpEntries(const char *name, const void *entries, unsigned count)
{
    const TegraNVENCEntry *e = entries;  // the NVDEC entry has the same shape
    printf("static const E3Entry %s[%u] = {\n", name, count);
    for (unsigned i = 0; i < count; i++)
        printf("    {%#06x, %u, %#x, %#x},\n", e[i].method, e[i].kind, e[i].value, e[i].offset);
    printf("};\n");
}

// One access unit for H264NVDEC: the slice header the NVENC setup asks for, then filler slice data (H264NVDEC parses
// headers only; NVDEC gets NVENC's real slice).
static size_t unit(const H264EncConfig *c, const H264EncPicture *p, uint8_t *out)
{
    uint8_t rbsp[64], nal[96];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    h264enc_slice_header(c, p, &b);
    h264bits_u(&b, 0xa5, 8);
    h264bits_trailing(&b);
    const size_t n = h264enc_nal(p->type == H264ENC_IDR ? 0x65 : 0x61, rbsp, h264bits_size(&b), nal, sizeof(nal));
    out[0] = (uint8_t)(n >> 24), out[1] = (uint8_t)(n >> 16), out[2] = (uint8_t)(n >> 8), out[3] = (uint8_t)n;
    memcpy(out + 4, nal, n);
    return n + 4;
}

int main(void)
{
    H264EncConfig c = {.width = 64,
                       .height = 64,
                       .profile_idc = H264_PROFILE_HIGH,
                       .cabac = true,
                       .transform8x8 = true,
                       .fps_num = 30,
                       .fps_den = 1,
                       .input_layout = TEGRA_NVENC_LAYOUT_PITCH,
                       .chroma_mode = TEGRA_NVENC_CHROMA_OFFSET};
    if (h264enc_configure(&c)) return 1;
    H264EncLayout l;
    h264enc_layout(&c, &l);
    const H264EncHandles idrHandles = {1, 2, 3, 4, 5}, pHandles = {1, 3, 2, 4, 5};
    const H264EncPicture pictures[2] = {
        {.type = H264ENC_IDR, .qp = 26, .frame_num = 0, .idr_pic_id = 0, .poc_lsb = 0, .picture_index = 0},
        {.type = H264ENC_P, .qp = 26, .frame_num = 1, .idr_pic_id = 0, .poc_lsb = 2, .picture_index = 1},
    };
    static TegraNVENCSubmit enc[2];
    h264enc_fill_submit(&c, &l, &pictures[0], &idrHandles, &enc[0]);
    h264enc_fill_submit(&c, &l, &pictures[1], &pHandles, &enc[1]);
    const uint64_t sizes[6] = {0, l.input.bytes, l.recon.bytes, l.recon.bytes, l.bitstream_bytes, l.aux_bytes};
    for (int i = 0; i < 2; i++) {  // the kernel's checker accepts them as built
        uint64_t avail[TEGRA_NVENC_MAX_ENTRIES] = {0};
        for (uint32_t k = 0; k < enc[i].entry_count; k++)
            if (enc[i].entries[k].kind == TEGRA_NVENC_ENTRY_RELOC)
                avail[k] = ((sizes[enc[i].entries[k].value] + 65535) & ~65535ull) - enc[i].entries[k].offset;
        const char *why = tegra_nvenc_check(&enc[i], avail);
        if (why) {
            fprintf(stderr, "e3gen: the checker refuses picture %d: %s\n", i, why);
            return 1;
        }
    }

    Fake fake = {0};
    const H264Backend backend = {&fake,       fakeAlloc, fakeFree, fakeSubmit, fakeWait,
                                 fakeDecoded, fakeEmit,  fakeEmit, NULL};
    H264Decoder *d = h264_decoder_create(&backend);
    uint8_t sps[160], pps[48], au[128];
    const size_t spsSize = h264enc_write_sps(&c, sps, sizeof(sps)), ppsSize = h264enc_write_pps(&c, pps, sizeof(pps));
    if (!d || h264_decoder_parameter_set(d, sps, spsSize) || h264_decoder_parameter_set(d, pps, ppsSize) ||
        h264_decoder_configure(d))
        return 1;
    for (int i = 0; i < 2; i++) {
        const size_t n = unit(&c, &pictures[i], au);
        if (h264_decoder_decode(d, au, n, 4, NULL, false) != 1) {
            fprintf(stderr, "e3gen: H264NVDEC did not take picture %d\n", i);
            return 1;
        }
    }
    if (fake.submitted != 2) return 1;

    printf("// Generated by SwitchVideoEncoders/test/e3gen; do not edit.\n");
    printf("// NVENC: H264NVENC.c, %ux%u High profile level %u CABAC 8x8 QP 26, input layout PITCH, chroma OFFSET.\n",
           c.width, c.height, c.level_idc);
    printf("// NVDEC: H264NVDEC.c over this stream's SPS/PPS and slice headers.\n");
    printf("struct E3Entry {\n    uint16_t method, kind;\n    uint32_t value, offset;\n};\n");
    printf("static const uint32_t kE3Width = %u, kE3Height = %u, kE3Pitch = %u, kE3ChromaOffset = %u;\n", c.width,
           c.height, l.input.pitch, l.input.chroma_offset);
    printf("static const uint32_t kE3EncSizes[6] = {0, %u, %u, %u, %u, %u};  // by NVENC handle\n", l.input.bytes,
           l.recon.bytes, l.recon.bytes, l.bitstream_bytes, l.aux_bytes);
    printf("static const uint32_t kE3EncSetupBytes = %u;\n", enc[0].setup_bytes);
    dumpBytes("kE3EncSetup0", enc[0].setup, enc[0].setup_bytes);
    dumpEntries("kE3EncEntries0", enc[0].entries, enc[0].entry_count);
    dumpBytes("kE3EncSetup1", enc[1].setup, enc[1].setup_bytes);
    dumpEntries("kE3EncEntries1", enc[1].entries, enc[1].entry_count);
    printf("static const uint32_t kE3DecFirstHandle = %u, kE3DecBuffers = %u;\n", NVDEC_FIRST_HANDLE, fake.count);
    printf("static const uint32_t kE3DecSizes[%u] = {", fake.count);
    for (unsigned i = 0; i < fake.count; i++) printf("%s%llu", i ? ", " : "", (unsigned long long)fake.size[i]);
    printf("};\n");
    for (int i = 0; i < 2; i++) {
        const TegraNVDECSubmit *s = &fake.submits[i];
        char name[32];
        snprintf(name, sizeof(name), "kE3DecSetup%d", i);
        dumpBytes(name, s->setup, s->setup_bytes);
        snprintf(name, sizeof(name), "kE3DecEntries%d", i);
        dumpEntries(name, s->entries, s->entry_count);
        printf("static const uint32_t kE3DecClean%d[%u][3] = {", i, s->clean_count);
        for (uint32_t k = 0; k < s->clean_count; k++)
            printf("%s{%u, %#x, %#x}", k ? ", " : "", s->clean[k].handle, s->clean[k].offset, s->clean[k].length);
        printf("};\n");
    }
    return 0;
}
