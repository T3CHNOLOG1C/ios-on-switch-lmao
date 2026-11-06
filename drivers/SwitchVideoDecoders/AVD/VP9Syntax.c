// SPDX-License-Identifier: BSD-3-Clause
// See VP9Syntax.h. Ported from libvpx v1.16.0 (licence in VP9Probs.c); function names follow libvpx's.
#include "VP9Syntax.h"
#include <string.h>

// ---- vpx_dsp/bitreader_buffer: the uncompressed header -------------------------------------------------------------

typedef struct {
    const uint8_t *p;
    size_t size, bit;
    bool overrun;
} BitReader;

static uint32_t rbBit(BitReader *r)  // zeros past the end (as libvpx), still counted
{
    if (r->bit >= r->size * 8) {
        r->overrun = true;
        r->bit++;
        return 0;
    }
    const uint32_t b = (r->p[r->bit >> 3] >> (7 - (r->bit & 7))) & 1;
    r->bit++;
    return b;
}

static uint32_t rbLiteral(BitReader *r, int bits)
{
    uint32_t v = 0;
    while (bits-- > 0)
        v = (v << 1) | rbBit(r);
    return v;
}

static int rbSigned(BitReader *r, int bits)
{
    const int v = (int)rbLiteral(r, bits);
    return rbBit(r) ? -v : v;
}

// ---- vpx_dsp/bitreader: the boolean decoder of the compressed header ----------------------------------------------

typedef struct {
    BitReader in;
    uint32_t value, range;
} BoolDecoder;

static bool boolRead(BoolDecoder *d, uint32_t prob)
{
    const uint32_t split = 1 + (((d->range - 1) * prob) >> 8);
    bool bit;
    if (d->value < split) {
        d->range = split;
        bit = false;
    } else {
        d->range -= split;
        d->value -= split;
        bit = true;
    }
    while (d->range < 128) {
        d->value = (d->value << 1) | rbBit(&d->in);
        d->range <<= 1;
    }
    return bit;
}

static uint32_t boolLiteral(BoolDecoder *d, int bits)
{
    uint32_t v = 0;
    while (bits-- > 0)
        v = (v << 1) | boolRead(d, 128);
    return v;
}

static bool boolInit(BoolDecoder *d, const uint8_t *data, size_t size)
{
    d->in = (BitReader){data, size, 0, false};
    d->value = rbLiteral(&d->in, 8);
    d->range = 255;
    return !boolRead(d, 128);  // marker bit
}

// vp9_dsubexp.c
static int invRecenterNonneg(int v, int m)
{
    if (v > 2 * m)
        return v;
    return (v & 1) ? m - ((v + 1) >> 1) : m + (v >> 1);
}

static int invRemapProb(int v, int m)
{
    static const uint8_t kInvMap[255] = {
        7,   20,  33,  46,  59,  72,  85,  98,  111, 124, 137, 150, 163, 176, 189, 202, 215, 228, 241, 254, 1,   2,
        3,   4,   5,   6,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  21,  22,  23,  24,  25,  26,
        27,  28,  29,  30,  31,  32,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  47,  48,  49,  50,
        51,  52,  53,  54,  55,  56,  57,  58,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  73,  74,
        75,  76,  77,  78,  79,  80,  81,  82,  83,  84,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,  96,  97,
        99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121,
        122, 123, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 138, 139, 140, 141, 142, 143, 144, 145,
        146, 147, 148, 149, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 164, 165, 166, 167, 168, 169,
        170, 171, 172, 173, 174, 175, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 190, 191, 192, 193,
        194, 195, 196, 197, 198, 199, 200, 201, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 216, 217,
        218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240,
        242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 253};
    v = kInvMap[v];
    m--;
    return (m << 1) <= 255 ? 1 + invRecenterNonneg(v, m) : 255 - invRecenterNonneg(v, 255 - 1 - m);
}

static int decodeTermSubexp(BoolDecoder *d)
{
    if (!boolRead(d, 128))
        return (int)boolLiteral(d, 4);
    if (!boolRead(d, 128))
        return (int)boolLiteral(d, 4) + 16;
    if (!boolRead(d, 128))
        return (int)boolLiteral(d, 5) + 32;
    const int v = (int)boolLiteral(d, 7);  // decode_uniform: l = 8, m = 65
    return (v < 65 ? v : (v << 1) - 65 + (int)boolRead(d, 128)) + 64;
}

