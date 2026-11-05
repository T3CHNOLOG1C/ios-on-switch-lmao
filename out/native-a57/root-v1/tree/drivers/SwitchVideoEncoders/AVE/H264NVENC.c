// SPDX-License-Identifier: BSD-2-Clause-Patent
// See H264NVENC.h. Syntax: ITU-T H.264 (08/2021) 7.3.2.1.1 seq_parameter_set_data, E.1.1 vui_parameters, 7.3.2.2
// pic_parameter_set_rbsp, 7.3.3 slice_header, 7.4.1 emulation prevention, Table A-1 level limits; ISO/IEC 14496-15
// 5.3.3.1 AVCDecoderConfigurationRecord. Setup: NVIDIA's nvenc_drv.h and clc5b7.h (MIT, drivers/TegraNVENC/nvidia).
#include "H264NVENC.h"
#include <string.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../../TegraNVENC/nvidia/nvenc_drv.h"
#pragma clang diagnostic pop
#include "../../TegraNVENC/nvidia/clc5b7.h"
#include "../../TegraNVENC/TegraNVENCCheck.h"

#define ALIGN(v, a) (((v) + (a) - 1) & ~((uint32_t)(a) - 1))

enum { LOG2_MAX_FRAME_NUM = 16, LOG2_MAX_POC_LSB = 16, SLICE_OFF = 512, ME_OFF = 640, MD_OFF = 832, Q_OFF = 960 };
enum { SETUP_BYTES = Q_OFF + 192 };

// ---- Bits --------------------------------------------------------------------------------------------------------

void h264bits_init(H264Bits *b, uint8_t *data, size_t cap)
{
    memset(b, 0, sizeof(*b));
    b->data = data;
    b->cap = cap;
}

void h264bits_u(H264Bits *b, uint32_t value, unsigned n)
{
    for (unsigned i = n; i-- > 0;) {
        b->acc = (b->acc << 1) | ((value >> i) & 1);
        if (++b->count == 8) {
            if (b->bytes < b->cap)
                b->data[b->bytes] = (uint8_t)b->acc;
            else
                b->overflow = true;
            b->bytes++;
            b->acc = 0;
            b->count = 0;
        }
    }
}

void h264bits_ue(H264Bits *b, uint32_t value)
{
    const uint64_t v = (uint64_t)value + 1;
    unsigned bits = 0;
    while ((v >> bits) > 1) bits++;
    h264bits_u(b, 0, bits);
    h264bits_u(b, (uint32_t)v, bits + 1);
}

void h264bits_se(H264Bits *b, int32_t value)
{
    h264bits_ue(b, value > 0 ? (uint32_t)(2 * value - 1) : (uint32_t)(-2 * (int64_t)value));
}

bool h264bits_aligned(const H264Bits *b)
{
    return b->count == 0;
}

void h264bits_align_zero(H264Bits *b)
{
    while (b->count) h264bits_u(b, 0, 1);
}

void h264bits_trailing(H264Bits *b)
{
    h264bits_u(b, 1, 1);
    h264bits_align_zero(b);
}

size_t h264bits_size(const H264Bits *b)
{
    return b->overflow ? 0 : b->bytes;
}

size_t h264enc_nal(uint8_t header, const uint8_t *rbsp, size_t size, uint8_t *out, size_t cap)
{
    size_t n = 0;
    unsigned zeros = 0;
    if (cap < 1) return 0;
    out[n++] = header;
    for (size_t i = 0; i < size; i++) {
        if (zeros == 2 && rbsp[i] <= 3) {
            if (n == cap) return 0;
            out[n++] = 3;
            zeros = 0;
        }
        if (n == cap) return 0;
        out[n++] = rbsp[i];
        zeros = rbsp[i] ? 0 : zeros + 1;
    }
    return n;
}

// ---- Configuration ------------------------------------------------------------------------------------------------

uint32_t h264enc_width_mbs(const H264EncConfig *c)
{
    return (c->width + 15) / 16;
}

uint32_t h264enc_height_mbs(const H264EncConfig *c)
{
    return (c->height + 15) / 16;
}

