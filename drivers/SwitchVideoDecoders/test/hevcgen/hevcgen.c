// SPDX-License-Identifier: BSD-2-Clause-Patent
// hevcgen: makes the HEVC test data from FFmpeg (not part of the build; run by hand when the data must change).
//   * reads Annex-B clips encoded once on a Mac (README below) and splits them into access units;
//   * runs FFmpeg's trace_headers bitstream filter over every access unit and records, per slice segment, what the
//     plug-in's parser must find: NAL type, first_slice_segment_in_pic_flag, dependent_slice_segment_flag, slice_type,
//     slice_pic_order_cnt_lsb and sw_hdr_skip_length, the bits from the end of slice_type to the end of the reference
//     picture syntax (pic_output_flag, colour_plane_id, the POC LSBs, the short- and long-term RPS), measured on
//     trace_headers' own bit positions;
//   * decodes every clip with FFmpeg's HEVC decoder (one thread) and records the output order and the CRC-32 of every
//     output picture's luma (8-bit samples; a 10-bit clip's samples shifted right by 2, the 8-bit NV12 NVDEC writes
//     with disp_output_mode 2 and dithering off);
//   * writes ../HEVCClip.h (clips A, V and T: the device clip check and the host tests) and ../HEVCTrace.h (the slice
//     truth and clip K, the kernel's N3 HEVC picture).
// Clips (x265 4.3, FFmpeg 8.1 with VideoToolbox, testsrc2):
//   A 288x160 Main, 20 frames: IDR + open-GOP CRA with RASL leading pictures, 3 B-frames with pyramid, 4 references,
//     explicit RPS in every slice header, 2 slices per picture, default scaling lists, AMP, transform skip, weighted
//     bi-prediction, WPP entry points, CTU 32
//       x265 --profile main --keyint 10 --min-keyint 10 --open-gop --bframes 3 --b-pyramid --ref 4
//            --scaling-list default --slices 2 --amp --rect --tskip --weightb --aq-mode 2 --qg-size 16 --crf 32 --ctu 32
//   V 256x150 Main from Apple's encoder, 14 frames: conformance window (coded 256x160), 4 RPS in the SPS
//       ffmpeg -f lavfi -i testsrc2=size=256x150 -frames:v 14 -c:v hevc_videotoolbox -b:v 100k -g 30 -bf 0
//   T 208x160 Main10, 10 frames, 2 B-frames, CTU 16
//       x265 --input-depth 10 --profile main10 -D 10 --keyint 30 --bframes 2 --ref 2 --crf 32 --ctu 16
//   K 176x144 Main, one IDR, CTU 16, no WPP (x265 --bframes 0 --ref 1 --ctu 16 --crf 26 --no-wpp)
// Homebrew's FFmpeg 8.1.2 links libx265.216 while Homebrew ships x265 4.3 (libx265.217): FFmpeg loads once a one-function
// stand-in dylib for the old name is first on DYLD_LIBRARY_PATH (const void *x265_api_get_216(int) { return 0; }, built
// with -install_name /opt/homebrew/opt/x265/lib/libx265.216.dylib); nothing here encodes with it.
// Build and run (Homebrew FFmpeg; from this directory):
//   clang -std=c11 -O1 -I/opt/homebrew/opt/ffmpeg/include hevcgen.c -L/opt/homebrew/opt/ffmpeg/lib -lavcodec -lavutil
//   ./a.out <dir with clipA.hevc clipV.hevc clipT.hevc clipK.hevc> ..
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/log.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_AUS = 64, MAX_SLICES = 512 };

typedef struct {
    uint8_t *data;
    size_t size;
    size_t au_start[MAX_AUS + 1];
    int aus;
    int width, height, depth;
    int display[MAX_AUS];   // output rank of each access unit, -1 if never output
    uint32_t crc[MAX_AUS];  // by output rank
    int shown;
} Clip;

typedef struct {
    int clip, au, nal_type, first, dependent, slice_type, poc_lsb, skip;
} Slice;

static Slice gSlices[MAX_SLICES];
static int gSliceCount, gClip, gAu;
static struct {
    bool open;
    int nal_type, first, dependent, slice_type, poc_lsb, type_end, skip_end;
} gNal;