static void diffUpdate(BoolDecoder *d, uint8_t *p)
{
    if (boolRead(d, 252))
        *p = (uint8_t)invRemapProb(decodeTermSubexp(d), *p);  // DIFF_UPDATE_PROB
}

static void mvUpdate(BoolDecoder *d, uint8_t *p, int n)
{
    for (int i = 0; i < n; i++)
        if (boolRead(d, 252))
            p[i] = (uint8_t)((boolLiteral(d, 7) << 1) | 1);  // MV_UPDATE_PROB
}

// ---- Superframes (VP9 bitstream spec Annex B) ----------------------------------------------------------------------

unsigned vp9_superframe_split(const uint8_t *data, size_t size, size_t offsets[8], size_t sizes[8])
{
    offsets[0] = 0;
    sizes[0] = size;
    if (!size)
        return 0;
    const uint8_t marker = data[size - 1];
    if ((marker & 0xe0) != 0xc0)
        return 1;
    const unsigned frames = (marker & 7) + 1, mag = ((marker >> 3) & 3) + 1;
    const size_t index = 2 + (size_t)mag * frames;
    if (size < index || data[size - index] != marker)
        return 1;
    const uint8_t *p = data + size - index + 1;
    size_t at = 0;
    for (unsigned i = 0; i < frames; i++) {
        size_t n = 0;
        for (unsigned b = 0; b < mag; b++)
            n |= (size_t)*p++ << (8 * b);
        if (n > size - index - at)
            return 0;
        offsets[i] = at;
        sizes[i] = n;
        at += n;
    }
    return frames;
}

// ---- The uncompressed header ---------------------------------------------------------------------------------------

static bool syncCode(BitReader *r)
{
    return rbLiteral(r, 8) == 0x49 && rbLiteral(r, 8) == 0x83 && rbLiteral(r, 8) == 0x42;
}

// read_bitdepth_colorspace_sampling for profile 0: 8-bit; sRGB (4:4:4) is not allowed in profile 0.
static bool colorConfig(BitReader *r, VP9FrameHeader *h)
{
    h->bit_depth = 8;
    h->color_space = (uint8_t)rbLiteral(r, 3);
    if (h->color_space == 7)
        return false;  // VPX_CS_SRGB
    h->full_range = rbBit(r);
    h->subsampled = true;
    return true;
}

static void frameSize(BitReader *r, VP9FrameHeader *h)
{
    h->width = (uint16_t)(rbLiteral(r, 16) + 1);
    h->height = (uint16_t)(rbLiteral(r, 16) + 1);
}

static void renderSize(BitReader *r, VP9FrameHeader *h)
{
    h->render_width = h->width;
    h->render_height = h->height;
    if (rbBit(r)) {
        h->render_width = (uint16_t)(rbLiteral(r, 16) + 1);
        h->render_height = (uint16_t)(rbLiteral(r, 16) + 1);
    }
}

// vp9_setup_past_independence: what an intra-only or error-resilient frame resets.
static void setupPastIndependence(VP9State *s, VP9FrameHeader *h)
{
    memset(s->seg.feature_enabled, 0, sizeof(s->seg.feature_enabled));
    memset(s->seg.feature_data, 0, sizeof(s->seg.feature_data));
    s->seg.abs_delta = false;
    static const int8_t kRef[4] = {1, 0, -1, -1};
    memcpy(s->ref_deltas, kRef, sizeof(kRef));
    s->mode_deltas[0] = s->mode_deltas[1] = 0;
    VP9FrameContext defaults;
    vp9_default_frame_context(&defaults);
    if (h->key_frame || h->error_resilient || h->reset_frame_context == 3) {
        for (int i = 0; i < 4; i++)
            s->contexts[i] = defaults;
        s->contexts_valid = true;
    } else if (h->reset_frame_context == 2) {
        s->contexts[h->frame_context_idx] = defaults;
    }
    memset(h->sign_bias, 0, sizeof(h->sign_bias));
    h->frame_context_idx = 0;
}

