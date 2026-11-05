// SPDX-License-Identifier: BSD-2-Clause-Patent
// VIC 4.0 (Tegra X1's Video Image Compositor, host1x class 0x5D) job generation: the configuration structure the
// falcon firmware reads, the class methods that point it at the surfaces, the colour matrices, and the checker that
// bounds every byte the engine may read or write. Plain C, header only: TegraVIC (kernel C++) and the host test
// (drivers/TegraVIC/test/vic_host.c, tests/test_tegra_vic.py) compile the same code.
//
// Sources (written from them; layouts checked bit for bit by the host test against libdrm's MIT-licensed vic40.h):
//   libdrm tests/tegra/vic40.h, vic40.c, vic.h (NVIDIA, MIT): ConfigStruct and its sub-structures, the class methods
//   SET_APPLICATION_ID 0x200, EXECUTE 0x300, SET_SURFACE0_SLOT0_LUMA/CHROMA_U 0x400/0x404, SET_CONTROL_PARAMS 0x704,
//   SET_CONFIG_STRUCT_OFFSET 0x708, SET_OUTPUT_SURFACE_LUMA 0x720; pixel formats; cache widths.
//   cyndis/vaapi-tegra-driver engines/vic.cpp, engine_headers/vic04.h (NVIDIA, MIT): SET_FILTER_STRUCT_OFFSET 0x70c,
//   SET_OUTPUT_SURFACE_CHROMA_U 0x724, the 4:2:0 chroma plane geometry, the colour matrix encoding (coefficient
//   1.0 = 1 << (r_shift + 8), offsets in 1/256 of a 10-bit step).
//   switch-frame-tap tools/vic_csc.py (measured on a T210B01 VIC, 528 values reproduced exactly; read, not copied):
//   the 10-bit pipeline (in10 = in8 << 2, out8 = out10 >> 2), both matrix stages truncate, and which pipeline channel
//   each NV12 byte lands in. TegraVIC's boot self-test (V4) proves these on this unit before anything is published.
//   averne's FFmpeg nvtegra (libavutil/clb0b6.h, hwcontext_nvtegra.c; read, not copied): CONTROL_PARAMS GPTIMER_ON
//   and FALCON_CONTROL, PipeConfig downsample 1.0 = 4 (U9.2), the chroma siting values, NV12 = T_Y8___U8V8_N420.
#ifndef TEGRA_VIC_CONFIG_H
#define TEGRA_VIC_CONFIG_H
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Class methods (byte offsets in class 0xB0B6) -----------------------------------------------------------------
enum {
    TEGRA_VIC_M_SET_APPLICATION_ID = 0x200,
    TEGRA_VIC_M_EXECUTE = 0x300,
    TEGRA_VIC_M_SURFACE0_SLOT0_LUMA = 0x400,
    TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U = 0x404,
    TEGRA_VIC_M_SET_CONTROL_PARAMS = 0x704,
    TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET = 0x708,
    TEGRA_VIC_M_SET_FILTER_STRUCT_OFFSET = 0x70c,
    TEGRA_VIC_M_SET_FCE_UCODE_SIZE = 0x71c,
    TEGRA_VIC_M_OUTPUT_LUMA = 0x720,
    TEGRA_VIC_M_OUTPUT_CHROMA_U = 0x724,
    TEGRA_VIC_M_SET_FCE_UCODE_OFFSET = 0x72c,
};
#define TEGRA_VIC_EXECUTE_AWAKEN         (1u << 8)
#define TEGRA_VIC_CONTROL_GPTIMER_ON     (1u << 0)
#define TEGRA_VIC_CONTROL_FALCON_CONTROL (1u << 8)

// --- Configuration structure (VIC 4.0 ConfigStruct): byte offsets of its parts ------------------------------------
enum {
    TEGRA_VIC_CONFIG_BYTES = 0x610,
    TEGRA_VIC_PIPE = 0x000,            // PipeConfig, 16
    TEGRA_VIC_OUTPUT = 0x010,          // OutputConfig, 16
    TEGRA_VIC_OUTPUT_SURFACE = 0x020,  // OutputSurfaceConfig, 16
    TEGRA_VIC_OUT_MATRIX = 0x030,      // MatrixStruct outColorMatrixStruct, 32
    TEGRA_VIC_CLEAR_RECTS = 0x050,     // ClearRectStruct[4], 64
    TEGRA_VIC_SLOTS = 0x090,           // SlotStruct[8], 0xb0 each
    TEGRA_VIC_SLOT_BYTES = 0xb0,
    TEGRA_VIC_SLOT_CONFIG = 0x00,    // within a SlotStruct: SlotConfig, 64
    TEGRA_VIC_SLOT_SURFACE = 0x40,   // SlotSurfaceConfig, 16
    TEGRA_VIC_SLOT_LUMA_KEY = 0x50,  // LumaKeyStruct, 16
    TEGRA_VIC_SLOT_MATRIX = 0x60,    // MatrixStruct colorMatrixStruct, 32
    TEGRA_VIC_SLOT_GAMUT = 0x80,     // MatrixStruct gamutMatrixStruct, 32
    TEGRA_VIC_SLOT_BLEND = 0xa0,     // BlendingSlotStruct, 16
};