int h264enc_configure(H264EncConfig *c)
{
    if (c->width < TEGRA_NVENC_MIN_DIMENSION || c->height < TEGRA_NVENC_MIN_DIMENSION ||
        c->width > TEGRA_NVENC_MAX_DIMENSION || c->height > TEGRA_NVENC_MAX_DIMENSION || (c->width & 1) ||
        (c->height & 1))
        return -1;
    const uint32_t mbs = h264enc_width_mbs(c) * h264enc_height_mbs(c);
    if (mbs > TEGRA_NVENC_MAX_MBS) return -1;
    if (c->profile_idc != H264_PROFILE_BASELINE && c->profile_idc != H264_PROFILE_MAIN &&
        c->profile_idc != H264_PROFILE_HIGH)
        return -1;
    if (c->profile_idc == H264_PROFILE_BASELINE) c->cabac = false;
    if (c->profile_idc != H264_PROFILE_HIGH) c->transform8x8 = false;
    if (c->input_layout >= TEGRA_NVENC_LAYOUTS || c->chroma_mode > TEGRA_NVENC_CHROMA_METHOD) return -1;
    if (!c->level_idc) {
        static const struct {
            uint8_t level;
            uint32_t maxFs, maxMbps;
        } kLevels[] = {
            {20, 396, 11880},    {21, 792, 19800},    {22, 1620, 20250},    {30, 1620, 40500},
            {31, 3600, 108000},  {32, 5120, 216000},  {40, 8192, 245760},   {42, 8704, 522240},
            {50, 22080, 589824}, {51, 36864, 983040}, {52, 36864, 2073600},
        };
        const uint64_t fps = c->fps_num && c->fps_den ? ((uint64_t)c->fps_num + c->fps_den - 1) / c->fps_den : 60;
        const uint32_t wmbs = h264enc_width_mbs(c), hmbs = h264enc_height_mbs(c);
        c->level_idc = 52;
        for (unsigned i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); i++) {
            // A.3.1: frame size, macroblock rate, and neither side longer than sqrt(8 * MaxFS).
            const uint32_t side = (uint32_t)(8u * kLevels[i].maxFs);
            if (mbs <= kLevels[i].maxFs && (uint64_t)mbs * fps <= kLevels[i].maxMbps && wmbs * wmbs <= side &&
                hmbs * hmbs <= side) {
                c->level_idc = kLevels[i].level;
                break;
            }
        }
    }
    return 0;
}

void h264enc_layout(const H264EncConfig *c, H264EncLayout *l)
{
    memset(l, 0, sizeof(*l));
    const uint32_t wmbs = h264enc_width_mbs(c), hmbs = h264enc_height_mbs(c), mbs = wmbs * hmbs;
    const uint32_t pitch = ALIGN(wmbs * 16, 64), rows = tegra_nvenc_surface_rows(c->height);
    l->input.pitch = l->recon.pitch = pitch;
    l->input.rows = l->recon.rows = rows;
    l->input.chroma_offset = l->recon.chroma_offset = pitch * rows;
    l->input.bytes = l->recon.bytes = pitch * rows + pitch * rows / 2;
    // At most 3200 bits per macroblock (A.3.1 item j, the I_PCM bound) plus headers: NVENC's output never outgrows it.
    l->bitstream_bytes = ALIGN(mbs * 400 + 4096, 65536);
    l->status_bytes = 4096;
    l->history_bytes = ALIGN(tegra_nvenc_history_bytes(c->width), 256);
    l->coloc_bytes = ALIGN(mbs * (uint32_t)sizeof(nvenc_h264_coloc_mb_s), 256);
    l->history_offset = l->status_bytes;
    l->coloc_offset = l->history_offset + l->history_bytes;
    l->aux_bytes = l->coloc_offset + l->coloc_bytes;
}

// ---- Parameter sets -------------------------------------------------------------------------------------------------