static void tileInfo(BitReader *r, VP9FrameHeader *h)
{
    const int sb64Cols = (((h->width + 7) >> 3) + 7) >> 3;  // mi_cols_aligned_to_sb >> MI_BLOCK_SIZE_LOG2
    int minLog2 = 0, maxLog2 = 1;
    while ((64 << minLog2) < sb64Cols)
        minLog2++;  // MAX_TILE_WIDTH_B64
    while ((sb64Cols >> maxLog2) >= 4)
        maxLog2++;  // MIN_TILE_WIDTH_B64
    maxLog2--;
    int ones = maxLog2 - minLog2;
    h->log2_tile_cols = (uint8_t)minLog2;
    while (ones-- > 0 && rbBit(r))
        h->log2_tile_cols++;
    h->log2_tile_rows = (uint8_t)rbBit(r);
    if (h->log2_tile_rows)
        h->log2_tile_rows += (uint8_t)rbBit(r);
}

int vp9_parse_uncompressed(VP9State *s, const uint8_t *data, size_t size, VP9FrameHeader *h)
{
    BitReader r = {data, size, 0, false};
    memset(h, 0, sizeof(*h));
    if (rbLiteral(&r, 2) != 2)
        return -1;  // frame marker
    h->profile = (uint8_t)(rbBit(&r) | rbBit(&r) << 1);
    if (h->profile > 2)
        h->profile += (uint8_t)rbBit(&r);
    if (h->profile)
        return -1;  // NVDEC on T210 decodes profile 0 (8-bit 4:2:0)
    h->show_existing_frame = rbBit(&r);
    if (h->show_existing_frame) {
        h->frame_to_show = (uint8_t)rbLiteral(&r, 3);
        h->show_frame = true;
        h->uncompressed_bytes = (r.bit + 7) >> 3;
        return r.overrun || !s->ref_width[h->frame_to_show] ? -1 : 0;
    }
    h->key_frame = !rbBit(&r);  // KEY_FRAME is 0
    h->show_frame = rbBit(&r);
    h->error_resilient = rbBit(&r);
    if (h->key_frame) {
        if (!syncCode(&r) || !colorConfig(&r, h))
            return -1;
        h->refresh_frame_flags = 0xff;
        frameSize(&r, h);
        renderSize(&r, h);
    } else {
        h->intra_only = h->show_frame ? false : rbBit(&r);
        h->reset_frame_context = h->error_resilient ? 0 : (uint8_t)rbLiteral(&r, 2);
        if (h->intra_only) {
            if (!syncCode(&r))
                return -1;
            h->bit_depth = 8;  // profile 0 intra-only frames are BT.601 studio-range 4:2:0 (normative)
            h->color_space = 2;
            h->subsampled = true;
            h->refresh_frame_flags = (uint8_t)rbLiteral(&r, 8);
            frameSize(&r, h);
            renderSize(&r, h);
        } else {
            if (!s->contexts_valid)
                return -1;  // an inter frame before any key frame
            h->refresh_frame_flags = (uint8_t)rbLiteral(&r, 8);
            for (int i = 0; i < 3; i++) {
                h->ref_idx[i] = (uint8_t)rbLiteral(&r, 3);
                h->sign_bias[1 + i] = rbBit(&r);
                if (!s->ref_width[h->ref_idx[i]])
                    return -1;
            }
            bool found = false;
            for (int i = 0; i < 3 && !found; i++) {
                if (rbBit(&r)) {
                    h->width = s->ref_width[h->ref_idx[i]];
                    h->height = s->ref_height[h->ref_idx[i]];
                    found = true;
                }
            }
            if (!found)
                frameSize(&r, h);
            renderSize(&r, h);
            h->allow_high_precision_mv = rbBit(&r);
            static const uint8_t kLiteralToFilter[4] = {VP9_FILTER_SMOOTH, VP9_FILTER_EIGHTTAP, VP9_FILTER_SHARP,
                                                        VP9_FILTER_BILINEAR};
            h->interp_filter = rbBit(&r) ? VP9_FILTER_SWITCHABLE : kLiteralToFilter[rbLiteral(&r, 2)];
        }
    }
    if (h->width > 4096 || h->height > 4096)
        return -1;
    if (!h->error_resilient) {
        h->refresh_frame_context = rbBit(&r);
        h->frame_parallel = rbBit(&r);
    } else {
        h->refresh_frame_context = false;
        h->frame_parallel = true;
    }
    h->frame_context_idx = (uint8_t)rbLiteral(&r, 2);
    if (h->key_frame || h->intra_only || h->error_resilient)
        setupPastIndependence(s, h);
    if (!s->contexts_valid)
        return -1;  // an intra-only frame that did not reset them, before any key frame

    // setup_loopfilter
    h->lf_level = (uint8_t)rbLiteral(&r, 6);
    h->lf_sharpness = (uint8_t)rbLiteral(&r, 3);
    h->lf_delta_enabled = rbBit(&r);
    if (h->lf_delta_enabled) {
        h->lf_delta_update = rbBit(&r);
        if (h->lf_delta_update) {
            for (int i = 0; i < 4; i++)
                if (rbBit(&r))
                    s->ref_deltas[i] = (int8_t)rbSigned(&r, 6);
            for (int i = 0; i < 2; i++)
                if (rbBit(&r))
                    s->mode_deltas[i] = (int8_t)rbSigned(&r, 6);
        }
    }
    // setup_quantization
    h->base_qindex = (uint8_t)rbLiteral(&r, 8);
    h->y_dc_delta_q = (int8_t)(rbBit(&r) ? rbSigned(&r, 4) : 0);
    h->uv_dc_delta_q = (int8_t)(rbBit(&r) ? rbSigned(&r, 4) : 0);
    h->uv_ac_delta_q = (int8_t)(rbBit(&r) ? rbSigned(&r, 4) : 0);
    h->lossless = !h->base_qindex && !h->y_dc_delta_q && !h->uv_dc_delta_q && !h->uv_ac_delta_q;
    // setup_segmentation
    s->seg.update_map = s->seg.update_data = false;
    s->seg.enabled = rbBit(&r);
    if (s->seg.enabled) {
        s->seg.update_map = rbBit(&r);
        if (s->seg.update_map) {
            for (int i = 0; i < 7; i++)
                s->seg.tree_probs[i] = (uint8_t)(rbBit(&r) ? rbLiteral(&r, 8) : 255);
            s->seg.temporal_update = rbBit(&r);
            for (int i = 0; i < 3; i++)
                s->seg.pred_probs[i] = (uint8_t)(s->seg.temporal_update && rbBit(&r) ? rbLiteral(&r, 8) : 255);
        } else {
            s->seg.temporal_update = false;
        }
        s->seg.update_data = rbBit(&r);
        if (s->seg.update_data) {
            static const int kBits[4] = {8, 6, 2, 0}, kMax[4] = {255, 63, 3, 0};
            static const bool kSigned[4] = {true, true, false, false};
            s->seg.abs_delta = rbBit(&r);
            memset(s->seg.feature_enabled, 0, sizeof(s->seg.feature_enabled));
            memset(s->seg.feature_data, 0, sizeof(s->seg.feature_data));
            for (int i = 0; i < 8; i++) {
                for (int j = 0; j < 4; j++) {
                    int data = 0;
                    if ((s->seg.feature_enabled[i][j] = rbBit(&r))) {
                        data = (int)rbLiteral(&r, kBits[j]);
                        if (data > kMax[j])
                            data = kMax[j];
                        if (kSigned[j] && rbBit(&r))
                            data = -data;
                    }
                    s->seg.feature_data[i][j] = (int16_t)data;
                }
            }
        }
    } else {
        s->seg.temporal_update = false;
    }
    tileInfo(&r, h);
    if (h->log2_tile_cols > 6)
        return -1;
    h->compressed_bytes = rbLiteral(&r, 16);
    h->uncompressed_bytes = (r.bit + 7) >> 3;
    if (r.overrun || !h->compressed_bytes || h->compressed_bytes > size - h->uncompressed_bytes)
        return -1;
    return 0;
}

