// SPDX-License-Identifier: BSD-2-Clause-Patent
// H.264 on NVDEC 2.0; see H264NVDEC.h. Clause numbers are ITU-T H.264 (08/2021).
#include "H264NVDEC.h"
#include <stdlib.h>
#include <string.h>
#include "../../TegraNVDEC/nvidia/nvdec_drv.h"
#include "../../TegraNVDEC/nvidia/clc5b0.h"

#define MAX_SURFACES 17
#define MAX_PENDING 34
#define ALIGN(v, a) (((v) + (a) - 1) / (a) * (a))

enum { REF_NONE, REF_SHORT, REF_LONG };

typedef struct {
    uint8_t ref;               // REF_NONE: slot free
    bool non_existing;         // 8.2.5.2 frame_num gap filler
    uint8_t pic_idx;           // surface, picture slot and colocated slot
    uint8_t slot;              // entry in nvdec_h264_pic_s.dpb[]
    uint32_t frame_num, long_term_frame_idx;
    int32_t top_poc, bottom_poc;
} RefFrame;

typedef struct {
    int32_t poc;
    void *item;
} Pending;

struct H264Decoder {
    H264Backend b;
    H264ParamSets sets;
    H264SPS sps;                         // the active SPS
    bool configured;
    // Buffers for the configured geometry
    uint32_t width_mbs, height_mbs, refs_allocated;
    uint32_t pitch, aligned_height, luma_bytes, surface_bytes, surfaces;
    uint32_t surface[MAX_SURFACES];
    uint8_t *surface_va[MAX_SURFACES];
    uint32_t aux, mbhist_off, mbhist_bytes, history_off, history_bytes;
    uint32_t input, slices_off, bits_off, max_slices;
    uint8_t *input_va;
    uint64_t input_bytes;
    // Reference frames (8.2.5) and their NVDEC DPB slots
    RefFrame refs[16];
    int32_t max_long_term_frame_idx;     // -1: "no long-term frame indices"
    uint8_t last_pic_idx;
    // Picture order count state (8.2.1)
    int32_t prev_poc_msb, prev_poc_lsb;
    uint32_t prev_frame_num_offset, prev_frame_num, prev_ref_frame_num;
    // Display order
    Pending pending[MAX_PENDING];
    uint32_t pending_count, reorder;
    // The access unit being assembled
    uint32_t bits_len, slices;
    uint32_t picture_index;
};

H264Decoder *h264_decoder_create(const H264Backend *backend)
{
    H264Decoder *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->b = *backend;
    d->max_long_term_frame_idx = -1;
    return d;
}

static void releaseBuffers(H264Decoder *d)
{
    for (uint32_t i = 0; i < d->surfaces; i++) d->b.free(d->b.ctx, d->surface[i]);
    if (d->aux) d->b.free(d->b.ctx, d->aux);
    if (d->input) d->b.free(d->b.ctx, d->input);
    d->surfaces = d->aux = d->input = 0;
    d->input_va = NULL;
    d->configured = false;
}

void h264_decoder_destroy(H264Decoder *d)
{
    if (!d) return;
    for (uint32_t i = 0; i < d->pending_count; i++) d->b.discard(d->b.ctx, d->pending[i].item);
    releaseBuffers(d);
    free(d);
}

int h264_decoder_parameter_set(H264Decoder *d, const uint8_t *nal, size_t size)
{
    if (!size) return -1;
    switch (nal[0] & 0x1f) {
    case H264_NAL_SPS: return h264_parse_sps(&d->sets, nal, size, NULL);
    case H264_NAL_PPS: return h264_parse_pps(&d->sets, nal, size, NULL);
    default: return 0;
    }
}

const H264SPS *h264_decoder_sps(const H264Decoder *d) { return d->configured ? &d->sps : NULL; }

// Table A-1 MaxDpbMbs.
static uint32_t maxDpbFrames(const H264SPS *s)
{
    uint32_t mbs;
    switch (s->level_idc) {
    case 9: case 10: mbs = 396; break;
    case 11: mbs = (s->constraint_flags & 0x10) && s->profile_idc != 100 ? 396 : 900; break;   // level 1b
    case 12: case 13: case 20: mbs = 2376; break;
    case 21: mbs = 4752; break;
    case 22: case 30: mbs = 8100; break;
    case 31: mbs = 18000; break;
    case 32: mbs = 20480; break;
    case 40: case 41: mbs = 32768; break;
    case 42: mbs = 34816; break;
    case 50: mbs = 110400; break;
    case 51: case 52: mbs = 184320; break;
    default: mbs = 696320; break;
    }
    const uint32_t frames = mbs / ((uint32_t)s->width_mbs * s->height_map_units);
    return frames < 1 ? 1 : frames > 16 ? 16 : frames;
}