static size_t finish(H264Bits *b, uint8_t header, uint8_t *out, size_t cap)
{
    h264bits_trailing(b);
    const size_t n = h264bits_size(b);
    return n ? h264enc_nal(header, b->data, n, out, cap) : 0;
}

size_t h264enc_write_sps(const H264EncConfig *c, uint8_t *out, size_t cap)
{
    uint8_t rbsp[128];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    const uint32_t wmbs = h264enc_width_mbs(c), hmbs = h264enc_height_mbs(c);
    h264bits_u(&b, c->profile_idc, 8);
    // constraint_set1 for Baseline: constrained baseline (no FMO/ASO/redundant slices), which this stream is.
    h264bits_u(&b, c->profile_idc == H264_PROFILE_BASELINE ? 0x40 : 0, 8);
    h264bits_u(&b, c->level_idc, 8);
    h264bits_ue(&b, 0);  // seq_parameter_set_id
    if (c->profile_idc == H264_PROFILE_HIGH) {
        h264bits_ue(&b, 1);    // chroma_format_idc 4:2:0
        h264bits_ue(&b, 0);    // bit_depth_luma_minus8
        h264bits_ue(&b, 0);    // bit_depth_chroma_minus8
        h264bits_u(&b, 0, 1);  // qpprime_y_zero_transform_bypass_flag
        h264bits_u(&b, 0, 1);  // seq_scaling_matrix_present_flag
    }
    h264bits_ue(&b, LOG2_MAX_FRAME_NUM - 4);
    h264bits_ue(&b, 0);  // pic_order_cnt_type
    h264bits_ue(&b, LOG2_MAX_POC_LSB - 4);
    h264bits_ue(&b, 1);    // max_num_ref_frames
    h264bits_u(&b, 0, 1);  // gaps_in_frame_num_value_allowed_flag
    h264bits_ue(&b, wmbs - 1);
    h264bits_ue(&b, hmbs - 1);
    h264bits_u(&b, 1, 1);  // frame_mbs_only_flag
    h264bits_u(&b, 1, 1);  // direct_8x8_inference_flag
    const uint32_t cropRight = (wmbs * 16 - c->width) / 2, cropBottom = (hmbs * 16 - c->height) / 2;
    h264bits_u(&b, cropRight || cropBottom, 1);
    if (cropRight || cropBottom) {
        h264bits_ue(&b, 0);
        h264bits_ue(&b, cropRight);
        h264bits_ue(&b, 0);
        h264bits_ue(&b, cropBottom);
    }
    h264bits_u(&b, 1, 1);  // vui_parameters_present_flag
    h264bits_u(&b, 0, 1);  // aspect_ratio_info_present_flag (square samples)
    h264bits_u(&b, 0, 1);  // overscan_info_present_flag
    h264bits_u(&b, 1, 1);  // video_signal_type_present_flag
    h264bits_u(&b, 5, 3);  // video_format: unspecified
    h264bits_u(&b, c->full_range, 1);
    h264bits_u(&b, c->colour_description, 1);
    if (c->colour_description) {
        h264bits_u(&b, c->colour_primaries, 8);
        h264bits_u(&b, c->transfer, 8);
        h264bits_u(&b, c->matrix, 8);
    }
    h264bits_u(&b, 0, 1);  // chroma_loc_info_present_flag
    const bool timing = c->fps_num && c->fps_den;
    h264bits_u(&b, timing, 1);
    if (timing) {
        h264bits_u(&b, c->fps_den, 32);      // num_units_in_tick
        h264bits_u(&b, 2 * c->fps_num, 32);  // time_scale (two ticks a frame)
        h264bits_u(&b, 0, 1);                // fixed_frame_rate_flag
    }
    h264bits_u(&b, 0, 1);               // nal_hrd_parameters_present_flag
    h264bits_u(&b, 0, 1);               // vcl_hrd_parameters_present_flag
    h264bits_u(&b, 0, 1);               // pic_struct_present_flag
    h264bits_u(&b, 1, 1);               // bitstream_restriction_flag: no reordering, so decoders output at once
    h264bits_u(&b, 1, 1);               // motion_vectors_over_pic_boundaries_flag
    h264bits_ue(&b, 0);                 // max_bytes_per_pic_denom
    h264bits_ue(&b, 0);                 // max_bits_per_mb_denom
    h264bits_ue(&b, 15);                // log2_max_mv_length_horizontal
    h264bits_ue(&b, 15);                // log2_max_mv_length_vertical
    h264bits_ue(&b, 0);                 // max_num_reorder_frames
    h264bits_ue(&b, 1);                 // max_dec_frame_buffering
    return finish(&b, 0x67, out, cap);  // nal_ref_idc 3, SPS
}