// ---- The compressed header -----------------------------------------------------------------------------------------

int vp9_parse_compressed(VP9FrameHeader *h, const uint8_t *data, size_t size, VP9FrameContext *fc)
{
    BoolDecoder d;
    if (!boolInit(&d, data, size))
        return -1;
    if (h->lossless) {
        h->tx_mode = VP9_TX_ONLY_4X4;
    } else {
        h->tx_mode = (uint8_t)boolLiteral(&d, 2);
        if (h->tx_mode == VP9_TX_ALLOW_32X32)
            h->tx_mode += (uint8_t)boolRead(&d, 128);
    }
    if (h->tx_mode == VP9_TX_MODE_SELECT) {
        for (int i = 0; i < 2; i++)
            diffUpdate(&d, &fc->tx.p8x8[i][0]);
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                diffUpdate(&d, &fc->tx.p16x16[i][j]);
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 3; j++)
                diffUpdate(&d, &fc->tx.p32x32[i][j]);
    }
    static const int kBiggestTx[5] = {0, 1, 2, 3, 3};
    for (int t = 0; t <= kBiggestTx[h->tx_mode]; t++) {
        if (!boolRead(&d, 128))
            continue;
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 6; k++)
                    for (int l = 0; l < (k ? 6 : 3); l++)
                        for (int m = 0; m < 3; m++)
                            diffUpdate(&d, &fc->coef_probs[t][i][j][k][l][m]);
    }
    for (int k = 0; k < 3; k++)
        diffUpdate(&d, &fc->skip_probs[k]);
    h->reference_mode = VP9_REFERENCE_SINGLE;
    if (!h->key_frame && !h->intra_only) {
        for (int i = 0; i < 7; i++)
            for (int j = 0; j < 3; j++)
                diffUpdate(&d, &fc->inter_mode_probs[i][j]);
        if (h->interp_filter == VP9_FILTER_SWITCHABLE)
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 2; i++)
                    diffUpdate(&d, &fc->switchable_interp_prob[j][i]);
        for (int i = 0; i < 4; i++)
            diffUpdate(&d, &fc->intra_inter_prob[i]);
        // vp9_compound_reference_allowed / read_frame_reference_mode / vp9_setup_compound_reference_mode
        h->compound_allowed = h->sign_bias[2] != h->sign_bias[1] || h->sign_bias[3] != h->sign_bias[1];
        if (h->compound_allowed && boolRead(&d, 128))
            h->reference_mode = boolRead(&d, 128) ? VP9_REFERENCE_SELECT : VP9_REFERENCE_COMPOUND;
        if (h->reference_mode != VP9_REFERENCE_SINGLE) {
            if (h->sign_bias[1] == h->sign_bias[2]) {
                h->comp_fixed_ref = 3, h->comp_var_ref[0] = 1, h->comp_var_ref[1] = 2;
            } else if (h->sign_bias[1] == h->sign_bias[3]) {
                h->comp_fixed_ref = 2, h->comp_var_ref[0] = 1, h->comp_var_ref[1] = 3;
            } else {
                h->comp_fixed_ref = 1, h->comp_var_ref[0] = 2, h->comp_var_ref[1] = 3;
            }
        }
        if (h->reference_mode == VP9_REFERENCE_SELECT)
            for (int i = 0; i < 5; i++)
                diffUpdate(&d, &fc->comp_inter_prob[i]);
        if (h->reference_mode != VP9_REFERENCE_COMPOUND)
            for (int i = 0; i < 5; i++) {
                diffUpdate(&d, &fc->single_ref_prob[i][0]);
                diffUpdate(&d, &fc->single_ref_prob[i][1]);
            }
        if (h->reference_mode != VP9_REFERENCE_SINGLE)
            for (int i = 0; i < 5; i++)
                diffUpdate(&d, &fc->comp_ref_prob[i]);
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 9; i++)
                diffUpdate(&d, &fc->y_mode_prob[j][i]);
        for (int j = 0; j < 16; j++)
            for (int i = 0; i < 3; i++)
                diffUpdate(&d, &fc->partition_prob[j][i]);
        // read_mv_probs
        mvUpdate(&d, fc->nmvc.joints, 3);
        for (int i = 0; i < 2; i++) {
            VP9MVComponent *c = &fc->nmvc.comps[i];
            mvUpdate(&d, &c->sign, 1);
            mvUpdate(&d, c->classes, 10);
            mvUpdate(&d, c->class0, 1);
            mvUpdate(&d, c->bits, 10);
        }
        for (int i = 0; i < 2; i++) {
            VP9MVComponent *c = &fc->nmvc.comps[i];
            for (int j = 0; j < 2; j++)
                mvUpdate(&d, c->class0_fp[j], 3);
            mvUpdate(&d, c->fp, 3);
        }
        if (h->allow_high_precision_mv)
            for (int i = 0; i < 2; i++) {
                mvUpdate(&d, &fc->nmvc.comps[i].class0_hp, 1);
                mvUpdate(&d, &fc->nmvc.comps[i].hp, 1);
            }
    }
    // vpx_reader_has_error: only a header that ran far past its end is corrupt (the decoder reads ahead).
    return d.in.bit > size * 8 + 64 ? -1 : 0;
}
