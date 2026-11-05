// SPDX-License-Identifier: BSD-3-Clause
// See VP9Probs.h. Ported from libvpx v1.16.0: vp9/common/vp9_entropy.c (vp9_adapt_coef_probs), vp9_entropymode.c
// (init_mode_probs, vp9_adapt_mode_probs, tx_counts_to_branch_counts_*), vp9_entropymv.c (default_nmv_context,
// vp9_adapt_mv_probs) and vpx_dsp/prob.[ch] (merge_probs, mode_mv_merge_probs, vpx_tree_merge_probs).
//
// Copyright (c) 2010, Google Inc. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
// following conditions are met:
//   * Redistributions of source code must retain the above copyright notice, this list of conditions and the
//     following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
//     following disclaimer in the documentation and/or other materials provided with the distribution.
//   * Neither the name of Google nor the names of its contributors may be used to endorse or promote products derived
//     from this software without specific prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// (libvpx also grants patent rights in its PATENTS file.)
#include "VP9Probs.h"
#include <string.h>
#include "VP9Tables.h"

typedef int8_t TreeIndex;

// Mode numbering as libvpx and NVDEC use it: DC V H D45 D135 D117 D153 D207 D63 TM.
static const TreeIndex kIntraModeTree[18] = {-0, 2, -9, 4, -1, 6, 8, 12, -2, 10, -4, -5, -3, 14, -8, 16, -6, -7};
static const TreeIndex kInterModeTree[6] = {-2, 2, -0, 4, -1, -3};  // ZERO, NEAREST, NEAR, NEW (INTER_OFFSET)
static const TreeIndex kPartitionTree[6] = {-0, 2, -1, 4, -2, -3};
static const TreeIndex kSwitchableInterpTree[4] = {-VP9_FILTER_EIGHTTAP, 2, -VP9_FILTER_SMOOTH, -VP9_FILTER_SHARP};
static const TreeIndex kMVJointTree[6] = {-0, 2, -1, 4, -2, -3};
static const TreeIndex kMVClassTree[20] = {-0, 2, -1, 4, 6, 8, -2, -3, 10, 12, -4, -5, -6, 14, 16, 18, -7, -8, -9, -10};
static const TreeIndex kMVClass0Tree[2] = {-0, -1};
static const TreeIndex kMVFPTree[6] = {-0, 2, -1, 4, -2, -3};

static const VP9MVComponent kDefaultMV[2] = {
    {128,
     {224, 144, 192, 168, 192, 176, 192, 198, 198, 245},
     {216},
     {136, 140, 148, 160, 176, 192, 224, 234, 234, 240},
     {{128, 128, 64}, {96, 112, 64}},
     {64, 96, 64},
     160,
     128},
    {128,
     {216, 128, 176, 160, 176, 176, 192, 198, 198, 208},
     {208},
     {136, 140, 148, 160, 176, 192, 224, 234, 234, 240},
     {{128, 128, 64}, {96, 112, 64}},
     {64, 96, 64},
     160,
     128},
};

void vp9_default_frame_context(VP9FrameContext *fc)
{
    static const uint8_t kIntraInter[4] = {9, 102, 187, 225}, kCompInter[5] = {239, 183, 119, 96, 41};
    static const uint8_t kCompRef[5] = {50, 126, 123, 221, 226};
    static const uint8_t kSingleRef[5][2] = {{33, 16}, {77, 74}, {142, 142}, {172, 170}, {238, 247}};
    static const uint8_t kSkip[3] = {192, 128, 64};
    static const uint8_t kTx32[2][3] = {{3, 136, 37}, {5, 52, 13}}, kTx16[2][2] = {{20, 152}, {15, 101}};
    static const uint8_t kTx8[2][1] = {{100}, {66}}, kJoints[3] = {32, 64, 96};
    memset(fc, 0, sizeof(*fc));
    memcpy(fc->coef_probs[0], kDefault_coef_probs_4x4, sizeof(fc->coef_probs[0]));
    memcpy(fc->coef_probs[1], kDefault_coef_probs_8x8, sizeof(fc->coef_probs[1]));
    memcpy(fc->coef_probs[2], kDefault_coef_probs_16x16, sizeof(fc->coef_probs[2]));
    memcpy(fc->coef_probs[3], kDefault_coef_probs_32x32, sizeof(fc->coef_probs[3]));
    memcpy(fc->uv_mode_prob, kDefault_if_uv_probs, sizeof(fc->uv_mode_prob));
    memcpy(fc->y_mode_prob, kDefault_if_y_probs, sizeof(fc->y_mode_prob));
    memcpy(fc->switchable_interp_prob, kDefault_switchable_interp_prob, sizeof(fc->switchable_interp_prob));
    memcpy(fc->partition_prob, kDefault_partition_probs, sizeof(fc->partition_prob));
    memcpy(fc->intra_inter_prob, kIntraInter, sizeof(kIntraInter));
    memcpy(fc->comp_inter_prob, kCompInter, sizeof(kCompInter));
    memcpy(fc->comp_ref_prob, kCompRef, sizeof(kCompRef));
    memcpy(fc->single_ref_prob, kSingleRef, sizeof(kSingleRef));
    memcpy(fc->tx.p32x32, kTx32, sizeof(kTx32));
    memcpy(fc->tx.p16x16, kTx16, sizeof(kTx16));
    memcpy(fc->tx.p8x8, kTx8, sizeof(kTx8));
    memcpy(fc->skip_probs, kSkip, sizeof(kSkip));
    memcpy(fc->inter_mode_probs, kDefault_inter_mode_probs, sizeof(fc->inter_mode_probs));
    memcpy(fc->nmvc.joints, kJoints, sizeof(kJoints));
    memcpy(fc->nmvc.comps, kDefaultMV, sizeof(kDefaultMV));
}

