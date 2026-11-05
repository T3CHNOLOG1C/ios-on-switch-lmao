// SPDX-License-Identifier: BSD-2-Clause-Patent
// vp9gen: makes the VP9 test data from libvpx itself (not part of the build; run by hand when the data must change).
//   * encodes synthetic clips with libvpx's VP9 encoder: A (512x288, 16 frames, alternate reference superframes,
//     variance AQ segmentation, 2 tile columns, backward adaptation on), B (the same with frame-parallel mode: no
//     adaptation, so libvpx's final context of every frame is exactly the one NVDEC receives) and K (one 160x144 key
//     frame for the kernel's N3 VP9 picture);
//   * decodes them frame by frame with libvpx's decoder and records, per frame, the uncompressed header's fields, CRCs
//     of libvpx's entropy context and its four stored contexts after the frame, libvpx's symbol counts converted to
//     NVDEC's nvdec_vp9EntropyCounts_t, and the luma CRC of every shown frame;
//   * writes ../VP9Clip.h (clip A and its CRCs: the device clip check) and ../VP9Trace.h (everything for the host test).
// libvpx's internals are read through its own headers (v1.16.0 source tree) laid out as Homebrew's libvpx 1.16.0 build
// (--enable-vp9-highbitdepth, arm64; vpx_config.h here); the program checks the layout against what it decodes.
// Build and run (from this directory):
//   clang -std=c11 -O1 -I. -I<libvpx-1.16.0 source> -I../../../TegraNVDEC vp9gen.c -L/opt/homebrew/opt/libvpx/lib -lvpx -lz
//   ./a.out ..
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>
#include "vp9/vp9_dx_iface.h"
#include "vp9/decoder/vp9_decoder.h"
#include "nvidia/nvdec_drv.h"

typedef struct {
    uint8_t *data[64];
    size_t size[64];
    int count;
} Packets;

