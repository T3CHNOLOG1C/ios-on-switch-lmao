// SPDX-License-Identifier: BSD-3-Clause
// VP9 frame syntax as far as a VLD engine's driver needs it: the superframe index, the uncompressed header (with the
// state it carries from frame to frame: entropy contexts, loop filter deltas, segmentation, reference sizes) and the
// compressed header's probability updates. NVDEC parses the tiles itself. Ported from libvpx v1.16.0
// (vp9/decoder/vp9_decodeframe.c read_uncompressed_header / read_compressed_header, vp9_dsubexp.c, vpx_dsp/bitreader;
// vp9_entropymode.c vp9_setup_past_independence); licence in VP9Probs.c.
#ifndef VP9_SYNTAX_H
#define VP9_SYNTAX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "VP9Probs.h"

enum {
    VP9_REFERENCE_SINGLE,
    VP9_REFERENCE_COMPOUND,
    VP9_REFERENCE_SELECT
};

// What persists between frames (libvpx VP9_COMMON's share of it).
typedef struct {
    VP9FrameContext contexts[4];
    bool contexts_valid;  // a key frame (or a resetting intra-only frame) has set them up
    int8_t ref_deltas[4], mode_deltas[2];

    struct {
        bool enabled, update_map, temporal_update, update_data, abs_delta;
        uint8_t tree_probs[7], pred_probs[3];
        bool feature_enabled[8][4];
        int16_t feature_data[8][4];
    } seg;

    uint16_t ref_width[8], ref_height[8];  // the frame each slot holds; 0: empty
} VP9State;

typedef struct {
    bool show_existing_frame;
    uint8_t frame_to_show;  // slot, when show_existing_frame
    bool key_frame, show_frame, error_resilient, intra_only;
    uint8_t profile, bit_depth, color_space;
    bool full_range, subsampled;  // subsampled: 4:2:0
    uint8_t reset_frame_context, refresh_frame_flags;
    uint8_t ref_idx[3];      // LAST, GOLDEN, ALTREF slots
    bool sign_bias[4];       // [INTRA, LAST, GOLDEN, ALTREF]
    uint16_t width, height;  // coded size (libvpx y_crop)
    uint16_t render_width, render_height;
    bool allow_high_precision_mv;
    uint8_t interp_filter;
    bool refresh_frame_context, frame_parallel;
    uint8_t frame_context_idx;
    uint8_t lf_level, lf_sharpness;
    bool lf_delta_enabled, lf_delta_update;
    uint8_t base_qindex;
    int8_t y_dc_delta_q, uv_dc_delta_q, uv_ac_delta_q;
    bool lossless;
    uint8_t log2_tile_cols, log2_tile_rows;
    size_t uncompressed_bytes, compressed_bytes;  // tile data starts after both
    // From the compressed header
    uint8_t tx_mode, reference_mode;
    bool compound_allowed;
    uint8_t comp_fixed_ref, comp_var_ref[2];  // libvpx frame ids (LAST 1, GOLDEN 2, ALTREF 3)
} VP9FrameHeader;

// Splits a sample into its frames (superframe index, Annex B of the VP9 spec): 1 when there is no index.
unsigned vp9_superframe_split(const uint8_t *data, size_t size, size_t offsets[8], size_t sizes[8]);

// The uncompressed header. Applies what it carries to `state` (vp9_setup_past_independence on intra-only and
// error-resilient frames, loop filter deltas, segmentation). 0, or -1 for a malformed or unsupported frame (profile > 0
// or not 4:2:0 8-bit, a reference slot with no frame, a frame size beyond 4096).
int vp9_parse_uncompressed(VP9State *state, const uint8_t *data, size_t size, VP9FrameHeader *header);

// The compressed header: fc must hold contexts[frame_context_idx] and gets the frame's delta updates; the header gets
// tx_mode, reference_mode and the compound references. 0, or -1 when the header is truncated or corrupt.
int vp9_parse_compressed(VP9FrameHeader *header, const uint8_t *data, size_t size, VP9FrameContext *fc);

#endif