// How many decoded pictures may wait for display (C.4.5.3 with max_num_reorder_frames): the VUI's count, none for
// Baseline and intra-only profiles, else the level's whole DPB.
static uint32_t reorderDepth(const H264SPS *s)
{
    if (s->bitstream_restriction) return s->max_num_reorder_frames;
    if (s->profile_idc == 66 || ((s->constraint_flags & 0x10) && s->profile_idc >= 100)) return 0;
    return maxDpbFrames(s);
}

static bool supported(const H264SPS *s)
{
    return s->valid && s->chroma_format_idc == 1 && s->bit_depth_luma == 8 && s->bit_depth_chroma == 8 &&
           !s->separate_colour_plane && s->frame_mbs_only && s->width_mbs >= 3 && s->width_mbs * 16 <= 4096 &&
           s->height_map_units * 16 <= 4096;
}

// Picture buffers as averne's decoder sizes them: block-linear NV12 surfaces (pitch 64-aligned, height 32-aligned),
// one colocated-motion area per surface, the MB history and the history buffer, and the input buffer holding the
// engine's status, the slice offsets and the bitstream.
static int allocate(H264Decoder *d, const H264SPS *s)
{
    releaseBuffers(d);
    d->width_mbs = s->width_mbs;
    d->height_mbs = s->height_map_units;
    d->refs_allocated = s->max_num_ref_frames;
    d->pitch = ALIGN(d->width_mbs * 16, 64);
    d->aligned_height = ALIGN(d->height_mbs * 16, 32);
    d->luma_bytes = d->pitch * d->aligned_height;
    d->surface_bytes = d->luma_bytes + d->luma_bytes / 2;
    const uint32_t count = s->max_num_ref_frames + 1 < 2 ? 2 : s->max_num_ref_frames + 1;
    for (uint32_t i = 0; i < count; i++) {
        void *va;
        if (d->b.alloc(d->b.ctx, d->surface_bytes, &d->surface[i], &va)) return -1;
        d->surface_va[i] = va;
        d->surfaces = i + 1;
    }
    const uint32_t coloc = ALIGN(ALIGN(d->height_mbs, 2) * d->width_mbs * 64 - 63, 256);
    d->mbhist_bytes = ALIGN(d->width_mbs * 104, 256);
    d->history_bytes = ALIGN(d->width_mbs * 0x200 + 0x1100, 0x200);
    d->mbhist_off = coloc * count;
    d->history_off = d->mbhist_off + d->mbhist_bytes;
    void *va;
    if (d->b.alloc(d->b.ctx, d->history_off + d->history_bytes, &d->aux, &va)) return -1;
    d->max_slices = d->width_mbs * d->height_mbs;
    d->slices_off = 256;
    d->bits_off = ALIGN(d->slices_off + (d->max_slices + 1) * 4, 256);
    d->input_bytes = d->bits_off + (d->width_mbs * d->height_mbs >= 3600 ? 0x100000 : 0x40000);
    if (d->b.alloc(d->b.ctx, d->input_bytes, &d->input, &va)) return -1;
    d->input_va = va;
    return 0;
}

// The first SPS a PPS refers to (avcC carries one of each in practice).
int h264_decoder_configure(H264Decoder *d)
{
    for (unsigned i = 0; i < 256; i++) {
        if (!d->sets.pps[i].valid) continue;
        const H264SPS *s = &d->sets.sps[d->sets.pps[i].sps_id];
        if (!supported(s) || d->sets.pps[i].num_slice_groups != 1) return -1;
        d->sps = *s;
        if (allocate(d, s)) {
            releaseBuffers(d);
            return -3;
        }
        memset(d->refs, 0, sizeof(d->refs));
        d->max_long_term_frame_idx = -1;
        d->configured = true;
        return 0;
    }
    return -1;
}

// ---- Display order -------------------------------------------------------------------------------------------------

