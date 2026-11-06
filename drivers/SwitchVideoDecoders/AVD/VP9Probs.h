// SPDX-License-Identifier: BSD-3-Clause
// VP9 entropy contexts: the defaults, the compressed header's delta updates and the backward adaptation a decoder runs
// after each frame from the frame's symbol counts. On Tegra the driver owns these (NVDEC takes the probabilities of a
// frame and writes back its counts), so they live here in plain C, ported from libvpx v1.16.0 (vp9_entropy.c,
// vp9_entropymode.c, vp9_entropymv.c, vpx_dsp/prob.[ch], vp9_dsubexp.c; licence in VP9Probs.c). The structures keep
// libvpx's field order (FRAME_CONTEXT, FRAME_COUNTS) so the host test compares them byte for byte with libvpx's own.
#ifndef VP9_PROBS_H
#define VP9_PROBS_H
#include <stdbool.h>
#include <stdint.h>

typedef uint8_t VP9CoefProbs[2][6][6][3];    // [ref type][band][context][node]
typedef uint32_t VP9CoefCounts[2][6][6][4];  // + EOB_MODEL_TOKEN

typedef struct {
    uint8_t sign;
    uint8_t classes[10];
    uint8_t class0[1];
    uint8_t bits[10];
    uint8_t class0_fp[2][3];
    uint8_t fp[3];
    uint8_t class0_hp;
    uint8_t hp;
} VP9MVComponent;

// libvpx FRAME_CONTEXT without its trailing `initialized`: 2039 probability bytes in the same order.
typedef struct {
    uint8_t y_mode_prob[4][9];
    uint8_t uv_mode_prob[10][9];
    uint8_t partition_prob[16][3];
    VP9CoefProbs coef_probs[4][2];  // [tx size][plane type]
    uint8_t switchable_interp_prob[4][2];
    uint8_t inter_mode_probs[7][3];
    uint8_t intra_inter_prob[4];
    uint8_t comp_inter_prob[5];
    uint8_t single_ref_prob[5][2];
    uint8_t comp_ref_prob[5];

    struct {
        uint8_t p32x32[2][3];
        uint8_t p16x16[2][2];
        uint8_t p8x8[2][1];
    } tx;

    uint8_t skip_probs[3];

    struct {
        uint8_t joints[3];
        VP9MVComponent comps[2];
    } nmvc;
} VP9FrameContext;

typedef struct {
    uint32_t sign[2];
    uint32_t classes[11];
    uint32_t class0[2];
    uint32_t bits[10][2];
    uint32_t class0_fp[2][4];
    uint32_t fp[4];
    uint32_t class0_hp[2];
    uint32_t hp[2];
} VP9MVComponentCounts;

// libvpx FRAME_COUNTS, same layout.
typedef struct {
    uint32_t y_mode[4][10];
    uint32_t uv_mode[10][10];
    uint32_t partition[16][4];
    VP9CoefCounts coef[4][2];
    uint32_t eob_branch[4][2][2][6][6];
    uint32_t switchable_interp[4][3];
    uint32_t inter_mode[7][4];  // NEARESTMV, NEARMV, ZEROMV, NEWMV
    uint32_t intra_inter[4][2];
    uint32_t comp_inter[5][2];
    uint32_t single_ref[5][2][2];
    uint32_t comp_ref[5][2];

    struct {
        uint32_t p32x32[2][4];
        uint32_t p16x16[2][3];
        uint32_t p8x8[2][2];
        uint32_t totals[4];
    } tx;

    uint32_t skip[3][2];

    struct {
        uint32_t joints[4];
        VP9MVComponentCounts comps[2];
    } mv;
} VP9Counts;

enum {
    VP9_TX_ONLY_4X4,
    VP9_TX_ALLOW_8X8,
    VP9_TX_ALLOW_16X16,
    VP9_TX_ALLOW_32X32,
    VP9_TX_MODE_SELECT
};

enum {
    VP9_FILTER_EIGHTTAP,
    VP9_FILTER_SMOOTH,
    VP9_FILTER_SHARP,
    VP9_FILTER_BILINEAR,
    VP9_FILTER_SWITCHABLE
};

// The defaults every context starts from (vp9_setup_past_independence).
void vp9_default_frame_context(VP9FrameContext *fc);
// The keyframe tables NVDEC also takes (kf_bmode_prob, kf_uv_mode_prob, partition_prob[0]).
extern const uint8_t vp9_kf_y_mode_prob[10][10][9];
extern const uint8_t vp9_kf_uv_mode_prob[10][9];
extern const uint8_t vp9_kf_partition_probs[16][3];

// Backward adaptation (vp9_adapt_coef_probs, vp9_adapt_mode_probs, vp9_adapt_mv_probs): fc from pre and counts.
void vp9_adapt_coef_probs(VP9FrameContext *fc, const VP9FrameContext *pre, const VP9Counts *counts, bool intraOnly,
                          bool lastWasKey);
void vp9_adapt_mode_probs(VP9FrameContext *fc, const VP9FrameContext *pre, const VP9Counts *counts, int interpFilter,
                          int txMode);
void vp9_adapt_mv_probs(VP9FrameContext *fc, const VP9FrameContext *pre, const VP9Counts *counts, bool allowHP);

#endif