size_t h264enc_write_pps(const H264EncConfig *c, uint8_t *out, size_t cap)
{
    uint8_t rbsp[32];
    H264Bits b;
    h264bits_init(&b, rbsp, sizeof(rbsp));
    h264bits_ue(&b, 0);  // pic_parameter_set_id
    h264bits_ue(&b, 0);  // seq_parameter_set_id
    h264bits_u(&b, c->cabac, 1);
    h264bits_u(&b, 0, 1);  // bottom_field_pic_order_in_frame_present_flag
    h264bits_ue(&b, 0);    // num_slice_groups_minus1
    h264bits_ue(&b, 0);    // num_ref_idx_l0_default_active_minus1
    h264bits_ue(&b, 0);    // num_ref_idx_l1_default_active_minus1
    h264bits_u(&b, 0, 1);  // weighted_pred_flag
    h264bits_u(&b, 0, 2);  // weighted_bipred_idc
    h264bits_se(&b, 0);    // pic_init_qp_minus26
    h264bits_se(&b, 0);    // pic_init_qs_minus26
    h264bits_se(&b, 0);    // chroma_qp_index_offset
    h264bits_u(&b, 1, 1);  // deblocking_filter_control_present_flag
    h264bits_u(&b, 0, 1);  // constrained_intra_pred_flag
    h264bits_u(&b, 0, 1);  // redundant_pic_cnt_present_flag
    if (c->profile_idc == H264_PROFILE_HIGH) {
        h264bits_u(&b, c->transform8x8, 1);
        h264bits_u(&b, 0, 1);  // pic_scaling_matrix_present_flag
        h264bits_se(&b, 0);    // second_chroma_qp_index_offset
    }
    return finish(&b, 0x68, out, cap);  // nal_ref_idc 3, PPS
}

size_t h264enc_write_avcc(const H264EncConfig *c, uint8_t *out, size_t cap)
{
    uint8_t sps[160], pps[48];
    const size_t spsSize = h264enc_write_sps(c, sps, sizeof(sps)), ppsSize = h264enc_write_pps(c, pps, sizeof(pps));
    const size_t n = 6 + 2 + spsSize + 1 + 2 + ppsSize + (c->profile_idc == H264_PROFILE_HIGH ? 4 : 0);
    if (!spsSize || !ppsSize || n > cap) return 0;
    size_t i = 0;
    out[i++] = 1;         // configurationVersion
    out[i++] = sps[1];    // AVCProfileIndication
    out[i++] = sps[2];    // profile_compatibility
    out[i++] = sps[3];    // AVCLevelIndication
    out[i++] = 0xfc | 3;  // lengthSizeMinusOne = 3
    out[i++] = 0xe0 | 1;  // one SPS
    out[i++] = (uint8_t)(spsSize >> 8);
    out[i++] = (uint8_t)spsSize;
    memcpy(out + i, sps, spsSize);
    i += spsSize;
    out[i++] = 1;  // one PPS
    out[i++] = (uint8_t)(ppsSize >> 8);
    out[i++] = (uint8_t)ppsSize;
    memcpy(out + i, pps, ppsSize);
    i += ppsSize;
    if (c->profile_idc == H264_PROFILE_HIGH) {  // 5.3.3.1.2: chroma format and bit depths, no SPS extensions
        out[i++] = 0xfc | 1;
        out[i++] = 0xf8 | 0;
        out[i++] = 0xf8 | 0;
        out[i++] = 0;
    }
    return i;
}