static void emitFirst(H264Decoder *d)
{
    uint32_t best = 0;
    for (uint32_t i = 1; i < d->pending_count; i++)
        if (d->pending[i].poc < d->pending[best].poc) best = i;
    void *item = d->pending[best].item;
    memmove(&d->pending[best], &d->pending[best + 1], (d->pending_count - best - 1) * sizeof(Pending));
    d->pending_count--;
    d->b.emit(d->b.ctx, item);
}

static void bump(H264Decoder *d, uint32_t keep)
{
    while (d->pending_count > keep) emitFirst(d);
}

void h264_decoder_flush(H264Decoder *d) { bump(d, 0); }

static void discardPending(H264Decoder *d)
{
    for (uint32_t i = 0; i < d->pending_count; i++) d->b.discard(d->b.ctx, d->pending[i].item);
    d->pending_count = 0;
}

// ---- Reference marking (8.2.5, frames) -----------------------------------------------------------------------------

static int32_t frameNumWrap(const H264Decoder *d, const RefFrame *f, uint32_t current)
{
    return f->frame_num > current ? (int32_t)f->frame_num - (1 << d->sps.log2_max_frame_num) : (int32_t)f->frame_num;
}

static unsigned refCount(const H264Decoder *d)
{
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) n += d->refs[i].ref != REF_NONE;
    return n;
}

// 8.2.5.3: the short-term frame with the smallest FrameNumWrap goes once the DPB holds max_num_ref_frames.
static void slidingWindow(H264Decoder *d, uint32_t current, unsigned limit)
{
    while (refCount(d) >= limit) {
        RefFrame *oldest = NULL;
        for (unsigned i = 0; i < 16; i++) {
            RefFrame *f = &d->refs[i];
            if (f->ref == REF_SHORT && (!oldest || frameNumWrap(d, f, current) < frameNumWrap(d, oldest, current)))
                oldest = f;
        }
        if (!oldest) break;
        oldest->ref = REF_NONE;
    }
}

static unsigned refLimit(const H264Decoder *d) { return d->sps.max_num_ref_frames ? d->sps.max_num_ref_frames : 1; }

static RefFrame *addRef(H264Decoder *d, const RefFrame *frame)
{
    bool taken[16] = {false};
    RefFrame *free = NULL;
    for (unsigned i = 0; i < 16; i++) {
        if (d->refs[i].ref != REF_NONE) taken[d->refs[i].slot] = true;
        else if (!free) free = &d->refs[i];
    }
    if (!free) return NULL;
    *free = *frame;
    for (uint8_t s = 0; s < 16; s++) {
        if (!taken[s]) {
            free->slot = s;
            break;
        }
    }
    return free;
}

static RefFrame *findShort(H264Decoder *d, int32_t picNum, uint32_t current)
{
    for (unsigned i = 0; i < 16; i++)
        if (d->refs[i].ref == REF_SHORT && frameNumWrap(d, &d->refs[i], current) == picNum) return &d->refs[i];
    return NULL;
}

static void dropLong(H264Decoder *d, uint32_t idx, const RefFrame *keep)
{
    for (unsigned i = 0; i < 16; i++)
        if (d->refs[i].ref == REF_LONG && d->refs[i].long_term_frame_idx == idx && &d->refs[i] != keep)
            d->refs[i].ref = REF_NONE;
}