static void finishNal(void)
{
    if (!gNal.open) return;
    gNal.open = false;
    if (gNal.nal_type >= 32 || gSliceCount == MAX_SLICES) return;
    Slice *s = &gSlices[gSliceCount++];
    s->clip = gClip, s->au = gAu, s->nal_type = gNal.nal_type, s->first = gNal.first, s->dependent = gNal.dependent;
    s->slice_type = gNal.slice_type, s->poc_lsb = gNal.poc_lsb;
    s->skip = gNal.dependent ? 0 : gNal.skip_end > gNal.type_end ? gNal.skip_end - gNal.type_end : 0;
}

static bool inSkip(const char *name)
{
    static const char *const kNames[] = {"pic_output_flag",
                                         "colour_plane_id",
                                         "slice_pic_order_cnt_lsb",
                                         "short_term_ref_pic_set_sps_flag",
                                         "inter_ref_pic_set_prediction_flag",
                                         "delta_idx_minus1",
                                         "delta_rps_sign",
                                         "abs_delta_rps_minus1",
                                         "used_by_curr_pic_flag",
                                         "use_delta_flag",
                                         "num_negative_pics",
                                         "num_positive_pics",
                                         "delta_poc_s0_minus1",
                                         "used_by_curr_pic_s0_flag",
                                         "delta_poc_s1_minus1",
                                         "used_by_curr_pic_s1_flag",
                                         "short_term_ref_pic_set_idx",
                                         "num_long_term_sps",
                                         "num_long_term_pics",
                                         "lt_idx_sps",
                                         "poc_lsb_lt",
                                         "used_by_curr_pic_lt_flag",
                                         "delta_poc_msb_present_flag",
                                         "delta_poc_msb_cycle_lt"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++)
        if (!strcmp(name, kNames[i])) return true;
    return false;
}

// trace_headers lines: "<bit position> <name>[index] <bits> = <value>" per syntax element.
static void onLog(void *avcl, int level, const char *fmt, va_list vl)
{
    char line[1024];
    int prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix);
    const char *p = strstr(line, "] ");
    p = p ? p + 2 : line;
    int pos = 0, value = 0;
    char name[128], bits[128];
    if (sscanf(p, "%d %127s %127s = %d", &pos, name, bits, &value) != 4) return;
    char *bracket = strchr(name, '[');
    if (bracket) *bracket = 0;
    const int end = pos + (int)strlen(bits);
    if (!strcmp(name, "forbidden_zero_bit")) {
        finishNal();
        memset(&gNal, 0, sizeof(gNal));
        gNal.open = true;
        gNal.slice_type = -1;
        return;
    }
    if (!gNal.open) return;
    if (!strcmp(name, "nal_unit_type"))
        gNal.nal_type = value;
    else if (!strcmp(name, "first_slice_segment_in_pic_flag"))
        gNal.first = value;
    else if (!strcmp(name, "dependent_slice_segment_flag"))
        gNal.dependent = value;
    else if (!strcmp(name, "slice_type"))
        gNal.slice_type = value, gNal.type_end = end;
    if (!strcmp(name, "slice_pic_order_cnt_lsb")) gNal.poc_lsb = value;
    if (gNal.slice_type >= 0 && inSkip(name) && end > gNal.skip_end) gNal.skip_end = end;
}

static bool load(Clip *c, const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    c->size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    c->data = malloc(c->size);
    const bool ok = fread(c->data, 1, c->size, f) == c->size;
    fclose(f);
    if (!ok) return false;
    // Access units: a VCL NAL with first_slice_segment_in_pic_flag starts one; the parameter sets, SEI and AUD before
    // it belong to it.
    size_t pending = 0;
    bool havePending = false;
    for (size_t i = 0; i + 4 < c->size; i++) {
        if (c->data[i] || c->data[i + 1] || c->data[i + 2] != 1) continue;
        size_t start = i && !c->data[i - 1] ? i - 1 : i;
        const int type = (c->data[i + 3] >> 1) & 0x3f;
        if (type >= 32 && type <= 40 && !havePending) pending = start, havePending = true;
        if (type < 32 && (c->data[i + 5] & 0x80)) {
            if (c->aus == MAX_AUS) return false;
            c->au_start[c->aus++] = havePending ? pending : start;
        }
        if (type < 32) havePending = false;
        i += 2;
    }
    c->au_start[c->aus] = c->size;
    return c->aus > 0;
}