// Pixel formats (NVB0B6_T_*), block kinds, cache widths, filter lengths, alpha fill modes.
enum {
    TEGRA_VIC_T_A8B8G8R8 = 31,
    TEGRA_VIC_T_A8R8G8B8 = 32,
    TEGRA_VIC_T_Y8___U8V8_N420 = 67,
    TEGRA_VIC_BLK_PITCH = 0,
    TEGRA_VIC_CACHE_64Bx4 = 2,
    TEGRA_VIC_FILTER_1TAP = 0,
    TEGRA_VIC_FILTER_2TAP = 1,
    TEGRA_VIC_ALPHA_OPAQUE = 0,
    TEGRA_VIC_ALPHA_SOURCE_STREAM = 3,
};

// Stores `value` in bits [bit, bit + width) of the little-endian bit string at cfg + offset (a field never straddles
// more than two 64-bit words).
static inline void tegra_vic_put(uint8_t *cfg, uint32_t offset, uint32_t bit, uint32_t width, uint64_t value)
{
    for (uint32_t i = 0; i < width; i++) {
        const uint32_t b = bit + i;
        uint8_t *p = cfg + offset + b / 8;
        if ((value >> i) & 1)
            *p = (uint8_t)(*p | (1u << (b % 8)));
        else
            *p = (uint8_t)(*p & ~(1u << (b % 8)));
    }
}
static inline uint64_t tegra_vic_get(const uint8_t *cfg, uint32_t offset, uint32_t bit, uint32_t width)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < width; i++) {
        const uint32_t b = bit + i;
        v |= (uint64_t)((cfg[offset + b / 8] >> (b % 8)) & 1) << i;
    }
    return v;
}

// Field positions (bit offset in its structure, width), from libdrm vic40.h.
#define TEGRA_VIC_F(name, bit, width) enum { name##_BIT = (bit), name##_WIDTH = (width) };
TEGRA_VIC_F(PIPE_DOWNSAMPLE_HORIZ, 0, 11)
TEGRA_VIC_F(PIPE_DOWNSAMPLE_VERT, 16, 11)
TEGRA_VIC_F(OUT_ALPHA_FILL_MODE, 0, 3)
TEGRA_VIC_F(OUT_ALPHA_FILL_SLOT, 3, 3)
TEGRA_VIC_F(OUT_BACKGROUND_ALPHA, 6, 10)
TEGRA_VIC_F(OUT_BACKGROUND_R, 16, 10)
TEGRA_VIC_F(OUT_BACKGROUND_G, 26, 10)
TEGRA_VIC_F(OUT_BACKGROUND_B, 36, 10)
TEGRA_VIC_F(OUT_FLIP_X, 48, 1)
TEGRA_VIC_F(OUT_FLIP_Y, 49, 1)
TEGRA_VIC_F(OUT_TRANSPOSE, 50, 1)
TEGRA_VIC_F(OUT_TARGET_LEFT, 64, 14)
TEGRA_VIC_F(OUT_TARGET_RIGHT, 80, 14)
TEGRA_VIC_F(OUT_TARGET_TOP, 96, 14)
TEGRA_VIC_F(OUT_TARGET_BOTTOM, 112, 14)
TEGRA_VIC_F(SURF_FORMAT, 0, 7)
TEGRA_VIC_F(SURF_CHROMA_LOC_HORIZ, 7, 2)
TEGRA_VIC_F(SURF_CHROMA_LOC_VERT, 9, 2)
TEGRA_VIC_F(SURF_BLK_KIND, 11, 4)
TEGRA_VIC_F(SURF_BLK_HEIGHT, 15, 4)
TEGRA_VIC_F(SURF_CACHE_WIDTH, 19, 3)  // slot surfaces only
TEGRA_VIC_F(SURF_WIDTH, 32, 14)
TEGRA_VIC_F(SURF_HEIGHT, 46, 14)
TEGRA_VIC_F(SURF_LUMA_WIDTH, 64, 14)
TEGRA_VIC_F(SURF_LUMA_HEIGHT, 78, 14)
TEGRA_VIC_F(SURF_CHROMA_WIDTH, 96, 14)
TEGRA_VIC_F(SURF_CHROMA_HEIGHT, 110, 14)
TEGRA_VIC_F(SLOT_ENABLE, 0, 1)
TEGRA_VIC_F(SLOT_CURRENT_FIELD, 8, 1)
TEGRA_VIC_F(SLOT_FRAME_FORMAT, 16, 4)
TEGRA_VIC_F(SLOT_FILTER_Y, 20, 2)
TEGRA_VIC_F(SLOT_FILTER_X, 22, 2)
TEGRA_VIC_F(SLOT_SOFT_CLAMP_LOW, 128, 10)
TEGRA_VIC_F(SLOT_SOFT_CLAMP_HIGH, 138, 10)
TEGRA_VIC_F(SLOT_PLANAR_ALPHA, 160, 10)
TEGRA_VIC_F(SLOT_CONSTANT_ALPHA, 170, 1)
TEGRA_VIC_F(SLOT_SOURCE_LEFT, 256, 30)
TEGRA_VIC_F(SLOT_SOURCE_RIGHT, 288, 30)
TEGRA_VIC_F(SLOT_SOURCE_TOP, 320, 30)
TEGRA_VIC_F(SLOT_SOURCE_BOTTOM, 352, 30)
TEGRA_VIC_F(SLOT_DEST_LEFT, 384, 14)
TEGRA_VIC_F(SLOT_DEST_RIGHT, 400, 14)
TEGRA_VIC_F(SLOT_DEST_TOP, 416, 14)
TEGRA_VIC_F(SLOT_DEST_BOTTOM, 432, 14)
TEGRA_VIC_F(MATRIX_C00, 0, 20)
TEGRA_VIC_F(MATRIX_C10, 20, 20)
TEGRA_VIC_F(MATRIX_C20, 40, 20)
TEGRA_VIC_F(MATRIX_SHIFT, 60, 4)
TEGRA_VIC_F(MATRIX_C01, 64, 20)
TEGRA_VIC_F(MATRIX_C11, 84, 20)
TEGRA_VIC_F(MATRIX_C21, 104, 20)
TEGRA_VIC_F(MATRIX_ENABLE, 127, 1)
TEGRA_VIC_F(MATRIX_C02, 128, 20)
TEGRA_VIC_F(MATRIX_C12, 148, 20)
TEGRA_VIC_F(MATRIX_C22, 168, 20)
TEGRA_VIC_F(MATRIX_C03, 192, 20)
TEGRA_VIC_F(MATRIX_C13, 212, 20)
TEGRA_VIC_F(MATRIX_C23, 232, 20)
#undef TEGRA_VIC_F
#define TEGRA_VIC_SET(cfg, base, name, value) tegra_vic_put((cfg), (base), name##_BIT, name##_WIDTH, (uint64_t)(value))
#define TEGRA_VIC_GET(cfg, base, name)        tegra_vic_get((cfg), (base), name##_BIT, name##_WIDTH)

