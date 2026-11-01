// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the kernel accepts in a TegraNVDECSubmit (TegraNVDECABI.h), shared with the host tests so a stream the plug-in
// builds is checked by the same code before it ever reaches hardware.
//
// NVDEC writes physical memory directly (no SMMU), so every size that steers its writes is checked here against the
// buffer the relocation points into: the decoded surfaces (pitch x 32-aligned height, NV12 block linear), the
// colocated-motion slots (one per picture index used), the MB history, the history buffer and the status record.
// Reads (bitstream, slice offsets) are bounded by their buffers too. Only H.264 frames are accepted for now.
#ifndef TEGRA_NVDEC_CHECK_H
#define TEGRA_NVDEC_CHECK_H
#include <stdint.h>
#include "TegraNVDECABI.h"
#include "nvidia/nvdec_drv.h"
#include "nvidia/clc5b0.h"

// avail[i]: bytes from entry i's relocated address to the end of its buffer (ignored for values).
// Returns NULL when the submit is acceptable, else the reason.
static inline const char *tegra_nvdec_check(const TegraNVDECSubmit *s, const uint64_t *avail)
{
    enum { LUMA = NVC5B0_SET_PICTURE_LUMA_OFFSET0, CHROMA = NVC5B0_SET_PICTURE_CHROMA_OFFSET0, SLOTS = 17 };
    if (s->codec != TEGRA_NVDEC_CODEC_H264) return "codec";
    if (s->entry_count < 1 || s->entry_count > TEGRA_NVDEC_MAX_ENTRIES || s->clean_count > TEGRA_NVDEC_MAX_RANGES)
        return "counts";
    if (s->setup_bytes != sizeof(nvdec_h264_pic_s)) return "setup size";
    uint64_t bitstream = 0, slices = 0, status = 0, coloc = 0, mbhist = 0, history = 0;
    uint64_t luma[SLOTS] = {0}, chroma[SLOTS] = {0};
    uint32_t seen = 0, pictures = 0;
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        const uint16_t m = e->method;
        if (e->kind == TEGRA_NVDEC_ENTRY_VALUE) {
            uint32_t bit;
            switch (m) {
            case NVC5B0_SET_APPLICATION_ID:
                if (e->value != s->codec) return "application id";
                bit = 0;
                break;
            case NVC5B0_SET_CONTROL_PARAMS:  // codec type, GPTIMER_ON, RET_ERROR, ERR_CONCEAL_ON, MBTIMER_ON
                if ((e->value & 0xf) != s->codec || (e->value & ~0x207fu) != 0) return "control params";
                bit = 1;
                break;
            case NVC5B0_SET_PICTURE_INDEX: bit = 2; break;
            case NVC5B0_SET_WATCHDOG_TIMER: bit = 3; break;
            case NVC5B0_EXECUTE:  // AWAKEN only: NOTIFY would release a semaphore at a raw address
                if (e->value & ~0x100u) return "execute flags";
                if (i != s->entry_count - 1) return "execute not last";
                bit = 4;
                break;
            default: return "value method";
            }
            if (seen & (1u << bit)) return "repeated method";
            seen |= 1u << bit;
            continue;
        }
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC) return "entry kind";
        if (e->offset & 0xff) return "relocation alignment";
        const uint64_t a = avail[i];
        uint64_t *slot = 0;
        switch (m) {
        case NVC5B0_SET_IN_BUF_BASE_OFFSET: slot = &bitstream; break;
        case NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET: slot = &slices; break;
        case NVC5B0_SET_NVDEC_STATUS_OFFSET: slot = &status; break;
        case NVC5B0_SET_COLOC_DATA_OFFSET: slot = &coloc; break;
        case NVC5B0_H264_SET_MBHIST_BUF_OFFSET: slot = &mbhist; break;
        case NVC5B0_SET_HISTORY_OFFSET: slot = &history; break;
        default:
            if (m >= LUMA && m < LUMA + 4 * SLOTS && !(m & 3))
                slot = &luma[(m - LUMA) / 4];
            else if (m >= CHROMA && m < CHROMA + 4 * SLOTS && !(m & 3))
                slot = &chroma[(m - CHROMA) / 4];
            else
                return "relocated method";
            pictures++;
        }
        if (*slot) return "repeated relocation";
        *slot = a ? a : 1;  // a relocation exactly at a buffer's end still counts as present
    }
    if ((seen & 0x13) != 0x13) return "missing application id, control params or execute";
    if (!bitstream || !slices || !status || !coloc || !mbhist || !history || pictures != 2 * SLOTS)
        return "missing relocation";

    const nvdec_h264_pic_s *p = (const nvdec_h264_pic_s *)s->setup;
    const uint8_t *otf = (const uint8_t *)&p->encryption_params;
    for (uint32_t i = 0; i < sizeof(p->encryption_params); i++)
        if (otf[i]) return "encryption parameters";
    if (p->hint_dump_en) return "hint dump";
    const uint32_t w = (uint32_t)p->PicWidthInMbs, h = (uint32_t)p->FrameHeightInMbs;
    if (p->PicWidthInMbs < 3 || w > 256 || p->FrameHeightInMbs < 1 || h > 256) return "dimensions";
    if (!p->frame_mbs_only_flag || p->field_pic_flag || p->MbaffFrameFlag) return "not progressive";
    if (p->tileFormat || p->gob_height || p->output_memory_layout || p->chroma_format_idc != 1) return "surface format";
    if (p->luma_top_offset || p->luma_bot_offset || p->luma_frame_offset || p->chroma_top_offset ||
        p->chroma_bot_offset || p->chroma_frame_offset)
        return "surface offsets";
    if (p->pitch_luma != p->pitch_chroma || (p->pitch_luma & 63) || p->pitch_luma < w * 16 || p->pitch_luma > 16384)
        return "pitch";
    const uint64_t lumaBytes = (uint64_t)p->pitch_luma * ((h * 16 + 31) & ~31u);
    for (uint32_t i = 0; i < SLOTS; i++)
        if (luma[i] < lumaBytes || chroma[i] < lumaBytes / 2) return "surface too small";
    uint32_t cols = p->CurrColIdx;
    if (p->CurrPicIdx >= SLOTS || p->CurrColIdx >= SLOTS) return "picture index";
    for (uint32_t i = 0; i < 16; i++) {
        if (p->dpb[i].index >= SLOTS || p->dpb[i].col_idx >= SLOTS || p->dpb[i].output_memory_layout)
            return "dpb entry";
        if (p->dpb[i].col_idx > cols) cols = p->dpb[i].col_idx;
    }
    const uint64_t colocBytes = ((((h + 1) & ~1u) * w * 64 - 63 + 255) & ~255u);
    if (coloc < colocBytes * (cols + 1)) return "colocated buffer";
    if (p->mbhist_buffer_size < ((w * 104 + 255) & ~255u) || mbhist < p->mbhist_buffer_size) return "mb history";
    if ((uint64_t)p->HistBufferSize * 256 < ((w * 0x200 + 0x1100 + 0x1ff) & ~0x1ffu) ||
        history < (uint64_t)p->HistBufferSize * 256)
        return "history";
    if (status < sizeof(nvdec_status_s)) return "status";
    if (!p->slice_count || slices < ((uint64_t)p->slice_count + 1) * 4 || !p->stream_len || bitstream < p->stream_len)
        return "bitstream";
    return 0;
}

#endif