// ---- vpx_dsp/prob.h ------------------------------------------------------------------------------------------------

static uint8_t getProb(uint32_t num, uint32_t den)
{
    const int p = (int)(((uint64_t)num * 256 + (den >> 1)) / den);
    return (uint8_t)(p > 255 ? 255 : p < 1 ? 1 : p);
}

static uint8_t weightedProb(int prob1, int prob2, int factor)
{
    return (uint8_t)((prob1 * (256 - factor) + prob2 * factor + 128) >> 8);  // ROUND_POWER_OF_TWO(.., 8)
}

static uint8_t mergeProbs(uint8_t pre, const uint32_t ct[2], uint32_t countSat, uint32_t maxUpdateFactor)
{
    const uint32_t den = ct[0] + ct[1];
    const uint8_t prob = den ? getProb(ct[0], den) : 128;
    const uint32_t count = den < countSat ? den : countSat;
    return weightedProb(pre, prob, (int)(maxUpdateFactor * count / countSat));
}

static uint8_t modeMVMergeProbs(uint8_t pre, const uint32_t ct[2])
{
    static const int kCountToUpdateFactor[21] = {0,  6,  12, 19, 25, 32,  38,  44,  51,  57, 64,
                                                 70, 76, 83, 89, 96, 102, 108, 115, 121, 128};
    const uint32_t den = ct[0] + ct[1];
    if (!den) return pre;
    return weightedProb(pre, getProb(ct[0], den), kCountToUpdateFactor[den < 20 ? den : 20]);
}

static uint32_t treeMerge(unsigned i, const TreeIndex *tree, const uint8_t *pre, const uint32_t *counts, uint8_t *probs)
{
    const int l = tree[i], r = tree[i + 1];
    const uint32_t left = l <= 0 ? counts[-l] : treeMerge((unsigned)l, tree, pre, counts, probs);
    const uint32_t right = r <= 0 ? counts[-r] : treeMerge((unsigned)r, tree, pre, counts, probs);
    const uint32_t ct[2] = {left, right};
    probs[i >> 1] = modeMVMergeProbs(pre[i >> 1], ct);
    return left + right;
}

static void treeMergeProbs(const TreeIndex *tree, const uint8_t *pre, const uint32_t *counts, uint8_t *probs)
{
    treeMerge(0, tree, pre, counts, probs);
}

// ---- Adaptation ----------------------------------------------------------------------------------------------------

void vp9_adapt_coef_probs(VP9FrameContext *fc, const VP9FrameContext *pre, const VP9Counts *counts, bool intraOnly,
                          bool lastWasKey)
{
    // COEF_MAX_UPDATE_FACTOR(_KEY) 112, _AFTER_KEY 128; every COEF_COUNT_SAT is 24.
    const uint32_t countSat = 24, factor = !intraOnly && lastWasKey ? 128 : 112;
    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 2; i++)
            for (int j = 0; j < 2; j++)
                for (int k = 0; k < 6; k++)
                    for (int l = 0; l < (k ? 6 : 3); l++) {
                        const uint32_t *n = counts->coef[t][i][j][k][l];
                        const uint32_t eob = counts->eob_branch[t][i][j][k][l];
                        const uint32_t branch[3][2] = {{n[3], eob - n[3]}, {n[0], n[1] + n[2]}, {n[1], n[2]}};
                        for (int m = 0; m < 3; m++)
                            fc->coef_probs[t][i][j][k][l][m] =
                                mergeProbs(pre->coef_probs[t][i][j][k][l][m], branch[m], countSat, factor);
                    }
}