// --- Colour matrices ----------------------------------------------------------------------------------------------
// A 3x4 matrix in the engine's encoding: out[r] = (((c[r][0] * in0 + c[r][1] * in1 + c[r][2] * in2) >> shift)
// + c[r][3]) >> 8, clamped to 0..1023, on 10-bit pipeline values (in10 = in8 << 2, out8 = out10 >> 2). So a gain g
// is round(g * 2^(shift + 8)) and an offset of o 10-bit steps is round(o * 256). Coefficients are 20-bit signed.
typedef struct {
    int32_t c[3][4];
    uint32_t shift;
} TegraVICMatrix;

#define TEGRA_VIC_MATRIX_SHIFT 8u  // 1.0 = 65536: every gain below 8 fits the 20 bits

static inline void tegra_vic_matrix_put(uint8_t *cfg, uint32_t base, const TegraVICMatrix *m)
{
    const uint32_t mask = (1u << 20) - 1;
    TEGRA_VIC_SET(cfg, base, MATRIX_C00, (uint32_t)m->c[0][0] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C01, (uint32_t)m->c[0][1] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C02, (uint32_t)m->c[0][2] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C03, (uint32_t)m->c[0][3] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C10, (uint32_t)m->c[1][0] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C11, (uint32_t)m->c[1][1] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C12, (uint32_t)m->c[1][2] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C13, (uint32_t)m->c[1][3] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C20, (uint32_t)m->c[2][0] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C21, (uint32_t)m->c[2][1] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C22, (uint32_t)m->c[2][2] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_C23, (uint32_t)m->c[2][3] & mask);
    TEGRA_VIC_SET(cfg, base, MATRIX_SHIFT, m->shift);
    TEGRA_VIC_SET(cfg, base, MATRIX_ENABLE, 1);
}

// The engine's arithmetic for one pixel (the law above), for the self-test's expectations and the host test.
static inline void tegra_vic_matrix_apply(const TegraVICMatrix *m, const uint32_t in10[3], uint32_t out10[3])
{
    for (int r = 0; r < 3; r++) {
        int64_t acc = (int64_t)m->c[r][0] * in10[0] + (int64_t)m->c[r][1] * in10[1] + (int64_t)m->c[r][2] * in10[2];
        acc = (acc >> m->shift) + m->c[r][3];
        acc >>= 8;
        out10[r] = acc < 0 ? 0 : acc > 1023 ? 1023 : (uint32_t)acc;
    }
}

// Fixed-point rounding of a gain or offset given in millionths (kernel code has no floating point).
static inline int32_t tegra_vic_fix(int64_t micros, int64_t one)
{
    const int64_t n = micros * one;
    return (int32_t)(n >= 0 ? (n + 500000) / 1000000 : -((-n + 500000) / 1000000));
}

enum { TEGRA_VIC_YCC_601 = 0, TEGRA_VIC_YCC_709 = 1 };