// Moving content with texture, edges and occlusion, so inter prediction, compound references and intra modes all
// get used.
static void synth(vpx_image_t *img, int frame)
{
    const int w = (int)img->d_w, h = (int)img->d_h;
    uint32_t seed = 12345;
    for (int y = 0; y < h; y++) {
        uint8_t *row = img->planes[0] + (size_t)y * img->stride[0];
        for (int x = 0; x < w; x++) {
            seed = seed * 1103515245 + 12345;
            int v = ((x + frame * 3) * 255 / (w + 90)) ^ ((y + frame) & 32 ? 40 : 0);
            v += (int)((seed >> 16) & 7);
            for (int b = 0; b < 3; b++) {
                const int bx = (40 + b * 150 + frame * (b + 2) * 2) % (w - 60),
                          by = 30 + b * 70 + (frame * (3 - b)) % 40;
                if (x >= bx && x < bx + 60 && y >= by && y < by + 50) v = 40 + b * 80 + ((x - bx) ^ (y - by)) % 16;
            }
            row[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    }
    for (int p = 1; p < 3; p++)
        for (int y = 0; y < (h + 1) / 2; y++)
            for (int x = 0; x < (w + 1) / 2; x++)
                img->planes[p][(size_t)y * img->stride[p] + x] =
                    (uint8_t)(128 + (p == 1 ? 1 : -1) * ((x + y + frame) % 48 - 24));
}

static void collect(vpx_codec_ctx_t *enc, Packets *out, vpx_fixed_buf_t *stats)
{
    const vpx_codec_cx_pkt_t *pkt;
    vpx_codec_iter_t it = NULL;
    while ((pkt = vpx_codec_get_cx_data(enc, &it))) {
        if (pkt->kind == VPX_CODEC_STATS_PKT && stats) {
            stats->buf = realloc(stats->buf, stats->sz + pkt->data.twopass_stats.sz);
            memcpy((uint8_t *)stats->buf + stats->sz, pkt->data.twopass_stats.buf, pkt->data.twopass_stats.sz);
            stats->sz += pkt->data.twopass_stats.sz;
        } else if (pkt->kind == VPX_CODEC_CX_FRAME_PKT && out && out->count < 64) {
            out->data[out->count] = malloc(pkt->data.frame.sz);
            memcpy(out->data[out->count], pkt->data.frame.buf, pkt->data.frame.sz);
            out->size[out->count++] = pkt->data.frame.sz;
        }
    }
}

// Two passes, so libvpx places alternate reference frames (hidden, sent in superframes; compound prediction).
static void encode(int w, int h, int frames, int frameParallel, int keyOnly, Packets *out)
{
    vpx_fixed_buf_t stats = {NULL, 0};
    out->count = 0;
    for (int pass = keyOnly ? 1 : 0; pass < 2; pass++) {
        vpx_codec_enc_cfg_t cfg;
        vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &cfg, 0);
        cfg.g_w = (unsigned)w, cfg.g_h = (unsigned)h, cfg.g_timebase.num = 1, cfg.g_timebase.den = 30;
        cfg.rc_target_bitrate = keyOnly ? 200 : 240, cfg.g_lag_in_frames = keyOnly ? 0 : 16, cfg.kf_max_dist = 10;
        cfg.g_threads = 1, cfg.rc_end_usage = VPX_VBR, cfg.g_error_resilient = 0;
        cfg.g_pass = keyOnly ? VPX_RC_ONE_PASS : pass ? VPX_RC_LAST_PASS : VPX_RC_FIRST_PASS;
        if (pass && !keyOnly) cfg.rc_twopass_stats_in = stats;
        vpx_codec_ctx_t enc;
        if (vpx_codec_enc_init(&enc, vpx_codec_vp9_cx(), &cfg, 0)) exit(1);
        vpx_codec_control(&enc, VP8E_SET_CPUUSED, 2);
        vpx_codec_control(&enc, VP8E_SET_ENABLEAUTOALTREF, keyOnly ? 0 : 1);
        vpx_codec_control(&enc, VP9E_SET_AQ_MODE, keyOnly ? 0 : 1);
        vpx_codec_control(&enc, VP9E_SET_TILE_COLUMNS, 1);
        vpx_codec_control(&enc, VP9E_SET_FRAME_PARALLEL_DECODING, frameParallel);
        vpx_image_t *img = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, (unsigned)w, (unsigned)h, 16);
        for (int f = 0; f < frames; f++) {
            synth(img, f);
            if (vpx_codec_encode(&enc, img, f, 1, 0, VPX_DL_GOOD_QUALITY)) exit(2);
            collect(&enc, pass ? out : NULL, pass ? NULL : &stats);
        }
        for (int i = 0; i < 32; i++) {  // drain the look-ahead
            if (vpx_codec_encode(&enc, NULL, frames, 1, 0, VPX_DL_GOOD_QUALITY)) break;
            collect(&enc, pass ? out : NULL, pass ? NULL : &stats);
        }
        vpx_img_free(img);
        vpx_codec_destroy(&enc);
    }
    free(stats.buf);
}

// Superframe index (written independently of AVD's VP9Syntax.c).
static int split(const uint8_t *d, size_t n, size_t off[8], size_t len[8])
{
    off[0] = 0, len[0] = n;
    const uint8_t m = d[n - 1];
    if ((m & 0xe0) != 0xc0) return 1;
    const int frames = (m & 7) + 1, mag = ((m >> 3) & 3) + 1;
    const size_t idx = 2 + (size_t)mag * frames;
    if (n < idx || d[n - idx] != m) return 1;
    size_t at = 0;
    for (int i = 0; i < frames; i++) {
        size_t s = 0;
        for (int b = 0; b < mag; b++) s |= (size_t)d[n - idx + 1 + i * mag + b] << (8 * b);
        off[i] = at, len[i] = s, at += s;
    }
    return frames;
}