void vp9_adapt_mode_probs(VP9FrameContext *fc, const VP9FrameContext *pre, const VP9Counts *counts, int interpFilter,
                          int txMode)
{
    for (int i = 0; i < 4; i++)
        fc->intra_inter_prob[i] = modeMVMergeProbs(pre->intra_inter_prob[i], counts->intra_inter[i]);
    for (int i = 0; i < 5; i++) {
        fc->comp_inter_prob[i] = modeMVMergeProbs(pre->comp_inter_prob[i], counts->comp_inter[i]);
        fc->comp_ref_prob[i] = modeMVMergeProbs(pre->comp_ref_prob[i], counts->comp_ref[i]);
        for (int j = 0; j < 2; j++)
            fc->single_ref_prob[i][j] = modeMVMergeProbs(pre->single_ref_prob[i][j], counts->single_ref[i][j]);
    }
    for (int i = 0; i < 7; i++)
        treeMergeProbs(kInterModeTree, pre->inter_mode_probs[i], counts->inter_mode[i], fc->inter_mode_probs[i]);
    for (int i = 0; i < 4; i++)
        treeMergeProbs(kIntraModeTree, pre->y_mode_prob[i], counts->y_mode[i], fc->y_mode_prob[i]);
    for (int i = 0; i < 10; i++)
        treeMergeProbs(kIntraModeTree, pre->uv_mode_prob[i], counts->uv_mode[i], fc->uv_mode_prob[i]);
    for (int i = 0; i < 16; i++)
        treeMergeProbs(kPartitionTree, pre->partition_prob[i], counts->partition[i], fc->partition_prob[i]);
    if (interpFilter == VP9_FILTER_SWITCHABLE)
        for (int i = 0; i < 4; i++)
            treeMergeProbs(kSwitchableInterpTree, pre->switchable_interp_prob[i], counts->switchable_interp[i],
                           fc->switchable_interp_prob[i]);
    if (txMode == VP9_TX_MODE_SELECT) {
        for (int i = 0; i < 2; i++) {
            const uint32_t *c8 = counts->tx.p8x8[i], *c16 = counts->tx.p16x16[i], *c32 = counts->tx.p32x32[i];
            const uint32_t b8[1][2] = {{c8[0], c8[1]}};
            const uint32_t b16[2][2] = {{c16[0], c16[1] + c16[2]}, {c16[1], c16[2]}};
            const uint32_t b32[3][2] = {
                {c32[0], c32[1] + c32[2] + c32[3]}, {c32[1], c32[2] + c32[3]}, {c32[2], c32[3]}};
            fc->tx.p8x8[i][0] = modeMVMergeProbs(pre->tx.p8x8[i][0], b8[0]);
            for (int j = 0; j < 2; j++) fc->tx.p16x16[i][j] = modeMVMergeProbs(pre->tx.p16x16[i][j], b16[j]);
            for (int j = 0; j < 3; j++) fc->tx.p32x32[i][j] = modeMVMergeProbs(pre->tx.p32x32[i][j], b32[j]);
        }
    }
    for (int i = 0; i < 3; i++) fc->skip_probs[i] = modeMVMergeProbs(pre->skip_probs[i], counts->skip[i]);
}

void vp9_adapt_mv_probs(VP9FrameContext *fc, const VP9FrameContext *pre, const VP9Counts *counts, bool allowHP)
{
    treeMergeProbs(kMVJointTree, pre->nmvc.joints, counts->mv.joints, fc->nmvc.joints);
    for (int i = 0; i < 2; i++) {
        VP9MVComponent *comp = &fc->nmvc.comps[i];
        const VP9MVComponent *p = &pre->nmvc.comps[i];
        const VP9MVComponentCounts *c = &counts->mv.comps[i];
        comp->sign = modeMVMergeProbs(p->sign, c->sign);
        treeMergeProbs(kMVClassTree, p->classes, c->classes, comp->classes);
        treeMergeProbs(kMVClass0Tree, p->class0, c->class0, comp->class0);
        for (int j = 0; j < 10; j++) comp->bits[j] = modeMVMergeProbs(p->bits[j], c->bits[j]);
        for (int j = 0; j < 2; j++) treeMergeProbs(kMVFPTree, p->class0_fp[j], c->class0_fp[j], comp->class0_fp[j]);
        treeMergeProbs(kMVFPTree, p->fp, c->fp, comp->fp);
        if (allowHP) {
            comp->class0_hp = modeMVMergeProbs(p->class0_hp, c->class0_hp);
            comp->hp = modeMVMergeProbs(p->hp, c->hp);
        }
    }
}