// Luma weights (Kr, Kb) in millionths: BT.601 0.299/0.114, BT.709 0.2126/0.0722.
static inline void tegra_vic_kr_kb(uint32_t standard, int64_t *kr, int64_t *kb)
{
    *kr = standard == TEGRA_VIC_YCC_709 ? 212600 : 299000;
    *kb = standard == TEGRA_VIC_YCC_709 ? 72200 : 114000;
}

// Which pipeline channel carries Cb and which Cr for the 4:2:0 formats (ch0 is always Y). The self-test (V4) proves
// it on the unit; switch-frame-tap measured Cr in channel 1 and Cb in channel 2 for T_Y8___U8V8_N420 with NV12 data.
typedef struct {
    uint32_t cbChannel, crChannel;
} TegraVICChroma;

// RGB (pipeline channels R, G, B) -> Y'CbCr, 8-bit video range (Y 16..235, C 16..240) or full range (Y 0..255, C 128
// +- 127.5), rounded to nearest at the final >> 2 (offset + 2 10-bit steps).
static inline void tegra_vic_matrix_rgb_to_ycc(uint32_t standard, int full, const TegraVICChroma *ch, TegraVICMatrix *m)
{
    int64_t kr, kb;
    tegra_vic_kr_kb(standard, &kr, &kb);
    const int64_t kg = 1000000 - kr - kb;
    const int64_t ys = full ? 1000000 : 1000000 * 219 / 255, cs = full ? 1000000 : 1000000 * 224 / 255;
    const int64_t one = (int64_t)1 << (TEGRA_VIC_MATRIX_SHIFT + 8);
    // Rows in micro-units: Y = kr R + kg G + kb B; Cb = (B - Y) / (2 (1 - kb)); Cr = (R - Y) / (2 (1 - kr)).
    const int64_t y[3] = {kr * ys / 1000000, kg * ys / 1000000, kb * ys / 1000000};
    const int64_t cbDen = 2 * (1000000 - kb), crDen = 2 * (1000000 - kr);
    const int64_t cb[3] = {-kr * cs / cbDen, -kg * cs / cbDen, (1000000 - kb) * cs / cbDen};
    const int64_t cr[3] = {(1000000 - kr) * cs / crDen, -kg * cs / crDen, -kb * cs / crDen};
    memset(m, 0, sizeof(*m));
    m->shift = TEGRA_VIC_MATRIX_SHIFT;
    for (int i = 0; i < 3; i++) {
        m->c[0][i] = tegra_vic_fix(y[i], one);
        m->c[ch->cbChannel][i] = tegra_vic_fix(cb[i], one);
        m->c[ch->crChannel][i] = tegra_vic_fix(cr[i], one);
    }
    // Offsets in 10-bit steps (x256), +2 steps so out8 = out10 >> 2 rounds: Y 64 (video) or 0; C 512.
    m->c[0][3] = (full ? 0 : 64 * 256) + 512;
    m->c[ch->cbChannel][3] = 512 * 256 + 512;
    m->c[ch->crChannel][3] = 512 * 256 + 512;
}

// Y'CbCr -> RGB, the inverse: R = Y + 2(1-kr) Cr; B = Y + 2(1-kb) Cb; G = Y - (2kb(1-kb) Cb + 2kr(1-kr) Cr) / kg.
static inline void tegra_vic_matrix_ycc_to_rgb(uint32_t standard, int full, const TegraVICChroma *ch, TegraVICMatrix *m)
{
    int64_t kr, kb;
    tegra_vic_kr_kb(standard, &kr, &kb);
    const int64_t kg = 1000000 - kr - kb;
    // Gains of the 8-bit code values: luma 255/219 (video) or 1; chroma 255/224 (video) or 1.
    const int64_t gy = full ? 1000000 : 1000000 * 255 / 219, gc = full ? 1000000 : 1000000 * 255 / 224;
    const int64_t rCr = 2 * (1000000 - kr) * gc / 1000000, bCb = 2 * (1000000 - kb) * gc / 1000000;
    const int64_t gCb = 2 * kb * (1000000 - kb) / kg * gc / 1000000, gCr = 2 * kr * (1000000 - kr) / kg * gc / 1000000;
    const int64_t one = (int64_t)1 << (TEGRA_VIC_MATRIX_SHIFT + 8);
    memset(m, 0, sizeof(*m));
    m->shift = TEGRA_VIC_MATRIX_SHIFT;
    const uint32_t y = 0, cb = ch->cbChannel, cr = ch->crChannel;
    m->c[0][y] = m->c[1][y] = m->c[2][y] = tegra_vic_fix(gy, one);
    m->c[0][cr] = tegra_vic_fix(rCr, one);
    m->c[1][cb] = tegra_vic_fix(-gCb, one);
    m->c[1][cr] = tegra_vic_fix(-gCr, one);
    m->c[2][cb] = tegra_vic_fix(bCb, one);
    // Offsets: -(gain x black level) in 10-bit steps (x256): luma 64 (video), chroma 512; +2 steps to round.
    const int64_t y0 = full ? 0 : 64;
    m->c[0][3] = tegra_vic_fix(-(gy * y0 + rCr * 512), 256) + 512;
    m->c[1][3] = tegra_vic_fix(-(gy * y0 - gCb * 512 - gCr * 512), 256) + 512;
    m->c[2][3] = tegra_vic_fix(-(gy * y0 + bCb * 512), 256) + 512;
}