// Marks after decoding a reference picture and adds it. Returns true for memory_management_control_operation 5.
static bool mark(H264Decoder *d, const H264Slice *sh, RefFrame *current)
{
    bool mmco5 = false;
    if (sh->nal_unit_type == H264_NAL_IDR) {
        if (sh->long_term_reference) {
            current->ref = REF_LONG;
            current->long_term_frame_idx = 0;
            d->max_long_term_frame_idx = 0;
        } else {
            d->max_long_term_frame_idx = -1;
        }
    } else if (sh->adaptive_ref_pic_marking) {
        const int32_t currPicNum = (int32_t)sh->frame_num;
        for (unsigned i = 0; i < sh->mmco_count; i++) {
            const H264MMCO *m = &sh->mmco[i];
            RefFrame *f;
            switch (m->type) {
            case 1:
                if ((f = findShort(d, currPicNum - (int32_t)(m->difference_of_pic_nums_minus1 + 1), sh->frame_num)))
                    f->ref = REF_NONE;
                break;
            case 2:
                for (unsigned j = 0; j < 16; j++)
                    if (d->refs[j].ref == REF_LONG && d->refs[j].long_term_frame_idx == m->long_term_pic_num)
                        d->refs[j].ref = REF_NONE;
                break;
            case 3:
                if ((f = findShort(d, currPicNum - (int32_t)(m->difference_of_pic_nums_minus1 + 1), sh->frame_num))) {
                    dropLong(d, m->long_term_frame_idx, f);
                    f->ref = REF_LONG;
                    f->long_term_frame_idx = m->long_term_frame_idx;
                }
                break;
            case 4:
                d->max_long_term_frame_idx = (int32_t)m->max_long_term_frame_idx_plus1 - 1;
                for (unsigned j = 0; j < 16; j++)
                    if (d->refs[j].ref == REF_LONG && (int32_t)d->refs[j].long_term_frame_idx > d->max_long_term_frame_idx)
                        d->refs[j].ref = REF_NONE;
                break;
            case 5:
                for (unsigned j = 0; j < 16; j++) d->refs[j].ref = REF_NONE;
                d->max_long_term_frame_idx = -1;
                mmco5 = true;
                break;
            case 6:
                dropLong(d, m->long_term_frame_idx, NULL);
                current->ref = REF_LONG;
                current->long_term_frame_idx = m->long_term_frame_idx;
                break;
            }
        }
    } else {
        slidingWindow(d, sh->frame_num, refLimit(d));
    }
    if (mmco5) current->frame_num = 0;
    // A stream that keeps more references than it declared loses its oldest short-term ones.
    slidingWindow(d, current->frame_num, refLimit(d));
    if (refCount(d) >= 16) slidingWindow(d, current->frame_num, 16);
    addRef(d, current);
    return mmco5;
}

// 8.2.5.2: frames skipped by frame_num become "non-existing" short-term references, slid in like decoded ones.
static void fillGap(H264Decoder *d, uint32_t frameNum)
{
    const uint32_t max = 1u << d->sps.log2_max_frame_num;
    uint32_t unused = (d->prev_ref_frame_num + 1) % max;
    if ((frameNum - unused + max) % max > refLimit(d)) {
        for (unsigned i = 0; i < 16; i++)
            if (d->refs[i].ref == REF_SHORT) d->refs[i].ref = REF_NONE;
        unused = (frameNum + max - refLimit(d)) % max;
    }
    for (; unused != frameNum; unused = (unused + 1) % max) {
        slidingWindow(d, unused, refLimit(d));
        const RefFrame filler = {.ref = REF_SHORT, .non_existing = true, .pic_idx = d->last_pic_idx,
                                 .frame_num = unused};
        addRef(d, &filler);
        if (unused < d->prev_frame_num) d->prev_frame_num_offset += max;
        d->prev_frame_num = d->prev_ref_frame_num = unused;
    }
}

// ---- Picture order count (8.2.1, frames) ---------------------------------------------------------------------------