void h264enc_slice_header(const H264EncConfig *c, const H264EncPicture *p, H264Bits *b)
{
    const bool idr = p->type == H264ENC_IDR, intra = p->type != H264ENC_P;
    h264bits_ue(b, 0);              // first_mb_in_slice
    h264bits_ue(b, intra ? 7 : 5);  // slice_type: every slice of the picture I (7) or P (5)
    h264bits_ue(b, 0);              // pic_parameter_set_id
    h264bits_u(b, p->frame_num & 0xffff, LOG2_MAX_FRAME_NUM);
    if (idr) h264bits_ue(b, p->idr_pic_id);
    h264bits_u(b, p->poc_lsb & 0xffff, LOG2_MAX_POC_LSB);
    if (!intra) {
        h264bits_u(b, 0, 1);  // num_ref_idx_active_override_flag
        h264bits_u(b, 0, 1);  // ref_pic_list_modification_flag_l0
    }
    if (idr) {
        h264bits_u(b, 0, 1);  // no_output_of_prior_pics_flag
        h264bits_u(b, 0, 1);  // long_term_reference_flag
    } else {
        h264bits_u(b, 0, 1);  // adaptive_ref_pic_marking_mode_flag: sliding window
    }
    if (c->cabac && !intra) h264bits_ue(b, 0);  // cabac_init_idc
    h264bits_se(b, (int32_t)p->qp - 26);        // slice_qp_delta
    h264bits_ue(b, 0);                          // disable_deblocking_filter_idc: on
    h264bits_se(b, 0);                          // slice_alpha_c0_offset_div2
    h264bits_se(b, 0);                          // slice_beta_offset_div2
}

// ---- The picture setup ----------------------------------------------------------------------------------------------

static void surfaceConfig(const H264EncConfig *c, const H264EncSurface *s, nvenc_h264_surface_cfg_s *cfg, bool input)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->frame_width_minus1 = (unsigned short)(c->width - 1);
    cfg->frame_height_minus1 = (unsigned short)(c->height - 1);
    cfg->sfc_pitch = cfg->sfc_pitch_chroma = (unsigned short)s->pitch;
    cfg->chroma_top_frm_offset = c->chroma_mode == TEGRA_NVENC_CHROMA_OFFSET ? s->chroma_offset / 256 : 0;
    if (!input) {
        cfg->tiled_16x16 = 1;  // references are always 16x16 tiled (nvenc_drv.h 1.5)
        return;
    }
    switch (c->input_layout) {
    case TEGRA_NVENC_LAYOUT_BL2_LOG: cfg->block_height = 1; break;
    case TEGRA_NVENC_LAYOUT_BL2_RAW:
    case TEGRA_NVENC_LAYOUT_BL4_LOG: cfg->block_height = 2; break;
    case TEGRA_NVENC_LAYOUT_TILED16: cfg->tiled_16x16 = 1; break;
    default: break;  // PITCH and BL1: block_height 0
    }
}

