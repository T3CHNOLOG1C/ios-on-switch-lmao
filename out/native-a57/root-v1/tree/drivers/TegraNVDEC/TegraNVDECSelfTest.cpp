// SPDX-License-Identifier: BSD-2-Clause-Patent
// N3 (switch-nvdec >= 2): NVDEC decodes one embedded H.264 picture through the same path user clients use
// (tegra_nvdec_run: checker, relocations, kernel setup slot, syncpoint fence by interrupt). The picture is a 64x64
// High-profile IDR (CABAC, 8x8 transform); its picture setup, bitstream and slice offsets were produced on the host by
// the plug-in's own H.264 code (SwitchVideoDecoders/test/h264_host --selftest), and the expected luma CRC is x264's
// reconstruction. Passing proves firmware, method stream, setup layout and the block-linear surface layout together.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <string.h>
#include "TegraNVDEC.h"
#include "nvidia/nvdec_drv.h"
#include "nvidia/clc5b0.h"
#include "selftest/H264Picture64.inc"

namespace {
enum { kInput = 1, kAux = 2, kSurface = 3 };
constexpr uint32_t kSlicesOff = 256, kBitsOff = 512, kMbhistOff = 0x8000, kHistoryOff = 0x9000;
TegraNVDECBlock gBlocks[4];

bool resolve(void *, uint32_t handle, TegraNVDECBlock *block)
{
    if (handle < kInput || handle > kSurface) return false;
    *block = gBlocks[handle];
    return true;
}

// Byte offset of (x, y) in a block-linear plane with 64Bx8 GOBs stacked two to a block (GOB height 2).
uint32_t blockLinear(uint32_t x, uint32_t y, uint32_t pitch)
{
    const uint32_t gob = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
    return (y / 16) * pitch * 16 + (x / 64) * 1024 + ((y % 16) / 8) * 512 + gob;
}

uint32_t crc32(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    return crc;
}

void entry(TegraNVDECSubmit *s, uint16_t method, uint16_t kind, uint32_t value, uint32_t offset)
{
    s->entries[s->entry_count++] = {method, kind, value, offset};
}

uint64_t nowUs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

int run(char *line, size_t size)
{
    const nvdec_h264_pic_s *setup = (const nvdec_h264_pic_s *)kTestSetup;
    const uint32_t width = kTestWidthMbs * 16, height = kTestHeightMbs * 16, pitch = setup->pitch_luma;
    const uint32_t lumaBytes = pitch * ((height + 31) & ~31u);
    for (int i = kInput; i <= kSurface; i++) {
        if (tegra_nvdec_arena_alloc(64 * 1024, &gBlocks[i])) {
            snprintf(line, size, "N3 FAIL no arena memory");
            return -12;
        }
    }
    memcpy(gBlocks[kInput].va + kSlicesOff, kTestSliceOffsets, sizeof(kTestSliceOffsets));
    memcpy(gBlocks[kInput].va + kBitsOff, kTestBitstream, sizeof(kTestBitstream));

    TegraNVDECSubmit *s = IONewZero(TegraNVDECSubmit, 1);
    if (!s) return -12;
    s->codec = TEGRA_NVDEC_CODEC_H264;
    s->setup_bytes = sizeof(kTestSetup);
    memcpy(s->setup, kTestSetup, sizeof(kTestSetup));
    entry(s, NVC5B0_SET_APPLICATION_ID, TEGRA_NVDEC_ENTRY_VALUE, NVC5B0_SET_APPLICATION_ID_ID_H264, 0);
    entry(s, NVC5B0_SET_CONTROL_PARAMS, TEGRA_NVDEC_ENTRY_VALUE,
          NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_H264 | 1u << 4 | 1u << 6, 0);
    entry(s, NVC5B0_SET_PICTURE_INDEX, TEGRA_NVDEC_ENTRY_VALUE, 0, 0);
    entry(s, NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kInput, kBitsOff);
    entry(s, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kInput, kSlicesOff);
    entry(s, NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kInput, 0);
    entry(s, NVC5B0_SET_COLOC_DATA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kAux, 0);
    entry(s, NVC5B0_H264_SET_MBHIST_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kAux, kMbhistOff);
    entry(s, NVC5B0_SET_HISTORY_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kAux, kHistoryOff);
    for (uint16_t i = 0; i < 17; i++) {
        entry(s, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, kSurface, 0);
        entry(s, (uint16_t)(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, kSurface, lumaBytes);
    }
    entry(s, NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 1u << 8, 0);
    s->clean[0] = {kInput, kSlicesOff, (uint32_t)sizeof(kTestSliceOffsets), 0};
    s->clean[1] = {kInput, kBitsOff, (uint32_t)sizeof(kTestBitstream), 0};
    s->clean_count = 2;

    const uint64_t began = nowUs();
    uint32_t fence = 0;
    const char *why = nullptr;
    int ret = tegra_nvdec_run(s, resolve, nullptr, &fence, &why);
    IODelete(s, TegraNVDECSubmit, 1);
    if (ret) {
        snprintf(line, size, "N3 FAIL submit refused (%d, %s)", ret, why ? why : "engine");
        return ret;
    }
    if ((ret = tegra_nvdec_wait(fence))) {
        snprintf(line, size, "N3 FAIL decode did not finish (%s; see the job line)",
                 ret == -60  ? "past its bound"
                 : ret == -5 ? "done, but its interrupt never came"
                             : "engine refused");
        return ret;
    }
    const uint64_t us = nowUs() - began;
    tegra_nvdec_cache_flush(gBlocks[kInput].va, sizeof(nvdec_status_s));
    tegra_nvdec_cache_flush(gBlocks[kSurface].va, lumaBytes * 3 / 2);
    const nvdec_status_s *status = (const nvdec_status_s *)gBlocks[kInput].va;
    uint32_t crc = 0xffffffffu;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) crc = crc32(crc, gBlocks[kSurface].va[blockLinear(x, y, pitch)]);
    crc = ~crc;
    const bool ok = crc == kTestLumaCrc && !status->error_status && !status->mbs_in_error;
    snprintf(line, size,
             "N3 %s H.264 %ux%u IDR in %llu us: %u MBs decoded, %u in error, error_status %#x, %u cycles; "
             "luma CRC %#010x (expected %#010x)",
             ok ? "ok" : "FAIL", width, height, us, status->mbs_correctly_decoded, status->mbs_in_error,
             status->error_status, status->cycle_count, crc, kTestLumaCrc);
    return ok ? 0 : -5;
}
}  // namespace

extern "C" int tegra_nvdec_self_test(void)
{
    char line[256];
    line[0] = 0;
    const int ret = run(line, sizeof(line));
    IOLog("TegraNVDEC: %s\n", line);
    if (ret != -60)  // a job still in the engine keeps its buffers
        for (int i = kInput; i <= kSurface; i++) tegra_nvdec_arena_free(&gBlocks[i]);
    return ret;
}