static void pictureOrder(H264Decoder *d, const H264Slice *sh, int32_t *top, int32_t *bottom, int32_t *msbOut,
                         uint32_t *offsetOut)
{
    const H264SPS *s = &d->sps;
    const bool idr = sh->nal_unit_type == H264_NAL_IDR;
    const uint32_t maxFrameNum = 1u << s->log2_max_frame_num;
    *msbOut = 0;
    *offsetOut = 0;
    if (s->poc_type == 0) {
        const int32_t maxLsb = 1 << s->log2_max_poc_lsb, lsb = (int32_t)sh->poc_lsb;
        const int32_t prevMsb = idr ? 0 : d->prev_poc_msb, prevLsb = idr ? 0 : d->prev_poc_lsb;
        int32_t msb = prevMsb;
        if (lsb < prevLsb && prevLsb - lsb >= maxLsb / 2) msb = prevMsb + maxLsb;
        else if (lsb > prevLsb && lsb - prevLsb > maxLsb / 2) msb = prevMsb - maxLsb;
        *msbOut = msb;
        *top = msb + lsb;
        *bottom = *top + sh->delta_poc_bottom;
        return;
    }
    const uint32_t offset = idr ? 0
                            : d->prev_frame_num > sh->frame_num ? d->prev_frame_num_offset + maxFrameNum
                                                                : d->prev_frame_num_offset;
    *offsetOut = offset;
    if (s->poc_type == 1) {
        uint32_t abs = s->num_ref_frames_in_poc_cycle ? offset + sh->frame_num : 0;
        if (!sh->nal_ref_idc && abs) abs--;
        int32_t expected = 0;
        if (abs) {
            int32_t perCycle = 0;
            for (unsigned i = 0; i < s->num_ref_frames_in_poc_cycle; i++) perCycle += s->offset_for_ref_frame[i];
            const uint32_t cycle = (abs - 1) / s->num_ref_frames_in_poc_cycle;
            const uint32_t inCycle = (abs - 1) % s->num_ref_frames_in_poc_cycle;
            expected = (int32_t)cycle * perCycle;
            for (uint32_t i = 0; i <= inCycle; i++) expected += s->offset_for_ref_frame[i];
        }
        if (!sh->nal_ref_idc) expected += s->offset_for_non_ref_pic;
        *top = expected + sh->delta_poc[0];
        *bottom = *top + s->offset_for_top_to_bottom_field + sh->delta_poc[1];
        return;
    }
    const int32_t poc = idr ? 0 : 2 * (int32_t)(offset + sh->frame_num) - (sh->nal_ref_idc ? 0 : 1);
    *top = *bottom = poc;
}

// ---- One picture ---------------------------------------------------------------------------------------------------

static void entry(TegraNVDECSubmit *s, uint16_t method, uint16_t kind, uint32_t value, uint32_t offset)
{
    TegraNVDECEntry *e = &s->entries[s->entry_count++];
    e->method = method;
    e->kind = kind;
    e->value = value;
    e->offset = offset;
}

static void fillSetup(const H264Decoder *d, nvdec_h264_pic_s *p, const H264Slice *sh, uint8_t picIdx, int32_t top,
                      int32_t bottom)
{
    const H264SPS *s = &d->sps;
    const H264PPS *pps = &d->sets.pps[sh->pps_id];
    memset(p, 0, sizeof(*p));
    p->stream_len = d->bits_len + 16;
    p->slice_count = d->slices;
    p->mbhist_buffer_size = d->mbhist_bytes;
    p->log2_max_pic_order_cnt_lsb_minus4 = s->poc_type == 0 ? s->log2_max_poc_lsb - 4 : 0;
    p->delta_pic_order_always_zero_flag = s->delta_pic_order_always_zero;
    p->frame_mbs_only_flag = s->frame_mbs_only;
    p->PicWidthInMbs = (int)d->width_mbs;
    p->FrameHeightInMbs = (int)d->height_mbs;
    p->tileFormat = 0;          // TBL
    p->gob_height = 0;          // GOB_2
    p->entropy_coding_mode_flag = pps->entropy_coding_mode;
    p->pic_order_present_flag = pps->bottom_field_pic_order_present;
    p->num_ref_idx_l0_active_minus1 = pps->num_ref_idx_active[0] - 1;
    p->num_ref_idx_l1_active_minus1 = pps->num_ref_idx_active[1] - 1;
    p->deblocking_filter_control_present_flag = pps->deblocking_filter_control_present;
    p->redundant_pic_cnt_present_flag = pps->redundant_pic_cnt_present;
    p->transform_8x8_mode_flag = pps->transform_8x8_mode;
    p->pitch_luma = p->pitch_chroma = d->pitch;
    p->HistBufferSize = d->history_bytes / 256;
    p->direct_8x8_inference_flag = s->direct_8x8_inference;
    p->weighted_pred_flag = pps->weighted_pred;
    p->constrained_intra_pred_flag = pps->constrained_intra_pred;
    p->ref_pic_flag = sh->nal_ref_idc != 0;
    p->log2_max_frame_num_minus4 = s->log2_max_frame_num - 4;
    p->chroma_format_idc = s->chroma_format_idc;
    p->pic_order_cnt_type = s->poc_type;
    p->pic_init_qp_minus26 = pps->pic_init_qp - 26;
    p->chroma_qp_index_offset = pps->chroma_qp_index_offset;
    p->second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;
    p->weighted_bipred_idc = pps->weighted_bipred_idc;
    p->CurrPicIdx = picIdx;
    p->CurrColIdx = picIdx;
    p->frame_num = sh->frame_num;
    p->output_memory_layout = 0;    // NV12
    p->CurrFieldOrderCnt[0] = top;
    p->CurrFieldOrderCnt[1] = bottom;
    for (unsigned i = 0; i < 16; i++) {
        const RefFrame *f = &d->refs[i];
        if (f->ref == REF_NONE) continue;
        nvdec_dpb_entry_s *e = &p->dpb[f->slot];
        const unsigned marking = f->ref == REF_LONG ? 2 : 1;
        e->index = f->pic_idx;
        e->col_idx = f->pic_idx;
        e->state = 3;               // both fields of the frame are used for reference
        e->is_long_term = f->ref == REF_LONG;
        e->not_existing = f->non_existing;
        e->top_field_marking = marking;
        e->bottom_field_marking = marking;
        e->FieldOrderCnt[0] = (unsigned)f->top_poc;
        e->FieldOrderCnt[1] = (unsigned)f->bottom_poc;
        e->FrameIdx = f->ref == REF_LONG ? (int)f->long_term_frame_idx : (int)f->frame_num;
    }
    memcpy(p->WeightScale, pps->scaling4, sizeof(p->WeightScale));
    memcpy(p->WeightScale8x8, pps->scaling8, sizeof(p->WeightScale8x8));
    p->lossless_ipred8x8_filter_enable = 1;
    p->qpprime_y_zero_transform_bypass_flag = s->transform_bypass;
}