// --- Formats ------------------------------------------------------------------------------------------------------
#define TEGRA_VIC_FOURCC(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
enum {
    TEGRA_VIC_KIND_RGB = 0,
    TEGRA_VIC_KIND_YCC_VIDEO = 1,
    TEGRA_VIC_KIND_YCC_FULL = 2,
};
typedef struct {
    uint32_t fourcc;
    uint8_t vic, planes, kind, alpha;
    uint8_t bpe[2];      // bytes per element of each plane (a CbCr pair is one element)
    uint8_t subsampled;  // 4:2:0 chroma
} TegraVICFormat;

static inline const TegraVICFormat *tegra_vic_format(uint32_t fourcc)
{
    // 'BGRA' is bytes B, G, R, A: the 32-bit word 0xAARRGGBB, NVIDIA's A8R8G8B8. 'RGBA' is A8B8G8R8. '420v'/'420f'
    // are NV12 (Cb then Cr in the second plane), video or full range.
    static const TegraVICFormat formats[] = {
        {TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'), TEGRA_VIC_T_A8R8G8B8, 1, TEGRA_VIC_KIND_RGB, 1, {4, 0}, 0},
        {TEGRA_VIC_FOURCC('R', 'G', 'B', 'A'), TEGRA_VIC_T_A8B8G8R8, 1, TEGRA_VIC_KIND_RGB, 1, {4, 0}, 0},
        {TEGRA_VIC_FOURCC('4', '2', '0', 'v'), TEGRA_VIC_T_Y8___U8V8_N420, 2, TEGRA_VIC_KIND_YCC_VIDEO, 0, {1, 2}, 1},
        {TEGRA_VIC_FOURCC('4', '2', '0', 'f'), TEGRA_VIC_T_Y8___U8V8_N420, 2, TEGRA_VIC_KIND_YCC_FULL, 0, {1, 2}, 1},
    };
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
        if (formats[i].fourcc == fourcc) return &formats[i];
    return 0;
}

// --- Jobs ---------------------------------------------------------------------------------------------------------
// One surface as the engine sees it: plane addresses in VIC's (SMMU) address space and how far each may extend.
typedef struct {
    const TegraVICFormat *format;
    uint32_t width, height;  // pixels
    uint32_t pitch[2];       // bytes per row of each plane
    uint32_t rows[2];        // rows each plane holds
    uint64_t base[2];        // plane addresses
    uint64_t span[2];        // bytes mapped from base[p] (the checker keeps every access inside)
    uint32_t standard;       // TEGRA_VIC_YCC_* for the 4:2:0 formats
    uint32_t chromaSite;     // IOSurfaceAccelerator siting code 0..6 (0: unspecified)
} TegraVICImage;

// IOSurfaceAccelerator's transform bits (the request's Transform option, flags bits 0..2).
enum { TEGRA_VIC_FLIP_H = 1, TEGRA_VIC_FLIP_V = 2, TEGRA_VIC_ROT90 = 4 };

typedef struct {
    uint64_t srcX, srcY, srcW, srcH;  // source rectangle, 16.16 fixed
    uint32_t dstX, dstY, dstW, dstH;  // destination rectangle, pixels
    uint32_t transform;               // TEGRA_VIC_FLIP_* | TEGRA_VIC_ROT90
    uint32_t nearest;                 // 1-tap filter instead of 2-tap
    uint32_t fill;                    // no source: write the colour below over the rectangle
    uint32_t fill10[3];               // the fill's R, G, B (10-bit; converted for a Y'CbCr destination)
    uint32_t fillAlpha10;             // and its alpha (10-bit)
} TegraVICOp;

// What the boot self-test proved about this engine (TegraVICSelfTest.cpp).
typedef struct {
    TegraVICChroma in, out;      // channel of Cb/Cr when reading and writing 4:2:0
    uint8_t vicFlips[8][3];      // IOSurfaceAccelerator transform -> OutputFlipX, OutputFlipY, OutputTranspose
    uint32_t preTransposeRects;  // 1: target/destination rectangles and surface size are given before the transpose
    uint32_t rotations;          // bit t: transform t proven
    uint32_t bilinear;           // 2-tap filtering proven (else every job is 1-tap)
    uint32_t pitchAlign;         // smallest pitch alignment proven (64 or 256 bytes)
    uint32_t fillThroughMatrix;  // 1: the background colour goes through the output matrix (else it is converted here)
} TegraVICTraits;

#define TEGRA_VIC_MAX_METHODS 12
typedef struct {
    uint8_t config[TEGRA_VIC_CONFIG_BYTES];
    uint32_t method[TEGRA_VIC_MAX_METHODS], value[TEGRA_VIC_MAX_METHODS];
    uint32_t count;
    uint64_t pixels;  // read plus written: the engine's wait bound scales with it
} TegraVICJob;

// Limits: VIC's rectangles are 14-bit; IOSurfaceAcceleratorCapabilitiesDict publishes these.
enum {
    TEGRA_VIC_MAX_DIM = 8192,
    TEGRA_VIC_MIN_DIM = 2,
    TEGRA_VIC_MAX_UPSCALE = 16,    // destination / source, per axis
    TEGRA_VIC_MAX_DOWNSCALE = 16,  // source / destination, per axis
};

static inline uint32_t tegra_vic_chroma_horiz(uint32_t site)
{
    // 1 Left, 2 Center, 3 TopLeft, 4 Top, 5 BottomLeft, 6 Bottom (IOSurfaceAccelerator); 0 unspecified = Left.
    return (site == 2 || site == 4 || site == 6) ? 1 : 0;
}
static inline uint32_t tegra_vic_chroma_vert(uint32_t site)
{
    return (site == 3 || site == 4) ? 0 : (site == 5 || site == 6) ? 2 : 1;
}

// Validates an image: geometry the engine can address and every plane's extent inside its mapping.
static inline const char *tegra_vic_check_image(const TegraVICImage *im, const TegraVICTraits *t, const char *which)
{
    (void)which;
    const TegraVICFormat *f = im->format;
    if (!f) return "pixel format the scaler does not take";
    if (im->width < TEGRA_VIC_MIN_DIM || im->height < TEGRA_VIC_MIN_DIM || im->width > TEGRA_VIC_MAX_DIM ||
        im->height > TEGRA_VIC_MAX_DIM)
        return "surface size outside 2..8192";
    if (f->subsampled && ((im->width | im->height) & 1)) return "4:2:0 surface with an odd size";
    for (uint32_t p = 0; p < f->planes; p++) {
        const uint32_t w = f->subsampled && p ? im->width / 2 : im->width;
        const uint32_t h = f->subsampled && p ? im->height / 2 : im->height;
        const uint32_t align = t && t->pitchAlign ? t->pitchAlign : 256;
        if (im->base[p] & 0xff) return "plane not 256-byte aligned";
        if (im->pitch[p] % align || im->pitch[p] % f->bpe[p]) return "row pitch the engine cannot address";
        if ((uint64_t)w * f->bpe[p] > im->pitch[p]) return "row pitch shorter than a row";
        if (im->pitch[p] / f->bpe[p] > 16384) return "row pitch over 16384 elements";
        if (im->rows[p] < h) return "plane shorter than the surface";
        // The engine reads or writes (LumaHeight + 1) rows of (LumaWidth + 1) elements: h rows of pitch bytes.
        if ((uint64_t)im->pitch[p] * h > im->span[p]) return "plane extends past its mapping";
    }
    return 0;
}

// Builds the configuration and methods for one job. `configAddr`, `filterAddr` are where the configuration and the
// filter structure sit in VIC's address space. Returns 0, or why the job is refused (nothing is written then).
static inline const char *tegra_vic_build(const TegraVICImage *src, const TegraVICImage *dst, const TegraVICOp *op,
                                          const TegraVICTraits *t, uint64_t configAddr, uint64_t filterAddr,
                                          TegraVICJob *job)
{
    const char *why;
    if ((why = tegra_vic_check_image(dst, t, "destination"))) return why;
    if (!op->fill && (why = tegra_vic_check_image(src, t, "source"))) return why;
    if (op->transform > 7) return "transform outside the 8 flips and rotations";
    if (!(t->rotations & (1u << op->transform))) return "transform this engine has not proven";
    if (!op->dstW || !op->dstH || op->dstX + (uint64_t)op->dstW > dst->width ||
        op->dstY + (uint64_t)op->dstH > dst->height)
        return "destination rectangle outside the destination";
    if ((configAddr | filterAddr) & 0xff) return "configuration not 256-byte aligned";
    const int transpose = t->vicFlips[op->transform][2];
    if (!op->fill) {
        if (op->srcW < (1u << 16) || op->srcH < (1u << 16)) return "source rectangle under one pixel";
        if (op->srcX + op->srcW > ((uint64_t)src->width << 16) || op->srcY + op->srcH > ((uint64_t)src->height << 16))
            return "source rectangle outside the source";
        // Scale per output axis: a transposed job's destination width comes from the source height.
        const uint64_t sw = transpose ? op->srcH : op->srcW, sh = transpose ? op->srcW : op->srcH;
        if (((uint64_t)op->dstW << 16) > sw * TEGRA_VIC_MAX_UPSCALE ||
            ((uint64_t)op->dstH << 16) > sh * TEGRA_VIC_MAX_UPSCALE)
            return "upscale past 16x";
        if (sw > ((uint64_t)op->dstW << 16) * TEGRA_VIC_MAX_DOWNSCALE ||
            sh > ((uint64_t)op->dstH << 16) * TEGRA_VIC_MAX_DOWNSCALE)
            return "downscale past 16x";
    }
    if (dst->format->subsampled && ((op->dstX | op->dstY | op->dstW | op->dstH) & 1))
        return "4:2:0 destination rectangle on odd coordinates";

    memset(job, 0, sizeof(*job));
    uint8_t *c = job->config;
    TEGRA_VIC_SET(c, TEGRA_VIC_PIPE, PIPE_DOWNSAMPLE_HORIZ, 4);  // 1.0, U9.2
    TEGRA_VIC_SET(c, TEGRA_VIC_PIPE, PIPE_DOWNSAMPLE_VERT, 4);

    // Output. With the rectangles given before the transpose (a trait the self-test proves), the target and the
    // surface size are swapped for a transposed job; the pitch is always the real one.
    const uint32_t swap = transpose && t->preTransposeRects;
    const uint32_t ow = swap ? dst->height : dst->width, oh = swap ? dst->width : dst->height;
    const uint32_t tx = swap ? op->dstY : op->dstX, ty = swap ? op->dstX : op->dstY;
    const uint32_t tw = swap ? op->dstH : op->dstW, th = swap ? op->dstW : op->dstH;
    const int alpha = !op->fill && src->format->alpha && dst->format->alpha;
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_ALPHA_FILL_MODE,
                  alpha ? TEGRA_VIC_ALPHA_SOURCE_STREAM : TEGRA_VIC_ALPHA_OPAQUE);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_ALPHA, op->fill ? op->fillAlpha10 & 1023 : 1023);
    if (op->fill) {
        // A Y'CbCr destination: the background goes through the output matrix if the engine does that (a trait the
        // self-test proves), else it is converted here, through the same arithmetic, into the output channels.
        uint32_t bg[3] = {op->fill10[0] & 1023, op->fill10[1] & 1023, op->fill10[2] & 1023};
        if (dst->format->kind != TEGRA_VIC_KIND_RGB) {
            TegraVICMatrix m;
            tegra_vic_matrix_rgb_to_ycc(dst->standard, dst->format->kind == TEGRA_VIC_KIND_YCC_FULL, &t->out, &m);
            if (t->fillThroughMatrix) {
                tegra_vic_matrix_put(c, TEGRA_VIC_OUT_MATRIX, &m);
            } else {
                const uint32_t rgb[3] = {bg[0], bg[1], bg[2]};
                tegra_vic_matrix_apply(&m, rgb, bg);
            }
        }
        TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_R, bg[0]);
        TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_G, bg[1]);
        TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_BACKGROUND_B, bg[2]);
    }
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_X, t->vicFlips[op->transform][0]);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_FLIP_Y, t->vicFlips[op->transform][1]);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TRANSPOSE, transpose);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_LEFT, tx);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_RIGHT, tx + tw - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_TOP, ty);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT, OUT_TARGET_BOTTOM, ty + th - 1);

    const TegraVICFormat *df = dst->format;
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_FORMAT, df->vic);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_HORIZ,
                  df->subsampled ? tegra_vic_chroma_horiz(dst->chromaSite) : 0);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_LOC_VERT,
                  df->subsampled ? tegra_vic_chroma_vert(dst->chromaSite) : 0);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_BLK_KIND, TEGRA_VIC_BLK_PITCH);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_WIDTH, ow - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_HEIGHT, oh - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_WIDTH, dst->pitch[0] / df->bpe[0] - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_LUMA_HEIGHT, dst->height - 1);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_WIDTH,
                  df->planes > 1 ? dst->pitch[1] / df->bpe[1] - 1 : 16383);
    TEGRA_VIC_SET(c, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_HEIGHT, df->planes > 1 ? dst->height / 2 - 1 : 16383);

    const int dstYcc = df->kind != TEGRA_VIC_KIND_RGB;
    if (!op->fill) {
        const TegraVICFormat *sf = src->format;
        const int srcYcc = sf->kind != TEGRA_VIC_KIND_RGB;
        const uint32_t slot = TEGRA_VIC_SLOTS;  // slot 0
        uint8_t *s = c;
        TEGRA_VIC_SET(s, slot, SLOT_ENABLE, 1);
        TEGRA_VIC_SET(s, slot, SLOT_CURRENT_FIELD, 1);
        const uint32_t filter = op->nearest || !t->bilinear ? TEGRA_VIC_FILTER_1TAP : TEGRA_VIC_FILTER_2TAP;
        TEGRA_VIC_SET(s, slot, SLOT_FILTER_X, filter);
        TEGRA_VIC_SET(s, slot, SLOT_FILTER_Y, filter);
        TEGRA_VIC_SET(s, slot, SLOT_SOFT_CLAMP_LOW, 0);
        TEGRA_VIC_SET(s, slot, SLOT_SOFT_CLAMP_HIGH, 1023);
        TEGRA_VIC_SET(s, slot, SLOT_PLANAR_ALPHA, 1023);
        TEGRA_VIC_SET(s, slot, SLOT_CONSTANT_ALPHA, 1);
        TEGRA_VIC_SET(s, slot, SLOT_SOURCE_LEFT, op->srcX);
        TEGRA_VIC_SET(s, slot, SLOT_SOURCE_RIGHT, op->srcX + op->srcW - (1u << 16));
        TEGRA_VIC_SET(s, slot, SLOT_SOURCE_TOP, op->srcY);
        TEGRA_VIC_SET(s, slot, SLOT_SOURCE_BOTTOM, op->srcY + op->srcH - (1u << 16));
        TEGRA_VIC_SET(s, slot, SLOT_DEST_LEFT, tx);
        TEGRA_VIC_SET(s, slot, SLOT_DEST_RIGHT, tx + tw - 1);
        TEGRA_VIC_SET(s, slot, SLOT_DEST_TOP, ty);
        TEGRA_VIC_SET(s, slot, SLOT_DEST_BOTTOM, ty + th - 1);
        const uint32_t surf = slot + TEGRA_VIC_SLOT_SURFACE;
        TEGRA_VIC_SET(s, surf, SURF_FORMAT, sf->vic);
        TEGRA_VIC_SET(s, surf, SURF_CHROMA_LOC_HORIZ, sf->subsampled ? tegra_vic_chroma_horiz(src->chromaSite) : 0);
        TEGRA_VIC_SET(s, surf, SURF_CHROMA_LOC_VERT, sf->subsampled ? tegra_vic_chroma_vert(src->chromaSite) : 0);
        TEGRA_VIC_SET(s, surf, SURF_BLK_KIND, TEGRA_VIC_BLK_PITCH);
        TEGRA_VIC_SET(s, surf, SURF_CACHE_WIDTH, TEGRA_VIC_CACHE_64Bx4);
        TEGRA_VIC_SET(s, surf, SURF_WIDTH, src->width - 1);
        TEGRA_VIC_SET(s, surf, SURF_HEIGHT, src->height - 1);
        TEGRA_VIC_SET(s, surf, SURF_LUMA_WIDTH, src->pitch[0] / sf->bpe[0] - 1);
        TEGRA_VIC_SET(s, surf, SURF_LUMA_HEIGHT, src->height - 1);
        TEGRA_VIC_SET(s, surf, SURF_CHROMA_WIDTH, sf->planes > 1 ? src->pitch[1] / sf->bpe[1] - 1 : 16383);
        TEGRA_VIC_SET(s, surf, SURF_CHROMA_HEIGHT, sf->planes > 1 ? src->height / 2 - 1 : 16383);

        // Colour: 4:2:0 in -> RGB through the slot matrix, RGB -> 4:2:0 out through the output matrix. A 4:2:0 to
        // 4:2:0 job of the same range and standard passes the channels through untouched (the identity, measured).
        TegraVICMatrix m;
        const int sameYcc = srcYcc && dstYcc && sf->kind == df->kind && src->standard == dst->standard &&
                            t->in.cbChannel == t->out.cbChannel;
        if (srcYcc && !sameYcc) {
            tegra_vic_matrix_ycc_to_rgb(src->standard, sf->kind == TEGRA_VIC_KIND_YCC_FULL, &t->in, &m);
            tegra_vic_matrix_put(s, slot + TEGRA_VIC_SLOT_MATRIX, &m);
        }
        if (dstYcc && !sameYcc) {
            tegra_vic_matrix_rgb_to_ycc(dst->standard, df->kind == TEGRA_VIC_KIND_YCC_FULL, &t->out, &m);
            tegra_vic_matrix_put(c, TEGRA_VIC_OUT_MATRIX, &m);
        }
    }

    uint32_t n = 0;