// Motion search: spatial and zero-vector hints, both searched and refined to quarter pel over one full 8x8 stamp;
// partitions down to 8x8, P skip; intra 4x4, 8x8 (with the 8x8 transform) and 16x16 with every mode. Biases,
// deadzones and thresholds at 0: NVENC's own defaults. Nothing that writes outside the picture, bitstream and status.
static void controls(const H264EncConfig *c, const H264EncPicture *p, uint8_t *setup)
{
    const uint32_t mbs = h264enc_width_mbs(c) * h264enc_height_mbs(c);
    nvenc_h264_slice_control_s *slice = (nvenc_h264_slice_control_s *)(setup + SLICE_OFF);
    slice->num_mb = mbs;
    slice->qp_avr = p->qp;
    slice->qp_slice_min = 0;
    slice->qp_slice_max = 51;
    slice->disable_deblocking_filter_idc = 0;
    slice->cabac_init_idc = 0;
    slice->force_intra = p->type != H264ENC_P;

    nvenc_h264_me_control_s *me = (nvenc_h264_me_control_s *)(setup + ME_OFF);
    me->refinement_mode = 1;  // quarter pel
    me->refine_on_search_enable = 1;
    me->fps_mvcost = 1;
    me->sps_mvcost = 1;
    me->predsrc.self_spatial_enable = 1;
    me->predsrc.self_spatial_search = 1;
    me->predsrc.self_spatial_refine = 1;
    me->predsrc.const_mv_enable = 1;
    me->predsrc.const_mv_search = 1;
    me->predsrc.const_mv_refine = 1;
    me->shape0.bitmask[0] = me->shape0.bitmask[1] = 0xffffffffu;
    me->hint_type0 = 0;
    me->hint_type1 = 1;
    me->hint_type2 = 2;
    me->hint_type3 = 3;
    me->hint_type4 = 4;

    nvenc_h264_md_control_s *md = (nvenc_h264_md_control_s *)(setup + MD_OFF);
    md->intra_luma4x4_mode_enable = 0x1ff;
    md->intra_luma8x8_mode_enable = c->transform8x8 ? 0x1ff : 0;
    md->intra_luma16x16_mode_enable = 0xf;
    md->intra_chroma_mode_enable = 0xf;
    md->l0_part_16x16_enable = 1;
    md->l0_part_16x8_enable = 1;
    md->l0_part_8x16_enable = 1;
    md->l0_part_8x8_enable = 1;
    md->pskip_enable = 1;
    md->mv_cost_enable = 1;
    md->ip_search_mode = c->transform8x8 ? 7 : 5;  // 4x4, (8x8,) 16x16
    (void)setup;
}