// libvpx FRAME_COUNTS -> NVDEC's counter buffer (what the engine writes after a frame; AVD reads it back).
static void toNvdec(const FRAME_COUNTS *c, nvdec_vp9EntropyCounts_t *n)
{
    memset(n, 0, sizeof(*n));
    for (int i = 0; i < 7; i++) {
        n->inter_mode_counts[i][1][0] = c->inter_mode[i][0];  // NEARESTMV
        n->inter_mode_counts[i][2][0] = c->inter_mode[i][1];  // NEARMV
        n->inter_mode_counts[i][0][0] = c->inter_mode[i][2];  // ZEROMV
        n->inter_mode_counts[i][2][1] = c->inter_mode[i][3];  // NEWMV
        n->inter_mode_counts[i][0][1] = c->inter_mode[i][0] + c->inter_mode[i][1] + c->inter_mode[i][3];
        n->inter_mode_counts[i][1][1] = c->inter_mode[i][1] + c->inter_mode[i][3];
    }
    memcpy(n->sb_ymode_counts, c->y_mode, sizeof(n->sb_ymode_counts));
    memcpy(n->uv_mode_counts, c->uv_mode, sizeof(n->uv_mode_counts));
    memcpy(n->partition_counts, c->partition, sizeof(n->partition_counts));
    memcpy(n->switchable_interp_counts, c->switchable_interp, sizeof(n->switchable_interp_counts));
    memcpy(n->intra_inter_count, c->intra_inter, sizeof(n->intra_inter_count));
    memcpy(n->comp_inter_count, c->comp_inter, sizeof(n->comp_inter_count));
    memcpy(n->single_ref_count, c->single_ref, sizeof(n->single_ref_count));
    memcpy(n->comp_ref_count, c->comp_ref, sizeof(n->comp_ref_count));
    memcpy(n->tx32x32_count, c->tx.p32x32, sizeof(n->tx32x32_count));
    memcpy(n->tx16x16_count, c->tx.p16x16, sizeof(n->tx16x16_count));
    memcpy(n->tx8x8_count, c->tx.p8x8, sizeof(n->tx8x8_count));
    memcpy(n->mbskip_count, c->skip, sizeof(n->mbskip_count));
    memcpy(n->nmvcount.joints, c->mv.joints, sizeof(n->nmvcount.joints));
    for (int i = 0; i < 2; i++) {
        const nmv_component_counts *s = &c->mv.comps[i];
        memcpy(n->nmvcount.sign[i], s->sign, sizeof(s->sign));
        memcpy(n->nmvcount.classes[i], s->classes, sizeof(s->classes));
        memcpy(n->nmvcount.class0[i], s->class0, sizeof(s->class0));
        memcpy(n->nmvcount.bits[i], s->bits, sizeof(s->bits));
        memcpy(n->nmvcount.class0_fp[i], s->class0_fp, sizeof(s->class0_fp));
        memcpy(n->nmvcount.fp[i], s->fp, sizeof(s->fp));
        memcpy(n->nmvcount.class0_hp[i], s->class0_hp, sizeof(s->class0_hp));
        memcpy(n->nmvcount.hp[i], s->hp, sizeof(s->hp));
    }
    memcpy(n->countCoeffs, c->coef[0], sizeof(n->countCoeffs));
    memcpy(n->countCoeffs8x8, c->coef[1], sizeof(n->countCoeffs8x8));
    memcpy(n->countCoeffs16x16, c->coef[2], sizeof(n->countCoeffs16x16));
    memcpy(n->countCoeffs32x32, c->coef[3], sizeof(n->countCoeffs32x32));
    memcpy(n->countEobs, c->eob_branch, sizeof(n->countEobs));
}

#define FC_BYTES (offsetof(FRAME_CONTEXT, nmvc) + sizeof(nmv_context))  // 2039: the probabilities, no padding

static void dumpBytes(FILE *f, const char *name, const uint8_t *p, size_t n)
{
    fprintf(f, "static const uint8_t %s[%zu] = {", name, n);
    for (size_t i = 0; i < n; i++) fprintf(f, "%s0x%02x,", i % 20 ? " " : "\n    ", p[i]);
    fprintf(f, "\n};\n");
}

static void dumpPackets(FILE *f, const char *prefix, const Packets *p)
{
    size_t total = 0;
    for (int i = 0; i < p->count; i++) total += p->size[i];
    uint8_t *all = malloc(total);
    size_t at = 0;
    fprintf(f, "static const uint32_t %sSizes[%d] = {", prefix, p->count);
    for (int i = 0; i < p->count; i++) {
        fprintf(f, "%s%zu", i ? ", " : "", p->size[i]);
        memcpy(all + at, p->data[i], p->size[i]);
        at += p->size[i];
    }
    fprintf(f, "};\n");
    char name[64];
    snprintf(name, sizeof(name), "%sData", prefix);
    dumpBytes(f, name, all, total);
    free(all);
}