#define TEGRA_VIC_METHOD(m, v)         \
    do {                               \
        job->method[n] = (m);          \
        job->value[n] = (uint32_t)(v); \
        n++;                           \
    } while (0)
    TEGRA_VIC_METHOD(TEGRA_VIC_M_SET_APPLICATION_ID, 1);
    TEGRA_VIC_METHOD(TEGRA_VIC_M_SET_CONTROL_PARAMS, (TEGRA_VIC_CONFIG_BYTES / 16) << 16 |
                                                         TEGRA_VIC_CONTROL_GPTIMER_ON |
                                                         TEGRA_VIC_CONTROL_FALCON_CONTROL);
    TEGRA_VIC_METHOD(TEGRA_VIC_M_SET_CONFIG_STRUCT_OFFSET, configAddr >> 8);
    TEGRA_VIC_METHOD(TEGRA_VIC_M_SET_FILTER_STRUCT_OFFSET, filterAddr >> 8);
    TEGRA_VIC_METHOD(TEGRA_VIC_M_OUTPUT_LUMA, dst->base[0] >> 8);
    if (df->planes > 1) TEGRA_VIC_METHOD(TEGRA_VIC_M_OUTPUT_CHROMA_U, dst->base[1] >> 8);
    if (!op->fill) {
        TEGRA_VIC_METHOD(TEGRA_VIC_M_SURFACE0_SLOT0_LUMA, src->base[0] >> 8);
        if (src->format->planes > 1) TEGRA_VIC_METHOD(TEGRA_VIC_M_SURFACE0_SLOT0_CHROMA_U, src->base[1] >> 8);
    }
    TEGRA_VIC_METHOD(TEGRA_VIC_M_EXECUTE, TEGRA_VIC_EXECUTE_AWAKEN);
#undef TEGRA_VIC_METHOD
    job->count = n;
    job->pixels = (uint64_t)op->dstW * op->dstH + (op->fill ? 0 : (op->srcW >> 16) * (op->srcH >> 16));
    return 0;
}

#ifdef __cplusplus
}
#endif
#endif
