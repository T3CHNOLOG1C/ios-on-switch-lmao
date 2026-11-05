// SPDX-License-Identifier: BSD-2-Clause-Patent
// E3: NVENC encodes a 64x64 picture through the same path user clients use (tegra_nvenc_run: checker, relocations,
// scratch methods, kernel setup slot, syncpoint fence by interrupt), and NVDEC, proven at its own N3, decodes the
// slice NVENC wrote; the decoded picture is compared with the source. That checks firmware, method stream, the
// nvenc_drv.h setup layout on NVENC 5.0 (never run on this firmware before) and the input surface layout together,
// against an independent decoder, with nothing taken on trust from the encoder's own status.
//
// The vectors (selftest/E3Vectors.inc) come from the code that runs on the device: the plug-in's H264NVENC.c builds the
// two NVENC submits (an IDR, then a P picture predicting from NVENC's reconstruction of it; High profile, CABAC, 8x8
// transform, QP 26) and AVD's H264NVDEC.c the two NVDEC submits from this stream's own SPS, PPS and slice headers
// (SwitchVideoEncoders/test/e3gen). What nvenc_drv.h leaves open is found here, not assumed:
//   * the input layout: each TEGRA_NVENC_LAYOUT_* in turn (the source written that way, block_height/tiled_16x16 set
//     to match), until NVDEC's luma comes back within 30 dB of the source;
//   * how chroma is addressed: TEGRA_NVENC_CHROMA_OFFSET first (the surface-config offset nvenc_drv.h's figure
//     shows), then METHOD; both stay inside the buffers whatever the firmware reads (TegraNVENCABI.h);
//   * whether P pictures work: NVENC's 16x16-tiled reconstruction is never read by the CPU, so the P picture is the
//     only check of it; a P failure leaves NVENC on, intra only (TegraNVENCInfo.p_frames 0).
// Only a NAL unit that looks like the slice (start code, nal_unit_type, nal_ref_idc, a sane size) goes to NVDEC: NVDEC
// is published by now, and a job past its bound panics there. An NVENC job past its bound wedges NVENC (not published yet: one line,
// the engine parked). NVENC's own status is logged for diagnosis, not trusted.
// PSNR >= 30 dB is checked as MSE <= 65 (255^2 / 10^3); the log line gives MSE x100 and CRCs a later build can pin.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <string.h>
#include "../TegraNVDEC/TegraNVDEC.h"
#include "../TegraNVDEC/nvidia/clc5b0.h"
#include "TegraNVENC.h"
#include "TegraNVENCCheck.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../TegraNVDEC/nvidia/nvdec_drv.h"
#include "selftest/E3Vectors.inc"
#pragma clang diagnostic pop

static_assert(sizeof(kE3DecSetup0) == sizeof(nvdec_h264_pic_s) && sizeof(kE3DecSetup1) == sizeof(nvdec_h264_pic_s),
              "E3Vectors.inc was generated against another nvdec_drv.h");
static_assert(sizeof(kE3EncSetup0) == kE3EncSetupBytes && kE3EncSetupBytes <= TEGRA_NVENC_MAX_SETUP, "setup");