void h264enc_fill_submit(const H264EncConfig *c, const H264EncLayout *l, const H264EncPicture *p,
                         const H264EncHandles *h, TegraNVENCSubmit *s)
{
    memset(s, 0, sizeof(*s));
    s->codec = TEGRA_NVENC_CODEC_H264;
    s->setup_bytes = SETUP_BYTES;
    nvenc_h264_drv_pic_setup_s *setup = (nvenc_h264_drv_pic_setup_s *)s->setup;
    const bool pPicture = p->type == H264ENC_P;
    setup->magic = 0xd0b70006u;  // NV_NVENC_5_0
    surfaceConfig(c, &l->recon, &setup->refpic_cfg, false);
    surfaceConfig(c, &l->input, &setup->input_cfg, true);
    surfaceConfig(c, &l->recon, &setup->outputpic_cfg, false);
    nvenc_h264_sps_data_s *sps = &setup->sps_data;
    sps->profile_idc = c->profile_idc;
    sps->level_idc = c->level_idc;
    sps->chroma_format_idc = 1;
    sps->pic_order_cnt_type = 0;
    sps->log2_max_frame_num_minus4 = LOG2_MAX_FRAME_NUM - 4;
    sps->log2_max_pic_order_cnt_lsb_minus4 = LOG2_MAX_POC_LSB - 4;
    sps->frame_mbs_only = 1;
    nvenc_h264_pps_data_s *pps = &setup->pps_data;
    pps->entropy_coding_mode_flag = c->cabac;
    pps->deblocking_filter_control_present_flag = 1;
    pps->transform_8x8_mode_flag = c->transform8x8;
    nvenc_h264_rc_s *rc = &setup->rate_control;  // constant QP: the plug-in's controller sets it per picture
    rc->QP[0] = rc->QP[1] = rc->QP[2] = p->qp;
    rc->maxQP[0] = rc->maxQP[1] = rc->maxQP[2] = 51;
    rc->framerate = c->fps_num && c->fps_den ? (int)(c->fps_num / c->fps_den) : 30;
    rc->gop_length = 0xffffffffu;
    nvenc_h264_pic_control_s *pc = &setup->pic_control;
    memset(pc->l0, -1, sizeof(pc->l0));
    memset(pc->l1, -1, sizeof(pc->l1));
    if (pPicture) {
        pc->l0[0] = 0;  // the previous picture, in reference slot 0
        pc->temp_dist_l0[0] = 1;
    }
    pc->frame_num = (unsigned short)p->frame_num;
    pc->pic_order_cnt_lsb = (unsigned short)p->poc_lsb;
    pc->idr_pic_id = (unsigned short)p->idr_pic_id;
    pc->slice_control_offset = SLICE_OFF;
    pc->me_control_offset = ME_OFF;
    pc->md_control_offset = MD_OFF;
    pc->q_control_offset = Q_OFF;
    pc->hist_buf_size = l->history_bytes;
    pc->bitstream_buf_size = l->bitstream_bytes;
    pc->bitstream_start_pos = 0;
    pc->max_byte_count_before_resid_zero = l->bitstream_bytes - 4096;
    pc->pic_struct = 0;
    pc->pic_type = p->type;
    pc->ref_pic_flag = 1;
    pc->slice_mode = 1;  // static: one slice, from the slice control array
    pc->e4byteStartCode = 1;
    setup->gpTimer_timeout_val = TEGRA_NVENC_GPTIMER_MAX;
    controls(c, p, s->setup);

    const bool method = c->chroma_mode == TEGRA_NVENC_CHROMA_METHOD;
    const uint32_t inChroma = method ? l->input.chroma_offset : 0, refChroma = method ? l->recon.chroma_offset : 0;
    TegraNVENCEntry *e = s->entries;
    uint32_t n = 0;
#define VALUE(m, v)              e[n++] = (TegraNVENCEntry){(uint16_t)(m), TEGRA_NVENC_ENTRY_VALUE, (v), 0}
#define RELOC(m, handle, offset) e[n++] = (TegraNVENCEntry){(uint16_t)(m), TEGRA_NVENC_ENTRY_RELOC, (handle), (offset)}
    VALUE(NVC5B7_SET_APPLICATION_ID, NVC5B7_SET_APPLICATION_ID_ID_NVENC_H264);
    VALUE(NVC5B7_SET_CONTROL_PARAMS, NVC5B7_SET_CONTROL_PARAMS_CODEC_TYPE_H264 | (1u << 12) /* GPTIMER_ON */);
    VALUE(NVC5B7_SET_PICTURE_INDEX, p->picture_index);
    RELOC(NVC5B7_SET_IN_CUR_PIC, h->input, 0);
    RELOC(NVC5B7_SET_IN_CUR_PIC_CHROMA_U, h->input, inChroma);
    RELOC(NVC5B7_SET_OUT_REF_PIC_LUMA, h->recon, 0);
    RELOC(NVC5B7_SET_OUT_REF_PIC_CHROMA, h->recon, refChroma);
    if (pPicture) {
        RELOC(NVC5B7_SET_IN_REF_PIC0_LUMA, h->ref, 0);
        RELOC(NVC5B7_SET_IN_REF_PIC0_CHROMA, h->ref, refChroma);
    }
    RELOC(NVC5B7_SET_OUT_BITSTREAM, h->bitstream, 0);
    RELOC(NVC5B7_SET_OUT_ENC_STATUS, h->aux, 0);
    RELOC(NVC5B7_SET_IOHISTORY, h->aux, l->history_offset);
    RELOC(NVC5B7_SET_OUT_COLOC_DATA, h->aux, l->coloc_offset);
    VALUE(NVC5B7_EXECUTE, 1u << 8 /* AWAKEN */);
#undef VALUE
#undef RELOC
    s->entry_count = n;
    s->clean[0] = (TegraNVENCRange){h->input, 0, l->input.bytes, 0};
    s->clean_count = 1;
}

// ---- Input pictures -------------------------------------------------------------------------------------------------

// A 64-byte x 8-row GOB of 16-byte x 2-row sectors (the layout NVDEC writes and BlockLinear.c reads).
static size_t gob(uint32_t x, uint32_t y)
{
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
}

static size_t blockLinear(uint32_t gobs, uint32_t pitch, uint32_t x, uint32_t y)
{
    const uint32_t blockRows = 8 * gobs;
    return (size_t)(y / blockRows) * pitch * blockRows + (size_t)(x / 64) * 512 * gobs + ((y % blockRows) / 8) * 512 +
           gob(x, y);
}