// Decodes every frame of `p` alone with libvpx; trace records go to `f` (when given), shown luma CRCs to crcs.
static int decode(const Packets *p, FILE *f, const char *prefix, uint32_t *crcs, int w, int h)
{
    vpx_codec_ctx_t dec;
    vpx_codec_dec_cfg_t cfg = {1, 0, 0};
    if (vpx_codec_dec_init(&dec, vpx_codec_vp9_dx(), &cfg, 0)) exit(3);
    int shown = 0, records = 0;
    uLong countsLen = 0;
    if (f) fprintf(f, "static const VP9TraceFrame %sFrames[] = {\n", prefix);
    uint8_t *packedCounts = malloc(64 * 2 * sizeof(nvdec_vp9EntropyCounts_t)), *cursor = packedCounts;
    for (int i = 0; i < p->count; i++) {
        size_t off[8], len[8];
        const int n = split(p->data[i], p->size[i], off, len);
        for (int s = 0; s < n; s++) {
            if (vpx_codec_decode(&dec, p->data[i] + off[s], (unsigned)len[s], NULL, 0)) {
                fprintf(stderr, "libvpx refused packet %d frame %d\n", i, s);
                exit(4);
            }
            const VP9_COMMON *cm = &((vpx_codec_alg_priv_t *)dec.priv)->pbi->common;
            const VP9Decoder *pbi = ((vpx_codec_alg_priv_t *)dec.priv)->pbi;
            if (cm->width != w || cm->height != h || cm->profile != 0) {
                fprintf(stderr, "libvpx layout mismatch: width %d height %d profile %d\n", cm->width, cm->height,
                        cm->profile);
                exit(5);
            }
            if (f) {
                const int adapted =
                    !cm->show_existing_frame && !cm->error_resilient_mode && !cm->frame_parallel_decoding_mode;
                uint32_t contexts = crc32(0, NULL, 0);
                for (int c = 0; c < 4; c++) contexts = crc32(contexts, (const Bytef *)&cm->frame_contexts[c], FC_BYTES);
                uLong countsAt = countsLen;
                if (adapted) {
                    nvdec_vp9EntropyCounts_t nv;
                    toNvdec(&cm->counts, &nv);
                    memcpy(cursor, &nv, sizeof(nv));
                    cursor += sizeof(nv);
                    countsLen += sizeof(nv);
                }
                fprintf(f,
                        "    {%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, "
                        "%d, %#010x, %#010x, %lu},\n",
                        i, s, cm->show_existing_frame, cm->frame_type == KEY_FRAME, cm->show_frame, cm->intra_only,
                        cm->error_resilient_mode, cm->refresh_frame_context, cm->frame_parallel_decoding_mode,
                        cm->frame_context_idx, cm->base_qindex, cm->lf.filter_level, cm->lf.sharpness_level,
                        cm->seg.enabled, cm->seg.update_map, cm->seg.temporal_update, cm->log2_tile_cols,
                        cm->log2_tile_rows, cm->tx_mode, cm->interp_filter, cm->reference_mode,
                        pbi->refresh_frame_flags, adapted,
                        (unsigned)crc32(crc32(0, NULL, 0), (const Bytef *)cm->fc, FC_BYTES), (unsigned)contexts,
                        adapted ? countsAt : 0xfffffffful);
                records++;
            }
            vpx_codec_iter_t it = NULL;
            vpx_image_t *img;
            while ((img = vpx_codec_get_frame(&dec, &it))) {
                uLong crc = crc32(0, NULL, 0);
                for (unsigned y = 0; y < img->d_h; y++)
                    crc = crc32(crc, img->planes[0] + (size_t)y * img->stride[0], img->d_w);
                if (crcs) crcs[shown] = (uint32_t)crc;
                shown++;
            }
        }
    }
    if (f) {
        fprintf(f, "};\n");
        // The counts, zlib-compressed: NVDEC's buffer layout, one nvdec_vp9EntropyCounts_t per adapted frame.
        uLongf packedLen = compressBound(countsLen);
        uint8_t *packed = malloc(packedLen);
        compress2(packed, &packedLen, packedCounts, countsLen, 9);
        char name[64];
        snprintf(name, sizeof(name), "%sCounts", prefix);
        fprintf(f, "static const uint32_t %sCountsBytes = %lu;\n", prefix, countsLen);
        dumpBytes(f, name, packed, packedLen);
        free(packed);
    }
    free(packedCounts);
    vpx_codec_destroy(&dec);
    return shown;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "..";
    static Packets a, b, k;
    encode(512, 288, 16, 0, 0, &a);
    encode(512, 288, 12, 1, 0, &b);
    encode(160, 144, 1, 0, 1, &k);
    char path[512];
    snprintf(path, sizeof(path), "%s/VP9Trace.h", dir);
    FILE *t = fopen(path, "w");
    fprintf(
        t,
        "// Generated by test/vp9gen/vp9gen.c from libvpx %s (encoder and decoder); do not edit.\n"
        "// Per decoded frame: packet, frame in packet, show_existing, key, show, intra_only, error_resilient,\n"
        "// refresh_frame_context, frame_parallel, frame_context_idx, base_qindex, filter level, sharpness,\n"
        "// segmentation enabled, update_map, temporal_update, log2 tile cols, rows, tx_mode, interp_filter,\n"
        "// reference_mode, refresh_frame_flags, adapted, CRC-32 of libvpx's context after the frame (2039\n"
        "// probability bytes), CRC-32 of its four stored contexts, offset of the frame's counts (NVDEC layout) in\n"
        "// the unpacked counts.\n",
        vpx_codec_version_str());
    fprintf(
        t,
        "typedef struct {\n    int packet, frame, show_existing, key, show, intra_only, error_resilient, refresh_ctx,"
        " frame_parallel, ctx_idx, base_q, lf_level, lf_sharpness, seg_enabled, seg_update_map, seg_temporal,"
        " log2_tile_cols, log2_tile_rows, tx_mode, interp_filter, reference_mode, refresh_flags, adapted;\n"
        "    uint32_t fc_crc, contexts_crc;\n    unsigned long counts_at;\n} VP9TraceFrame;\n");
    uint32_t crcA[64], crcK[2];
    decode(&a, t, "kTraceA", NULL, 512, 288);  // its packets are VP9Clip.h's
    dumpPackets(t, "kTraceB", &b);
    decode(&b, t, "kTraceB", NULL, 512, 288);
    dumpPackets(t, "kTraceK", &k);
    decode(&k, NULL, NULL, crcK, 160, 144);
    fprintf(t, "static const uint32_t kTraceKLumaCrc = %#010x;  // libvpx's 160x144 key frame\n", crcK[0]);
    fclose(t);

    const int shown = decode(&a, NULL, NULL, crcA, 512, 288);
    snprintf(path, sizeof(path), "%s/VP9Clip.h", dir);
    FILE *c = fopen(path, "w");
    fprintf(
        c,
        "// Generated by test/vp9gen/vp9gen.c from libvpx %s; do not edit. Clip A of VP9Trace.h: 512x288, %d packets\n"
        "// (alternate reference superframes, segmentation, 2 tile columns), %d shown frames; luma CRC-32 of libvpx's\n"
        "// output for each, in output order.\n",
        vpx_codec_version_str(), a.count, shown);
    fprintf(c,
            "static const int kVP9ClipWidth = 512, kVP9ClipHeight = 288, kVP9ClipPackets = %d, kVP9ClipShown = %d;\n",
            a.count, shown);
    fprintf(c, "static const uint32_t kVP9ClipLumaCrc[%d] = {", shown);
    for (int i = 0; i < shown; i++) fprintf(c, "%s%#010x", i ? ", " : "", crcA[i]);
    fprintf(c, "};\n");
    dumpPackets(c, "kVP9Clip", &a);
    fclose(c);
    printf("clip A %d packets, %d shown; clip B %d packets; key frame %zu bytes\n", a.count, shown, b.count, k.size[0]);
    return 0;
}