namespace {
constexpr uint32_t kEncHandles = 6, kDecHandles = 4, kMaxMse = 65, kNvdecWaitMs = 20000;
constexpr uint32_t kSlicesOff = 0x100, kBitsOff = 0x200;  // the NVDEC input buffer, as H264NVDEC lays it out
TegraNVENCBlock gEnc[kEncHandles], gDec[kDecHandles];

const char *const kLayoutName[TEGRA_NVENC_LAYOUTS] = {"pitch",        "BL2 (log2 1)", "BL2 (2 GOBs)",
                                                      "BL4 (log2 2)", "BL1",          "tiled 16x16"};

bool resolveEnc(void *, uint32_t handle, TegraNVENCBlock *block)
{
    if (!handle || handle >= kEncHandles) return false;
    *block = gEnc[handle];
    return true;
}

bool resolveDec(void *, uint32_t handle, TegraNVDECBlock *block)
{
    if (handle < kE3DecFirstHandle || handle >= kE3DecFirstHandle + kDecHandles) return false;
    const TegraNVENCBlock &b = gDec[handle - kE3DecFirstHandle];
    *block = TegraNVDECBlock{b.pa, b.size, b.va, b.first, b.count};
    return true;
}

uint64_t nowUs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// The source, picture f: a smooth diagonal gradient, a 16x16 square that moves 4 pixels right and 2 down in the P
// picture, and chroma ramps with the square in its own colour.
bool inSquare(uint32_t f, uint32_t x, uint32_t y)
{
    return x >= 16 + 4 * f && x < 32 + 4 * f && y >= 20 + 2 * f && y < 36 + 2 * f;
}
uint8_t lumaAt(uint32_t f, uint32_t x, uint32_t y)
{
    return inSquare(f, x, y) ? 200 : (uint8_t)(32 + 2 * x + y);
}
uint8_t chromaAt(uint32_t f, uint32_t x, uint32_t y, bool v)  // x, y in chroma samples
{
    if (inSquare(f, 2 * x, 2 * y)) return v ? 200 : 60;
    return v ? (uint8_t)(160 - y) : (uint8_t)(96 + x);
}

// h264enc_surface_offset (SwitchVideoEncoders/AVE/H264NVENC.c), which the host tests check.
size_t gob(uint32_t x, uint32_t y)
{
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
}
size_t blockLinear(uint32_t gobs, uint32_t pitch, uint32_t x, uint32_t y)
{
    const uint32_t rows = 8 * gobs;
    return (size_t)(y / rows) * pitch * rows + (size_t)(x / 64) * 512 * gobs + ((y % rows) / 8) * 512 + gob(x, y);
}
size_t surfaceOffset(uint32_t layout, uint32_t pitch, uint32_t x, uint32_t y)
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

void putSource(uint32_t f, uint32_t layout)
{
    uint8_t *in = gEnc[1].va;
    for (uint32_t y = 0; y < kE3Height; y++)
        for (uint32_t x = 0; x < kE3Width; x++) in[surfaceOffset(layout, kE3Pitch, x, y)] = lumaAt(f, x, y);
    for (uint32_t y = 0; y < kE3Height / 2; y++)
        for (uint32_t x = 0; x < kE3Width / 2; x++) {
            in[kE3ChromaOffset + surfaceOffset(layout, kE3Pitch, 2 * x, y)] = chromaAt(f, x, y, false);
            in[kE3ChromaOffset + surfaceOffset(layout, kE3Pitch, 2 * x + 1, y)] = chromaAt(f, x, y, true);
        }
}

struct Outcome {
    int ret;                 // tegra_nvenc_run/wait or tegra_nvdec_run/wait; -5 for a bad status or slice
    uint32_t bytes, status;  // NVENC's slice size and error_status | ucode_error_status << 2
    uint8_t head[6];         // the bitstream's first bytes
    uint32_t mse[3];         // x100, Y U V; ~0u when nothing decoded
    uint32_t sliceCrc, lumaCrc, decodeErrors;
};

// One NVENC picture (0: IDR, 1: P) in `layout` and `chroma`, then NVDEC on its slice.
void encodeAndDecode(uint32_t f, uint32_t layout, uint32_t chroma, Outcome *o)
{
    memset(o, 0, sizeof(*o));
    o->mse[0] = o->mse[1] = o->mse[2] = ~0u;
    putSource(f, layout);
    TegraNVENCSubmit *s = IONewZero(TegraNVENCSubmit, 1);
    if (!s) {
        o->ret = -12;
        return;
    }
    const E3Entry *entries = f ? kE3EncEntries1 : kE3EncEntries0;
    const uint32_t count = f ? sizeof(kE3EncEntries1) / sizeof(E3Entry) : sizeof(kE3EncEntries0) / sizeof(E3Entry);
    s->codec = TEGRA_NVENC_CODEC_H264;
    s->setup_bytes = kE3EncSetupBytes;
    memcpy(s->setup, f ? kE3EncSetup1 : kE3EncSetup0, kE3EncSetupBytes);
    nvenc_h264_drv_pic_setup_s *p = (nvenc_h264_drv_pic_setup_s *)s->setup;
    p->input_cfg.block_height = layout == TEGRA_NVENC_LAYOUT_BL2_LOG                                           ? 1
                                : layout == TEGRA_NVENC_LAYOUT_BL2_RAW || layout == TEGRA_NVENC_LAYOUT_BL4_LOG ? 2
                                                                                                               : 0;
    p->input_cfg.tiled_16x16 = layout == TEGRA_NVENC_LAYOUT_TILED16;
    const bool method = chroma == TEGRA_NVENC_CHROMA_METHOD;
    if (method)
        p->input_cfg.chroma_top_frm_offset = p->outputpic_cfg.chroma_top_frm_offset =
            p->refpic_cfg.chroma_top_frm_offset = 0;
    for (uint32_t i = 0; i < count; i++) {
        const E3Entry &e = entries[i];
        const bool chromaMethod =
            e.method == NVC5B7_SET_IN_CUR_PIC_CHROMA_U || e.method == NVC5B7_SET_OUT_REF_PIC_CHROMA ||
            (e.method >= NVC5B7_SET_IN_REF_PIC0_CHROMA && e.method < NVC5B7_SET_IN_REF_PIC0_CHROMA + 64);
        s->entries[s->entry_count++] = {e.method, e.kind, e.value, method && chromaMethod ? kE3ChromaOffset : e.offset};
    }
    s->clean[0] = {1, 0, kE3EncSizes[1], 0};
    s->clean_count = 1;
    bzero(gEnc[5].va, 4096);  // the status record
    tegra_nvenc_cache_flush(gEnc[5].va, 4096);
    uint32_t fence = 0;
    const char *why = nullptr;
    o->ret = tegra_nvenc_run(s, resolveEnc, nullptr, &fence, &why);
    IODelete(s, TegraNVENCSubmit, 1);
    if (o->ret || (o->ret = tegra_nvenc_wait(fence))) return;

    tegra_nvenc_cache_flush(gEnc[5].va, sizeof(nvenc_pic_stat_s));
    const nvenc_pic_stat_s *stat = (const nvenc_pic_stat_s *)gEnc[5].va;
    o->bytes = (stat->total_bit_count + 7) / 8;
    o->status = (uint32_t)stat->error_status | (uint32_t)stat->ucode_error_status << 2;
    if (o->bytes > kE3EncSizes[4]) o->bytes = kE3EncSizes[4];
    tegra_nvenc_cache_flush(gEnc[4].va, kE3EncSizes[4]);
    const uint8_t *bits = gEnc[4].va;
    memcpy(o->head, bits, sizeof(o->head));
    o->sliceCrc = crc32(0, bits, o->bytes);
    // The slice: the first NAL unit of nal_unit_type 5 (IDR) or 1 (P) with nal_ref_idc set (NVENC may put an access
    // unit delimiter, SEI or filler ahead of it), up to the next start code; with room for a slice body. Only that NAL
    // unit goes to NVDEC, as H264NVDEC only ever gives it slices.
    uint32_t start = ~0u, end = o->bytes;
    for (uint32_t i = 0; i + 3 < o->bytes; i++) {
        if (bits[i] || bits[i + 1] || bits[i + 2] != 1) continue;
        if (start != ~0u) {
            end = i && !bits[i - 1] ? i - 1 : i;  // a 4-byte start code's leading zero belongs to the next unit
            break;
        }
        if ((bits[i + 3] & 0x9f) == (f ? 0x01 : 0x05) && (bits[i + 3] & 0x60)) start = i;
        i += 2;
    }
    while (start != ~0u && end > start + 4 && !bits[end - 1]) end--;  // trailing_zero_8bits
    if (start == ~0u || end - start < 12) {
        o->ret = -5;
        return;
    }
    const uint32_t len = end - start;

    // NVDEC: the captured submit, this slice in its input buffer, stream length and slice count filled in.
    TegraNVDECSubmit *d = IONewZero(TegraNVDECSubmit, 1);
    if (!d) {
        o->ret = -12;
        return;
    }
    const E3Entry *dec = f ? kE3DecEntries1 : kE3DecEntries0;
    const uint32_t decCount = f ? sizeof(kE3DecEntries1) / sizeof(E3Entry) : sizeof(kE3DecEntries0) / sizeof(E3Entry);
    d->codec = TEGRA_NVDEC_CODEC_H264;
    d->setup_bytes = sizeof(nvdec_h264_pic_s);
    memcpy(d->setup, f ? kE3DecSetup1 : kE3DecSetup0, sizeof(nvdec_h264_pic_s));
    nvdec_h264_pic_s *pic = (nvdec_h264_pic_s *)d->setup;
    pic->stream_len = len + 16;
    pic->slice_count = 1;
    uint32_t surface = 0;
    for (uint32_t i = 0; i < decCount; i++) {
        d->entries[d->entry_count++] = {dec[i].method, dec[i].kind, dec[i].value, dec[i].offset};
        if (dec[i].method == NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * pic->CurrPicIdx) surface = dec[i].value;
    }
    const uint32_t (*clean)[3] = f ? kE3DecClean1 : kE3DecClean0;
    const uint32_t input = clean[0][0];
    TegraNVENCBlock &in = gDec[input - kE3DecFirstHandle];
    static const uint8_t kEndOfStream[16] = {0, 0, 1, 0x0b, 0, 0, 0, 0, 0, 0, 1, 0x0b, 0, 0, 0, 0};
    bzero(in.va, kSlicesOff);
    ((uint32_t *)(in.va + kSlicesOff))[0] = 0;
    ((uint32_t *)(in.va + kSlicesOff))[1] = len;
    memcpy(in.va + kBitsOff, bits + start, len);
    memcpy(in.va + kBitsOff + len, kEndOfStream, sizeof(kEndOfStream));
    tegra_nvdec_cache_clean(in.va, kSlicesOff);  // the status record, zeroed
    d->clean[0] = {input, kSlicesOff, 8, 0};
    d->clean[1] = {input, kBitsOff, len + 16, 0};
    d->clean_count = 2;
    o->ret = tegra_nvdec_run(d, resolveDec, nullptr, &fence, &why);
    IODelete(d, TegraNVDECSubmit, 1);
    if (o->ret || (o->ret = tegra_nvdec_wait(fence))) return;

    const TegraNVENCBlock &out = gDec[surface - kE3DecFirstHandle];
    tegra_nvdec_cache_flush(in.va, sizeof(nvdec_status_s));
    tegra_nvdec_cache_flush(out.va, kE3DecSizes[surface - kE3DecFirstHandle]);
    o->decodeErrors = ((const nvdec_status_s *)in.va)->mbs_in_error;
    // NVDEC's surface: block linear, GOB height 2, pitch 64, chroma 4096 bytes in (H264NVDEC's allocation).
    uint64_t sse[3] = {0, 0, 0};
    uint32_t crc = 0;
    for (uint32_t y = 0; y < kE3Height; y++)
        for (uint32_t x = 0; x < kE3Width; x++) {
            const uint8_t v = out.va[blockLinear(2, kE3Pitch, x, y)];
            const int e = (int)v - lumaAt(f, x, y);
            sse[0] += (uint64_t)(e * e);
            crc = crc32(crc, &v, 1);
        }
    for (uint32_t y = 0; y < kE3Height / 2; y++)
        for (uint32_t x = 0; x < kE3Width / 2; x++)
            for (uint32_t c = 0; c < 2; c++) {
                const int e =
                    (int)out.va[kE3ChromaOffset + blockLinear(2, kE3Pitch, 2 * x + c, y)] - chromaAt(f, x, y, c);
                sse[1 + c] += (uint64_t)(e * e);
            }
    o->lumaCrc = crc;
    o->mse[0] = (uint32_t)(sse[0] * 100 / (kE3Width * kE3Height));
    o->mse[1] = (uint32_t)(sse[1] * 100 / (kE3Width * kE3Height / 4));
    o->mse[2] = (uint32_t)(sse[2] * 100 / (kE3Width * kE3Height / 4));
}

bool good(uint32_t mse)
{
    return mse <= kMaxMse * 100;
}

// "pitch: err -60" / "BL1: status 0x1 0 bytes 00 00 00 00 00 00" / "BL2: Y 123.45", for the FAIL line.
void describe(char *line, size_t size, uint32_t layout, const Outcome &o)
{
    const size_t used = strlen(line);
    if (used + 80 > size) return;
    if (o.mse[0] != ~0u)
        snprintf(line + used, size - used, " %s: MSE %u.%02u/%u.%02u/%u.%02u;", kLayoutName[layout], o.mse[0] / 100,
                 o.mse[0] % 100, o.mse[1] / 100, o.mse[1] % 100, o.mse[2] / 100, o.mse[2] % 100);
    else
        snprintf(line + used, size - used, " %s: %d status %#x %u bytes %02x %02x %02x %02x %02x %02x;",
                 kLayoutName[layout], o.ret, o.status, o.bytes, o.head[0], o.head[1], o.head[2], o.head[3], o.head[4],
                 o.head[5]);
}

int run(TegraNVENCProven *proven, char *line, size_t size)
{
    if (!tegra_nvdec_await(kNvdecWaitMs)) {  // settled before E0; here only if NVDEC stopped decoding since
        snprintf(line, size, "E3 FAIL NVDEC is not decoding: nothing to check NVENC's picture with");
        return -19;
    }
    for (uint32_t h = 1; h < kEncHandles; h++)
        if (tegra_nvenc_arena_alloc(kE3EncSizes[h], &gEnc[h])) {
            snprintf(line, size, "E3 FAIL no arena memory");
            return -12;
        }
    for (uint32_t h = 0; h < kDecHandles; h++)
        if (tegra_nvenc_arena_alloc(kE3DecSizes[h], &gDec[h])) {
            snprintf(line, size, "E3 FAIL no arena memory");
            return -12;
        }
    const uint64_t began = nowUs();
    snprintf(line, size, "E3 FAIL no input layout decoded back (High CABAC 64x64 IDR, NVENC result per layout):");
    Outcome idr = {}, p = {};
    uint32_t layout = 0, chroma = TEGRA_NVENC_CHROMA_OFFSET;
    for (; layout < TEGRA_NVENC_LAYOUTS; layout++) {
        encodeAndDecode(0, layout, chroma, &idr);
        if (idr.ret == -60 || idr.ret == -19) {  // NVENC wedged: nothing more to try
            describe(line, size, layout, idr);
            return idr.ret;
        }
        if (good(idr.mse[0])) break;
        describe(line, size, layout, idr);
    }
    if (layout == TEGRA_NVENC_LAYOUTS) return -5;
    if (!good(idr.mse[1]) || !good(idr.mse[2])) {
        const Outcome offset = idr;
        chroma = TEGRA_NVENC_CHROMA_METHOD;
        encodeAndDecode(0, layout, chroma, &idr);
        if (!good(idr.mse[0]) || !good(idr.mse[1]) || !good(idr.mse[2])) {
            snprintf(line, size, "E3 FAIL %s luma decodes but chroma does not:", kLayoutName[layout]);
            describe(line, size, layout, offset);
            describe(line, size, layout, idr);
            return idr.ret == -60 ? -60 : -5;
        }
    }
    encodeAndDecode(1, layout, chroma, &p);
    if (p.ret == -60 || p.ret == -19) {
        snprintf(line, size, "E3 FAIL the P picture wedged NVENC:");
        describe(line, size, layout, p);
        return p.ret;
    }
    proven->inputLayout = layout;
    proven->chromaMode = chroma;
    proven->pFrames = good(p.mse[0]) && good(p.mse[1]) && good(p.mse[2]);
    snprintf(line, size,
             "E3 ok H.264 64x64 High CABAC QP 26 decoded back by NVDEC in %llu us: input layout %s, chroma %s; IDR %u "
             "bytes MSE %u.%02u/%u.%02u/%u.%02u (slice CRC %#010x, luma CRC %#010x); P %u bytes MSE %u.%02u/%u.%02u/"
             "%u.%02u (ret %d, status %#x, slice CRC %#010x): %s",
             nowUs() - began, kLayoutName[layout], chroma ? "method" : "offset", idr.bytes, idr.mse[0] / 100,
             idr.mse[0] % 100, idr.mse[1] / 100, idr.mse[1] % 100, idr.mse[2] / 100, idr.mse[2] % 100, idr.sliceCrc,
             idr.lumaCrc, p.bytes, p.mse[0] / 100, p.mse[0] % 100, p.mse[1] / 100, p.mse[1] % 100, p.mse[2] / 100,
             p.mse[2] % 100, p.ret, p.status, p.sliceCrc,
             proven->pFrames ? "P pictures on" : "P pictures off (intra only)");
    return 0;
}
}  // namespace

extern "C" int tegra_nvenc_self_test(TegraNVENCProven *proven)
{
    char line[1024];
    line[0] = 0;
    const int ret = run(proven, line, sizeof(line));
    IOLog("TegraNVENC: %s\n", line);
    if (ret != -60) {  // a job still in an engine keeps its buffers
        for (uint32_t h = 1; h < kEncHandles; h++) tegra_nvenc_arena_free(&gEnc[h]);
        for (uint32_t h = 0; h < kDecHandles; h++) tegra_nvenc_arena_free(&gDec[h]);
    }
    return ret;
}