size_t h264enc_surface_offset(uint32_t layout, uint32_t pitch, uint32_t x, uint32_t y)
{
    switch (layout) {
    case TEGRA_NVENC_LAYOUT_BL2_LOG:
    case TEGRA_NVENC_LAYOUT_BL2_RAW: return blockLinear(2, pitch, x, y);
    case TEGRA_NVENC_LAYOUT_BL4_LOG: return blockLinear(4, pitch, x, y);
    case TEGRA_NVENC_LAYOUT_BL1: return blockLinear(1, pitch, x, y);
    case TEGRA_NVENC_LAYOUT_TILED16: return (size_t)(y / 16) * pitch * 16 + (x / 16) * 256 + (y % 16) * 16 + x % 16;
    default: return (size_t)y * pitch + x;
    }
}

// One plane: `rows` x `bytes` samples from `src` (edge-replicated past `w` x `h`), 16 bytes at a time: each 16-byte
// run of a row is contiguous in every layout.
static void putPlane(uint32_t layout, uint32_t pitch, uint8_t *dst, const uint8_t *src, size_t stride, uint32_t w,
                     uint32_t h, uint32_t codedW, uint32_t codedH, uint32_t pairs)
{
    for (uint32_t y = 0; y < codedH; y++) {
        const uint8_t *row = src + (size_t)(y < h ? y : h - 1) * stride;
        if (layout == TEGRA_NVENC_LAYOUT_PITCH) {
            memcpy(dst + (size_t)y * pitch, row, w);
            for (uint32_t x = w; x < codedW; x++) dst[(size_t)y * pitch + x] = row[pairs ? w - 2 + (x & 1) : w - 1];
            continue;
        }
        for (uint32_t x = 0; x < codedW; x += 16) {
            uint8_t *out = dst + h264enc_surface_offset(layout, pitch, x, y);
            if (x + 16 <= w) {
                memcpy(out, row + x, 16);
                continue;
            }
            for (uint32_t i = 0; i < 16; i++) {
                const uint32_t sx = x + i;
                out[i] = row[sx < w ? sx : (pairs ? w - 2 + (sx & 1) : w - 1)];
            }
        }
    }
}

void h264enc_put_input(const H264EncConfig *c, const H264EncLayout *l, uint8_t *buffer, const uint8_t *luma,
                       size_t lumaStride, const uint8_t *chroma, size_t chromaStride)
{
    const uint32_t codedW = h264enc_width_mbs(c) * 16, codedH = h264enc_height_mbs(c) * 16;
    putPlane(c->input_layout, l->input.pitch, buffer, luma, lumaStride, c->width, c->height, codedW, codedH, 0);
    putPlane(c->input_layout, l->input.pitch, buffer + l->input.chroma_offset, chroma, chromaStride, c->width,
             c->height / 2, codedW, codedH / 2, 1);
}

// ---- Output -----------------------------------------------------------------------------------------------------

size_t h264enc_annexb_to_avcc(const uint8_t *in, size_t size, uint8_t *out, size_t cap)
{
    size_t n = 0, i = 0;
    while (i + 3 <= size) {
        if (!(in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 1)) {
            i++;
            continue;
        }
        const size_t start = i + 3;
        size_t end = start;
        while (end + 3 <= size && !(in[end] == 0 && in[end + 1] == 0 && (in[end + 2] == 1 || in[end + 2] == 0))) end++;
        if (end + 3 > size) end = size;
        while (end > start && in[end - 1] == 0) end--;  // trailing_zero_8bits and the next start code's zero byte
        const size_t length = end - start;
        if (length) {
            if (n + 4 + length > cap) return 0;
            out[n++] = (uint8_t)(length >> 24);
            out[n++] = (uint8_t)(length >> 16);
            out[n++] = (uint8_t)(length >> 8);
            out[n++] = (uint8_t)length;
            memcpy(out + n, in + start, length);
            n += length;
        }
        i = end;
    }
    return n;
}