static uint32_t crc32Update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static uint32_t lumaCrc(const AVFrame *f, int depth)
{
    uint32_t crc = 0;
    uint8_t row[8192];
    for (int y = 0; y < f->height; y++) {
        const uint8_t *src = f->data[0] + (size_t)y * f->linesize[0];
        if (depth == 8) {
            crc = crc32Update(crc, src, (size_t)f->width);
            continue;
        }
        for (int x = 0; x < f->width; x++) row[x] = (uint8_t)(((const uint16_t *)src)[x] >> 2);
        crc = crc32Update(crc, row, (size_t)f->width);
    }
    return crc;
}

static bool run(Clip *c, int index)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->thread_count = 1;
    const AVBitStreamFilter *trace = av_bsf_get_by_name("trace_headers");
    AVBSFContext *bsf = NULL;
    if (!ctx || avcodec_open2(ctx, codec, NULL) < 0 || !trace || av_bsf_alloc(trace, &bsf) < 0) return false;
    bsf->par_in->codec_id = AV_CODEC_ID_HEVC;
    if (av_bsf_init(bsf) < 0) return false;
    AVPacket *pkt = av_packet_alloc(), *out = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    for (int i = 0; i < c->aus; i++) c->display[i] = -1;
    gClip = index;
    for (int i = 0; i <= c->aus; i++) {
        if (i < c->aus) {
            av_new_packet(pkt, (int)(c->au_start[i + 1] - c->au_start[i]));
            memcpy(pkt->data, c->data + c->au_start[i], (size_t)pkt->size);
            pkt->pts = pkt->dts = i;
            AVPacket *copy = av_packet_clone(pkt);
            gAu = i;
            av_log_set_callback(onLog);
            av_bsf_send_packet(bsf, copy);
            while (av_bsf_receive_packet(bsf, out) == 0) av_packet_unref(out);
            finishNal();
            av_log_set_callback(av_log_default_callback);
            av_packet_free(&copy);
            if (avcodec_send_packet(ctx, pkt) < 0) return false;
            av_packet_unref(pkt);
        } else {
            avcodec_send_packet(ctx, NULL);
        }
        while (avcodec_receive_frame(ctx, frame) == 0) {
            c->width = frame->width, c->height = frame->height;
            c->depth = frame->format == AV_PIX_FMT_YUV420P10LE ? 10 : 8;
            if (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUV420P10LE) return false;
            if (frame->pts < 0 || frame->pts >= c->aus) return false;
            c->display[frame->pts] = c->shown;
            c->crc[c->shown++] = lumaCrc(frame, c->depth);
            av_frame_unref(frame);
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_packet_free(&out);
    av_bsf_free(&bsf);
    avcodec_free_context(&ctx);
    return true;
}

static void bytes(FILE *f, const char *name, const uint8_t *p, size_t n)
{
    fprintf(f, "static const uint8_t %s[%zu] = {", name, n);
    for (size_t i = 0; i < n; i++) fprintf(f, "%s0x%02x,", i % 20 ? "" : "\n    ", p[i]);
    fprintf(f, "\n};\n");
}

static void clipTables(FILE *f, const Clip *c, char letter)
{
    char name[64];
    snprintf(name, sizeof(name), "kHEVCClip%cData", letter);
    bytes(f, name, c->data, c->size);
    fprintf(f, "static const int16_t kHEVCClip%cDisplay[%d] = {", letter, c->aus);
    for (int i = 0; i < c->aus; i++) fprintf(f, "%s%d,", i % 16 ? " " : "\n    ", c->display[i]);
    fprintf(f, "\n};\nstatic const uint32_t kHEVCClip%cCrc[%d] = {", letter, c->shown);
    for (int i = 0; i < c->shown; i++) fprintf(f, "%s0x%08x,", i % 6 ? " " : "\n    ", c->crc[i]);
    fprintf(f, "\n};\n");
}

int main(int argc, char **argv)
{
    if (argc != 3) return fprintf(stderr, "usage: hevcgen <clip dir> <output dir>\n"), 2;
    static const char *const kFiles[4] = {"clipA.hevc", "clipV.hevc", "clipT.hevc", "clipK.hevc"};
    static const char kLetters[4] = {'A', 'V', 'T', 'K'};
    static Clip clips[4];
    for (int i = 0; i < 4; i++) {
        if (!load(&clips[i], argv[1], kFiles[i]) || !run(&clips[i], i))
            return fprintf(stderr, "hevcgen: %s failed\n", kFiles[i]), 1;
        printf("clip %c: %zu bytes, %d access units, %d shown, %dx%d %d-bit\n", kLetters[i], clips[i].size,
               clips[i].aus, clips[i].shown, clips[i].width, clips[i].height, clips[i].depth);
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/HEVCClip.h", argv[2]);
    FILE *f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f,
            "// SPDX-License-Identifier: BSD-2-Clause-Patent\n"
            "// Generated by test/hevcgen (FFmpeg's HEVC decoder is the reference); do not edit.\n"
            "// HEVC clips for the clip check (AVD's ClipCheck.c) and the host tests: Annex-B access units in decode\n"
            "// order, each one's output rank (its presentation index) and the luma CRC-32 of each output picture by\n"
            "// rank (10-bit clips: samples >> 2, NVDEC's 8-bit output with dithering off).\n"
            "#ifndef HEVC_CLIP_H\n#define HEVC_CLIP_H\n#include <stddef.h>\n#include <stdint.h>\n\n"
            "typedef struct {\n    char name;\n    int width, height, depth, aus, shown;\n    const uint8_t *data;\n"
            "    size_t size;\n    const int16_t *display;\n    const uint32_t *crc;\n} HEVCClip;\n\n");
    for (int i = 0; i < 3; i++) clipTables(f, &clips[i], kLetters[i]);
    fprintf(f, "\nstatic const HEVCClip kHEVCClips[3] = {\n");
    for (int i = 0; i < 3; i++)
        fprintf(f,
                "    {'%c', %d, %d, %d, %d, %d, kHEVCClip%cData, sizeof(kHEVCClip%cData), kHEVCClip%cDisplay, "
                "kHEVCClip%cCrc},\n",
                kLetters[i], clips[i].width, clips[i].height, clips[i].depth, clips[i].aus, clips[i].shown, kLetters[i],
                kLetters[i], kLetters[i], kLetters[i]);
    fprintf(f, "};\n\n#endif\n");
    fclose(f);

    snprintf(path, sizeof(path), "%s/HEVCTrace.h", argv[2]);
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(
        f,
        "// SPDX-License-Identifier: BSD-2-Clause-Patent\n"
        "// Generated by test/hevcgen from FFmpeg's trace_headers; do not edit. Per slice segment of clips A, V, T,\n"
        "// K (kHEVCClips order, then K): what the plug-in's slice header parser must find, sw_hdr_skip_length\n"
        "// included (bits from the end of slice_type to the end of the reference picture syntax). Clip K is the\n"
        "// kernel's N3 HEVC picture.\n"
        "#ifndef HEVC_TRACE_H\n#define HEVC_TRACE_H\n#include <stdint.h>\n\n"
        "typedef struct {\n    uint8_t clip, au, nal_type, first, dependent;\n    int8_t slice_type;\n"
        "    uint16_t poc_lsb, skip;\n} HEVCSliceTruth;\n\n"
        "static const HEVCSliceTruth kHEVCSlices[%d] = {\n",
        gSliceCount);
    for (int i = 0; i < gSliceCount; i++) {
        const Slice *s = &gSlices[i];
        fprintf(f, "    {%d, %d, %d, %d, %d, %d, %d, %d},\n", s->clip, s->au, s->nal_type, s->first, s->dependent,
                s->slice_type, s->poc_lsb, s->skip);
    }
    fprintf(f, "};\n\n");
    clipTables(f, &clips[3], 'K');
    fprintf(f, "static const int kHEVCClipKWidth = %d, kHEVCClipKHeight = %d;\n\n#endif\n", clips[3].width,
            clips[3].height);
    fclose(f);
    printf("%d slice segments\n", gSliceCount);
    return 0;
}