// The method stream of averne's nvtegra_h264_prepare_cmdbuf, as relocations the kernel resolves.
static void fillSubmit(H264Decoder *d, TegraNVDECSubmit *s, uint8_t picIdx)
{
    s->codec = TEGRA_NVDEC_CODEC_H264;
    s->setup_bytes = sizeof(nvdec_h264_pic_s);
    entry(s, NVC5B0_SET_APPLICATION_ID, TEGRA_NVDEC_ENTRY_VALUE, NVC5B0_SET_APPLICATION_ID_ID_H264, 0);
    entry(s, NVC5B0_SET_CONTROL_PARAMS, TEGRA_NVDEC_ENTRY_VALUE,
          NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_H264 | (1u << 4) /* GPTIMER_ON */ | (1u << 6) /* ERR_CONCEAL_ON */, 0);
    entry(s, NVC5B0_SET_PICTURE_INDEX, TEGRA_NVDEC_ENTRY_VALUE, d->picture_index++, 0);
    entry(s, NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->input, d->bits_off);
    entry(s, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->input, d->slices_off);
    entry(s, NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->input, 0);
    entry(s, NVC5B0_SET_COLOC_DATA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, 0);
    entry(s, NVC5B0_H264_SET_MBHIST_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->mbhist_off);
    entry(s, NVC5B0_SET_HISTORY_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, d->aux, d->history_off);
    for (uint16_t i = 0; i < MAX_SURFACES; i++) {
        const uint32_t surface = d->surface[i < d->surfaces ? i : picIdx];
        entry(s, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, surface, 0);
        entry(s, (uint16_t)(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, surface, d->luma_bytes);
    }
    entry(s, NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 1u << 8 /* AWAKEN */, 0);
    s->clean[0] = (TegraNVDECRange){d->input, d->slices_off, (d->slices + 1) * 4, 0};
    s->clean[1] = (TegraNVDECRange){d->input, d->bits_off, d->bits_len + 16, 0};
    s->clean_count = 2;
}

static int freeSurface(const H264Decoder *d)
{
    for (uint32_t i = 0; i < d->surfaces; i++) {
        bool used = false;
        for (unsigned j = 0; j < 16 && !used; j++)
            used = d->refs[j].ref != REF_NONE && !d->refs[j].non_existing && d->refs[j].pic_idx == i;
        if (!used) return (int)i;
    }
    return -1;
}

static int decodePicture(H264Decoder *d, const H264Slice *sh, void *token, bool reorder)
{
    const bool idr = sh->nal_unit_type == H264_NAL_IDR;
    const uint32_t maxFrameNum = 1u << d->sps.log2_max_frame_num;
    if (idr) {
        if (sh->no_output_of_prior_pics) discardPending(d);
        else bump(d, 0);
        memset(d->refs, 0, sizeof(d->refs));
        d->prev_poc_msb = d->prev_poc_lsb = 0;
        d->prev_frame_num_offset = d->prev_frame_num = d->prev_ref_frame_num = 0;
    } else if (sh->frame_num != d->prev_ref_frame_num && sh->frame_num != (d->prev_ref_frame_num + 1) % maxFrameNum) {
        fillGap(d, sh->frame_num);
    }
    int32_t top, bottom, msb;
    uint32_t frameNumOffset;
    pictureOrder(d, sh, &top, &bottom, &msb, &frameNumOffset);

    int picIdx = freeSurface(d);
    if (picIdx < 0) {                  // more references than declared: make room
        slidingWindow(d, sh->frame_num, refCount(d));
        if ((picIdx = freeSurface(d)) < 0) return -1;
    }
    static const uint8_t endOfStream[16] = {0, 0, 1, 0x0b, 0, 0, 0, 0, 0, 0, 1, 0x0b, 0, 0, 0, 0};
    memcpy(d->input_va + d->bits_off + d->bits_len, endOfStream, sizeof(endOfStream));
    ((uint32_t *)(d->input_va + d->slices_off))[d->slices] = d->bits_len;

    TegraNVDECSubmit submit;
    memset(&submit, 0, sizeof(submit));
    fillSetup(d, (nvdec_h264_pic_s *)submit.setup, sh, (uint8_t)picIdx, top, bottom);
    fillSubmit(d, &submit, (uint8_t)picIdx);
    TegraNVDECWait wait = {.timeout_us = 1000000, .invalidate_count = 2};
    wait.invalidate[0] = (TegraNVDECRange){d->input, 0, sizeof(nvdec_status_s), 0};
    wait.invalidate[1] = (TegraNVDECRange){d->surface[picIdx], 0, d->surface_bytes, 0};
    if (d->b.submit(d->b.ctx, &submit, &wait.fence) || d->b.wait(d->b.ctx, &wait)) return -2;
    const nvdec_status_s *status = (const nvdec_status_s *)d->input_va;
    d->last_pic_idx = (uint8_t)picIdx;

    const H264Picture picture = {
        .luma = d->surface_va[picIdx],
        .chroma = d->surface_va[picIdx] + d->luma_bytes,
        .pitch = d->pitch,
        .coded_width = d->width_mbs * 16,
        .coded_height = d->height_mbs * 16,
        .crop_x = d->sps.crop_left,
        .crop_y = d->sps.crop_top,
        .width = d->width_mbs * 16 - d->sps.crop_left - d->sps.crop_right,
        .height = d->height_mbs * 16 - d->sps.crop_top - d->sps.crop_bottom,
        .poc = top < bottom ? top : bottom,
        .status = status->error_status && !status->mbs_correctly_decoded ? (int)status->error_status : 0,
        .token = token,
    };
    void *item = d->b.decoded(d->b.ctx, &picture);

    bool mmco5 = false;
    if (sh->nal_ref_idc) {
        RefFrame current = {.ref = REF_SHORT, .pic_idx = (uint8_t)picIdx, .frame_num = sh->frame_num,
                            .top_poc = top, .bottom_poc = bottom};
        mmco5 = mark(d, sh, &current);
        if (mmco5) {                   // 8.2.1: the picture's order counts restart from it
            const int32_t temp = top < bottom ? top : bottom;
            top -= temp;
            bottom -= temp;
            for (unsigned i = 0; i < 16; i++) {
                if (d->refs[i].ref != REF_NONE && d->refs[i].pic_idx == picIdx && !d->refs[i].non_existing) {
                    d->refs[i].top_poc = top;
                    d->refs[i].bottom_poc = bottom;
                }
            }
            bump(d, 0);                // C.4.4: no_output_of_prior_pics_flag inferred 0
        }
        d->prev_poc_msb = mmco5 ? 0 : msb;
        d->prev_poc_lsb = mmco5 ? top : (int32_t)sh->poc_lsb;
        d->prev_ref_frame_num = mmco5 ? 0 : sh->frame_num;
    }
    d->prev_frame_num_offset = mmco5 ? 0 : frameNumOffset;
    d->prev_frame_num = mmco5 ? 0 : sh->frame_num;

    if (item) {
        if (d->pending_count == MAX_PENDING) emitFirst(d);
        d->pending[d->pending_count++] = (Pending){top < bottom ? top : bottom, item};
    }
    bump(d, reorder ? d->reorder : 0);
    return 1;
}

static int growInput(H264Decoder *d, uint64_t needed)
{
    uint64_t bytes = d->input_bytes;
    while (bytes < needed) bytes *= 2;
    if (bytes > (32u << 20)) return -1;
    uint32_t handle;
    void *va;
    if (d->b.alloc(d->b.ctx, bytes, &handle, &va)) return -1;
    memcpy(va, d->input_va, d->bits_off + d->bits_len);
    d->b.free(d->b.ctx, d->input);
    d->input = handle;
    d->input_va = va;
    d->input_bytes = bytes;
    return 0;
}

static int appendSlice(H264Decoder *d, const uint8_t *nal, size_t size)
{
    if (d->slices >= d->max_slices) return -1;
    const uint64_t needed = (uint64_t)d->bits_off + d->bits_len + 3 + size + 16;
    if (needed > d->input_bytes && growInput(d, needed)) return -1;
    ((uint32_t *)(d->input_va + d->slices_off))[d->slices++] = d->bits_len;
    uint8_t *dst = d->input_va + d->bits_off + d->bits_len;
    dst[0] = dst[1] = 0;
    dst[2] = 1;
    memcpy(dst + 3, nal, size);
    d->bits_len += (uint32_t)size + 3;
    return 0;
}

static bool sameGeometry(const H264Decoder *d, const H264SPS *s)
{
    return s->width_mbs == d->width_mbs && s->height_map_units == d->height_mbs &&
           s->max_num_ref_frames <= d->refs_allocated;
}

int h264_decoder_decode(H264Decoder *d, const uint8_t *data, size_t size, unsigned lengthSize, void *token,
                        bool reorder)
{
    if (!d->configured || lengthSize < 1 || lengthSize > 4) return -1;
    H264Slice first, slice;
    bool have = false;
    d->bits_len = d->slices = 0;
    for (size_t pos = 0; pos + lengthSize <= size;) {
        uint32_t length = 0;
        for (unsigned i = 0; i < lengthSize; i++) length = (length << 8) | data[pos + i];
        pos += lengthSize;
        if (!length || length > size - pos) return -1;
        const uint8_t *nal = data + pos;
        pos += length;
        const uint8_t type = nal[0] & 0x1f;
        if (type == H264_NAL_SPS || type == H264_NAL_PPS) {
            if (have || h264_decoder_parameter_set(d, nal, length)) return -1;
        } else if (type == H264_NAL_SLICE || type == H264_NAL_IDR) {
            H264Slice *sh = have ? &slice : &first;
            if (h264_parse_slice(&d->sets, nal, length, sh)) return -1;
            if (have && (!sh->first_mb_in_slice || sh->frame_num != first.frame_num || sh->pps_id != first.pps_id))
                break;                 // a second picture in one sample: only the first is decoded
            have = true;
            if (appendSlice(d, nal, length)) return -1;
        }
    }
    if (!have) return 0;
    const H264PPS *pps = &d->sets.pps[first.pps_id];
    const H264SPS *sps = &d->sets.sps[pps->sps_id];
    if (pps->num_slice_groups != 1 || !supported(sps)) return -1;
    if (!sameGeometry(d, sps)) {       // new sequence with other dimensions: start over at its IDR
        if (first.nal_unit_type != H264_NAL_IDR) return -1;
        bump(d, 0);
        const uint32_t bits = d->bits_len, slices = d->slices;
        uint8_t *saved = malloc(bits + (slices + 1) * 4);
        if (!saved) return -1;
        memcpy(saved, d->input_va + d->bits_off, bits);
        memcpy(saved + bits, d->input_va + d->slices_off, (slices + 1) * 4);
        const int ret = allocate(d, sps);
        if (!ret && (uint64_t)d->bits_off + bits + 16 <= d->input_bytes && slices <= d->max_slices) {
            memcpy(d->input_va + d->bits_off, saved, bits);
            memcpy(d->input_va + d->slices_off, saved + bits, (slices + 1) * 4);
        }
        free(saved);
        if (ret) {
            releaseBuffers(d);
            return -3;
        }
        d->configured = true;
        d->bits_len = bits;
        d->slices = slices;
    }
    d->sps = *sps;
    d->reorder = reorderDepth(sps);
    return decodePicture(d, &first, token, reorder);
}
