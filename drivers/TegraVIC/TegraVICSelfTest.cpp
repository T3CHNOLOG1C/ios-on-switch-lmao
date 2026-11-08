// SPDX-License-Identifier: BSD-2-Clause-Patent
// VIC's boot self-test: V3 (SMMU translation) and V4 (what the engine does with the configurations TegraVICConfig.h
// generates, against the reference model in the same header). Small surfaces in the kernel arena (mapped 1:1), one job
// at a time; the expectations are computed here, from the same code the host test checks against the float colour
// conversion. The traits it proves (TegraVICTraits) are what the service later builds every job with:
//   transforms  each of the 8 OutputFlipX/FlipY/Transpose combinations is run on a 32x16 card whose every pixel names
//               its own coordinates; the map it produced (one of the 8 flips and rotations, or garbage) decides which
//               combination implements which IOSurfaceAccelerator transform, and whether the rectangles and,
//               apart, the output surface (size and memory geometry) of a transposed job are given before or after
//               the transpose (four conventions, TegraVICProbe.h; the plain transpose's four each log a diag line)
//   placement   a destination rectangle inside a larger surface under each proven transform: not a gate (a transform
//               whose rectangle lands elsewhere keeps whole-surface jobs only)
//   colour      flat patches BGRA -> 420f (BT.709, full range) through the output matrix, and 420f -> BGRA through the
//               slot matrix, exact to one 8-bit step against the engine's arithmetic (TegraVICConfig.h), in the chroma
//               channel order measured on T210B01 or, failing that, the other one
//   scaling     a 2:1 downscale of flat patches with the 2-tap filter (patch interiors exact to one step), else 1-tap
//   pitch       a source whose row pitch is 64- but not 256-byte aligned (as a 720-pixel BGRA row), else 256 is required
//   recording   the Screen Recording shape in small: BGRA turned a quarter each way (transforms 4 and 7) into 420f;
//               not a gate: off, the quarter turns are dropped
// Block-linear input (NVDEC's pictures, TegraVICPresent.h) is not measured: the 4:2:0 pass-through carries it. A
// 128x64 Tegra block-linear 420f picture copied through it matched to a step on all 21 boots that ran that check
// (11-05 and 11-06), and NVDEC's presents on those boots were never refused or failed.
// A failure is one "V3 FAIL" / "V4 FAIL" line with what was seen, and the service is not published.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <os/log.h>
#include <string.h>
#include "../TegraHost1x/TegraHost1x.h"
#include "TegraVIC.h"
#include "../../kits/SwitchTime/SwitchTime.h"
#include "TegraVICProbe.h"

namespace {
TegraVICBuffer gSrc, gDst, gPageA, gPageB;
uint32_t gJobs;
uint64_t gBusyUs;

constexpr uint32_t kPitch = 256;
TegraVICJob gLastJob;  // the last job run, for a failure's diagnostic

// One-boot diagnostic after a failed check: the last job's configuration as written (the parts colour and geometry
// depend on, as 32-bit words), its methods, and the falcon's state, one IOLog line each (a failure only).
void dumpLastJob(const char *what)
{
    const TegraVICJob &j = gLastJob;
    const uint32_t *w = reinterpret_cast<const uint32_t *>(j.config);
    char text[300];
    char falcon[200];
    tegra_vic_describe(falcon, sizeof(falcon));
    IOLog("TegraVIC: diag %s: %s\n", what, falcon);

    const struct {
        const char *name;
        uint32_t offset, bytes;
    } parts[] = {
        {"pipe", TEGRA_VIC_PIPE, 16},
        {"output", TEGRA_VIC_OUTPUT, 16},
        {"output surface", TEGRA_VIC_OUTPUT_SURFACE, 16},
        {"output matrix", TEGRA_VIC_OUT_MATRIX, 32},
        {"slot 0 config", TEGRA_VIC_SLOTS, 64},
        {"slot 0 surface", TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE, 16},
        {"slot 0 matrix", TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_MATRIX, 32},
    };

    for (const auto &part : parts) {
        int n = snprintf(text, sizeof(text), "TegraVIC: diag %s: %s @%#x:", what, part.name, part.offset);
        for (uint32_t i = 0; i < part.bytes / 4 && n > 0 && n < static_cast<int>(sizeof(text)); i++)
            n += snprintf(text + n, sizeof(text) - n, " %08x", w[part.offset / 4 + i]);
        IOLog("%s\n", text);
    }
    int n = snprintf(text, sizeof(text), "TegraVIC: diag %s: methods", what);
    for (uint32_t i = 0; i < j.count && n > 0 && n < static_cast<int>(sizeof(text)); i++)
        n += snprintf(text + n, sizeof(text) - n, " %03x=%08x", j.method[i], j.value[i]);
    IOLog("%s\n", text);
}

TegraVICImage bgra(const TegraVICBuffer &b, uint32_t w, uint32_t h, uint32_t pitch)
{
    TegraVICImage im = {};
    im.format = tegra_vic_format(TEGRA_VIC_FOURCC('B', 'G', 'R', 'A'));
    im.width = w;
    im.height = h;
    im.pitch[0] = pitch;
    im.rows[0] = h;
    im.base[0] = b.pa;
    im.span[0] = b.size;
    return im;
}

TegraVICImage nv12(const TegraVICBuffer &b, uint32_t w, uint32_t h)
{
    TegraVICImage im = {};
    im.format = tegra_vic_format(TEGRA_VIC_FOURCC('4', '2', '0', 'f'));
    im.width = w;
    im.height = h;
    im.pitch[0] = im.pitch[1] = kPitch;
    im.rows[0] = h;
    im.rows[1] = h / 2;
    im.base[0] = b.pa;
    im.span[0] = static_cast<uint64_t>(kPitch) * h;
    im.base[1] = b.pa + static_cast<uint64_t>(kPitch) * h;
    im.span[1] = b.size - im.span[0];
    im.standard = TEGRA_VIC_YCC_709;
    return im;
}

TegraVICOp copyOp(uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh, uint32_t transform)
{
    TegraVICOp op = {};
    op.srcW = static_cast<uint64_t>(sw) << 16;
    op.srcH = static_cast<uint64_t>(sh) << 16;
    op.dstW = dw;
    op.dstH = dh;
    op.transform = transform;
    op.nearest = 1;
    return op;
}

// Runs one job on kernel buffers (job lock held by the caller); the destination is clean before and invalidated after.
int run(const TegraVICImage *src, const TegraVICImage *dst, const TegraVICOp *op, const TegraVICTraits *t,
        const char **why)
{
    TegraVICJob &job = gLastJob;
    *why = tegra_vic_build(src, dst, op, t, tegra_vic_config_address(), tegra_vic_filter_address(), &job);
    if (*why)
        return -22;
    if (!op->fill)
        switch_dcache_clean(gSrc.va, gSrc.size);
    memset(gDst.va, 0xab, gDst.size);  // poison: what the engine does not write shows
    switch_dcache_clean_invalidate(gDst.va, gDst.size);
    uint64_t us = 0;
    const int ret = tegra_vic_run(&job, &us);
    switch_dcache_clean_invalidate(gDst.va, gDst.size);
    gJobs++;
    gBusyUs += us;
    if (ret)
        *why = "job did not complete";
    return ret;
}

// --- V3: the SMMU translates VIC's accesses ------------------------------------------------------------------------
bool smmuProof(char *line, size_t size)
{
    TegraVICTraits t = {};
    t.rotations = 1;
    t.pitchAlign = 256;
    const TegraVICImage dst = bgra(gPageA, 64, 4, kPitch);
    TegraVICOp op = {};
    op.fill = 1;
    op.dstW = 64;
    op.dstH = 4;
    op.fill10[0] = 0x55 << 2;  // R 0x55, G 0xaa, B 0x33 (8-bit values, x4 in the 10-bit pipeline)
    op.fill10[1] = 0xaa << 2;
    op.fill10[2] = 0x33 << 2;
    op.fillAlpha10 = 1023;
    static TegraVICJob job;
    const char *why =
        tegra_vic_build(nullptr, &dst, &op, &t, tegra_vic_config_address(), tegra_vic_filter_address(), &job);
    if (why) {
        snprintf(line, size, "V3 FAIL fill refused: %s", why);
        return false;
    }
    bzero(gPageA.va, 4096);
    bzero(gPageB.va, 4096);
    switch_dcache_clean_invalidate(gPageA.va, 4096);
    switch_dcache_clean_invalidate(gPageB.va, 4096);
    tegra_vic_identity_override(gPageA.pa, gPageB.pa);
    uint64_t us = 0;
    const int ret = tegra_vic_run(&job, &us);
    tegra_vic_identity_override(gPageA.pa, gPageA.pa);
    switch_dcache_clean_invalidate(gPageA.va, 4096);
    switch_dcache_clean_invalidate(gPageB.va, 4096);
    gJobs++;
    gBusyUs += us;
    const uint32_t want = 0xff55aa33u;  // A8R8G8B8: bytes 33 aa 55 ff
    uint32_t inA = 0;
    uint32_t inB = 0;
    for (uint32_t i = 0; i < 4 * kPitch / 4; i++) {
        inA += static_cast<uint32_t>(reinterpret_cast<const uint32_t *>(gPageA.va)[i] == want);
        inB += static_cast<uint32_t>(reinterpret_cast<const uint32_t *>(gPageB.va)[i] == want);
    }
    if (ret || inB != 64 * 4 || inA) {
        snprintf(line, size,
                 "V3 FAIL a fill at IOVA %#llx mapped to page %#llx: %s; %u of 256 pixels in that page, %u at the bus "
                 "address (%s); first words %#x %#x",
                 gPageA.pa, gPageB.pa, ret ? "job did not complete" : "landed elsewhere", inB, inA,
                 inA ? "translation is off" : "nowhere", reinterpret_cast<const uint32_t *>(gPageB.va)[0],
                 reinterpret_cast<const uint32_t *>(gPageA.va)[0]);
        return false;
    }
    snprintf(line, size,
             "V3 ok SMMU translates the vic swgroup: a 64x4 fill at IOVA %#llx landed in page %#llx, the bus "
             "address untouched, in %llu us",
             gPageA.pa, gPageB.pa, us);
    return true;
}

// --- V4 ----------------------------------------------------------------------------------------------------------
// Probe diag lines go to the log only (os_log), not the boot console.
int probeRun(void *, const TegraVICImage *src, const TegraVICImage *dst, const TegraVICOp *op, const TegraVICTraits *t,
             const char **why)
{
    return run(src, dst, op, t, why);
}

void probeNote(void *, const char *line)
{
    os_log(OS_LOG_DEFAULT, "%s", line);
}

TegraVICProbeEnv probeEnv()
{
    TegraVICProbeEnv e = {};
    e.run = probeRun;
    e.note = probeNote;
    e.srcVa = gSrc.va;
    e.dstVa = gDst.va;
    e.srcIova = gSrc.pa;
    e.srcBytes = gSrc.size;
    e.dstIova = gDst.pa;
    e.dstBytes = gDst.size;
    return e;
}

bool transforms(TegraVICTraits *t, char *line, size_t size)
{
    const TegraVICProbeEnv e = probeEnv();
    return tegra_vic_probe_transforms(&e, t, line, size) != 0;
}

// Flat-patch cards: 4 x 2 patches of 16 x 16 in 64 x 32 (BGRA) with these colours.
constexpr uint32_t kPatches[8] = {0xff000000, 0xffffffff, 0xffff0000, 0xff00ff00,
                                  0xff0000ff, 0xff808080, 0xffc04020, 0xff2090e0};

void drawPatches(uint32_t pitch, uint32_t width = 64)
{
    bzero(gSrc.va, gSrc.size);
    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < width; x++)
            reinterpret_cast<uint32_t *>(gSrc.va + static_cast<size_t>(y) * pitch)[x] = kPatches[(y / 16) * 4 + x / 16];
    }
}

inline uint32_t near(uint32_t a, uint32_t b)
{
    return static_cast<uint32_t>(a > b ? a - b <= 1 : b - a <= 1);
}

// The engine's result for an RGB pixel through the output matrix: Y, Cb, Cr (8-bit).
void expectYcc(const TegraVICMatrix *m, const TegraVICChroma *ch, uint32_t argb, uint32_t ycc[3])
{
    const uint32_t in10[3] = {((argb >> 16) & 0xff) << 2, ((argb >> 8) & 0xff) << 2, (argb & 0xff) << 2};
    uint32_t out10[3];
    tegra_vic_matrix_apply(m, in10, out10);
    ycc[0] = out10[0] >> 2;
    ycc[1] = out10[ch->cbChannel] >> 2;
    ycc[2] = out10[ch->crChannel] >> 2;
}

// Checks a 420f result in gDst (w x h, NV12 at kPitch) whose patch (px, py) of `patch` pixels should be `ycc`.
bool checkNv12(uint32_t w, uint32_t h, uint32_t patchW, uint32_t patchH, const uint32_t (*want)[3], uint32_t cols,
               uint32_t *bad)
{
    const uint8_t *yp = gDst.va;
    const uint8_t *uv = gDst.va + static_cast<uint64_t>(kPitch) * h;
    *bad = 0;
    for (uint32_t py = 0; py < h / patchH; py++) {
        for (uint32_t px = 0; px < w / patchW; px++) {
            const uint32_t *e = want[py * cols + px];
            for (uint32_t y = py * patchH + patchH / 4; y < (py + 1) * patchH - patchH / 4; y++) {
                for (uint32_t x = px * patchW + patchW / 4; x < (px + 1) * patchW - patchW / 4; x++) {
                    if (!near(yp[y * kPitch + x], e[0]))
                        (*bad)++;
                    const uint8_t *c = uv + static_cast<size_t>(y / 2) * kPitch + static_cast<size_t>(x / 2) * 2;
                    if (!near(c[0], e[1]) || !near(c[1], e[2]))
                        (*bad)++;
                }
            }
        }
    }
    return *bad == 0;
}

// 4:2:0 into the engine. The source is the 420f gSrc holds (64 x 32 patches, written by the engine itself), and every
// expectation is computed from the samples read back from it: what the engine reads, not what it was asked to write
// (r7u's V4 compared the patches with a source the flat-fill check had overwritten: 223 steps). 420f -> BGRA gates V4
// as before: within one step, on every patch interior, of the engine's arithmetic for the matrix the job programs
// (tegra_vic_matrix_apply; the host test holds that matrix within one step of the float BT.709 conversion), in one
// chroma order and one of two ways of widening an 8-bit sample to the 10-bit pipeline: a shift (x << 2, the RGB -> 420f
// law) or the shift with the top bits repeated (x << 2 | x >> 6). r7u-r7w's one pixel (the fill, Y 89 Cb 97 Cr 193
// +- 1, read in the other order as 0xff295bd6) is only the second (0x295bd5 from Cr 194; the shift gives 2 steps off at
// best). A wrong matrix, offset, range or chroma order is tens of steps off in both.
// The same-format pass-through (420f -> 420f, exact to a step) is a trait: refused per job when not proven.
inline uint32_t widen(uint32_t v8, uint32_t law)
{
    return law ? (v8 << 2) | (v8 >> 6) : v8 << 2;
}

bool yccSource(TegraVICTraits *t, char *line, size_t size, char *note, size_t noteSize)
{
    const TegraVICImage ysrc = nv12(gSrc, 64, 32);
    const TegraVICOp op = copyOp(64, 32, 64, 32, 0);
    const TegraVICChroma orders[2] = {t->out, {t->out.crChannel, t->out.cbChannel}};
    const uint8_t *yp = gSrc.va;
    const uint8_t *uv = gSrc.va + static_cast<uint64_t>(kPitch) * 32;
    // 420f -> BGRA through the slot matrix in both chroma orders. Patch centres kept for a failure's diagnostic: read
    // Y Cb Cr, wrote R G B, want R G B (shift), want R G B (repeated bits), BT.709 R G B.
    uint32_t worst[2][2] = {{256, 256}, {256, 256}};
    uint32_t centre[2][8][15] = {};
    const char *refused[2] = {nullptr, nullptr};
    for (uint32_t o = 0; o < 2; o++) {
        TegraVICTraits probe = *t;
        probe.in = orders[o];
        probe.yccIn = TEGRA_VIC_YCC_IN_RGB;
        const TegraVICImage rgb = bgra(gDst, 64, 32, kPitch);
        if (run(&ysrc, &rgb, &op, &probe, &refused[o]))
            continue;
        TegraVICMatrix toRgb;
        tegra_vic_matrix_ycc_to_rgb(TEGRA_VIC_YCC_709, true, &orders[o], &toRgb);
        worst[o][0] = worst[o][1] = 0;
        for (uint32_t i = 0; i < 8; i++) {
            for (uint32_t y = (i / 4) * 16 + 4; y < (i / 4) * 16 + 12; y++) {
                for (uint32_t x = (i % 4) * 16 + 4; x < (i % 4) * 16 + 12; x++) {
                    // NV12 memory: Cb then Cr (the RGB -> 420f check just proved the engine writes it so)
                    const uint8_t *c = uv + static_cast<size_t>(y / 2) * kPitch + static_cast<size_t>(x / 2) * 2;
                    const uint32_t ycc[3] = {yp[y * kPitch + x], c[0], c[1]};
                    const uint32_t px =
                        reinterpret_cast<const uint32_t *>(gDst.va + static_cast<size_t>(y) * kPitch)[x];
                    const uint32_t got[3] = {(px >> 16) & 0xff, (px >> 8) & 0xff, px & 0xff};
                    uint32_t want[2][3];
                    for (uint32_t law = 0; law < 2; law++) {
                        uint32_t pipe[3];
                        uint32_t out10[3];
                        pipe[0] = widen(ycc[0], law);
                        pipe[orders[o].cbChannel] = widen(ycc[1], law);
                        pipe[orders[o].crChannel] = widen(ycc[2], law);
                        tegra_vic_matrix_apply(&toRgb, pipe, out10);
                        for (int k = 0; k < 3; k++) {
                            want[law][k] = out10[k] >> 2;
                            const uint32_t d = got[k] > want[law][k] ? got[k] - want[law][k] : want[law][k] - got[k];
                            if (d > worst[o][law])
                                worst[o][law] = d;
                        }
                    }
                    if (x == (i % 4) * 16 + 8 && y == (i / 4) * 16 + 8) {
                        uint32_t *e = centre[o][i];
                        for (int k = 0; k < 3; k++)
                            e[k] = ycc[k], e[3 + k] = got[k], e[6 + k] = want[0][k], e[9 + k] = want[1][k];
                        tegra_vic_ideal_rgb709_full(ycc, e + 12);
                    }
                }
            }
        }
    }
    uint32_t best = 0;
    uint32_t law = 0;
    for (uint32_t o = 0; o < 2; o++) {
        for (uint32_t l = 0; l < 2; l++) {
            if (worst[o][l] < worst[best][law])
                best = o, law = l;
        }
    }
    const uint32_t *e = centre[best][6];  // the c0 40 20 patch
    if (worst[best][law] > 1) {
        // The one-boot diagnostic: what the engine read and wrote at each patch centre in both orders, then the
        // configuration of a job in the closer order (run again, so the dump is that job's).
        for (uint32_t o = 0; o < 2; o++) {
            for (uint32_t i = 0; i < 8; i++) {
                const uint32_t *d = centre[o][i];
                IOLog("TegraVIC: diag 420f -> BGRA order %u (Cb in channel %u) patch %u: read Y %u Cb %u Cr %u, wrote "
                      "%u %u %u, want %u %u %u (x << 2) or %u %u %u (x << 2 | x >> 6), BT.709 %u %u %u\n",
                      o, orders[o].cbChannel, i, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10],
                      d[11], d[12], d[13], d[14]);
            }
        }
        TegraVICTraits probe = *t;
        probe.in = orders[best];
        probe.yccIn = TEGRA_VIC_YCC_IN_RGB;
        const TegraVICImage rgb = bgra(gDst, 64, 32, kPitch);
        const char *why = nullptr;
        (void)run(&ysrc, &rgb, &op, &probe, &why);
        dumpLastJob("420f -> BGRA");
        if (refused[0] && refused[1]) {
            snprintf(line, size, "V4 FAIL 420f -> BGRA: %s", refused[0]);
        } else {
            snprintf(line, size,
                     "V4 FAIL 420f -> BGRA (BT.709 full) not the reference in either chroma order (worst %u/%u/%u/%u "
                     "steps; c0 40 20 patch read Y %u Cb %u Cr %u, wrote %u %u %u, want %u %u %u)",
                     worst[0][0], worst[0][1], worst[1][0], worst[1][1], e[0], e[1], e[2], e[3], e[4], e[5],
                     e[6 + 3 * law], e[7 + 3 * law], e[8 + 3 * law]);
        }
        return false;
    }
    t->in = orders[best];
    t->yccIn |= TEGRA_VIC_YCC_IN_RGB;
    // 420f -> 420f 1:1 (same range and standard): the pass-through, exact to a step. Only when the engine reads the
    // chroma in the order it writes it: otherwise such a job goes through both matrices (tegra_vic_build), proven above.
    uint32_t copyBad = 0;
    uint32_t firstBad = UINT32_MAX;
    const bool passThrough = t->in.cbChannel == t->out.cbChannel;
    if (passThrough) {
        TegraVICTraits probe = *t;
        probe.yccIn |= TEGRA_VIC_YCC_IN_COPY;
        const TegraVICImage dst = nv12(gDst, 64, 32);
        const char *why = nullptr;
        if (run(&ysrc, &dst, &op, &probe, &why)) {
            copyBad = UINT32_MAX;
        } else {
            for (uint32_t at = 0; at < kPitch * 48; at++) {
                if (at % kPitch >= 64)
                    continue;
                if (!near(gDst.va[at], gSrc.va[at])) {
                    if (firstBad == UINT32_MAX)
                        firstBad = at;
                    copyBad++;
                }
            }
        }
        if (!copyBad) {
            t->yccIn |= TEGRA_VIC_YCC_IN_COPY;
        } else if (copyBad != UINT32_MAX) {
            IOLog("TegraVIC: diag 420f -> 420f: %u of 3072 samples off, first at byte %#x (plane %s row %u column %u): "
                  "%u, source %u\n",
                  copyBad, firstBad, firstBad < kPitch * 32 ? "Y" : "CbCr", (firstBad % (kPitch * 32)) / kPitch,
                  firstBad % kPitch, gDst.va[firstBad], gSrc.va[firstBad]);
            dumpLastJob("420f -> 420f");
        }
    }
    const char *copy = "through the matrices";
    if (passThrough)
        copy = (t->yccIn & TEGRA_VIC_YCC_IN_COPY) ? "on" : "refused";
    snprintf(note, noteSize,
             "4:2:0 in: to RGB on (Cb in channel %u, 8-bit samples widened as %s; worst %u/%u/%u/%u steps by order and "
             "widening; c0 40 20 patch read Y %u Cb %u Cr %u -> %u %u %u, BT.709 %u %u %u), copy %s",
             t->in.cbChannel, law ? "x << 2 | x >> 6" : "x << 2", worst[0][0], worst[0][1], worst[1][0], worst[1][1],
             e[0], e[1], e[2], e[3], e[4], e[5], e[12], e[13], e[14], copy);
    return true;
}

bool colour(TegraVICTraits *t, char *line, size_t size, char *note, size_t noteSize)
{
    // RGB -> 420f through the output matrix; the measured channel order first.
    drawPatches(kPitch);
    const TegraVICImage src = bgra(gSrc, 64, 32, kPitch);
    const TegraVICImage dst = nv12(gDst, 64, 32);
    const TegraVICOp op = copyOp(64, 32, 64, 32, 0);
    const TegraVICChroma orders[2] = {{2, 1}, {1, 2}};
    uint32_t bad = 0;
    uint32_t outOrder = 2;
    for (uint32_t o = 0; o < 2 && outOrder == 2; o++) {
        t->out = orders[o];
        t->in = orders[o];
        const char *why = nullptr;
        if (run(&src, &dst, &op, t, &why)) {
            snprintf(line, size, "V4 FAIL BGRA -> 420f: %s", why);
            return false;
        }
        TegraVICMatrix m;
        tegra_vic_matrix_rgb_to_ycc(TEGRA_VIC_YCC_709, true, &t->out, &m);
        uint32_t want[8][3];
        for (uint32_t i = 0; i < 8; i++)
            expectYcc(&m, &t->out, kPatches[i], want[i]);
        if (checkNv12(64, 32, 16, 16, want, 4, &bad)) {
            outOrder = o;
        } else {
            const uint8_t *yp = gDst.va;
            const uint8_t *uv = gDst.va + static_cast<size_t>(kPitch) * 32;
            for (uint32_t i = 0; i < 8; i++) {
                const uint32_t x = (i % 4) * 16 + 8;
                const uint32_t y = (i / 4) * 16 + 8;
                const uint8_t *c = uv + static_cast<size_t>(y / 2) * kPitch + static_cast<size_t>(x / 2) * 2;
                IOLog(
                    "TegraVIC: diag BGRA -> 420f order %u (Cb in channel %u) patch %u (%06x): wrote Y %u Cb %u Cr %u, "
                    "want %u %u %u\n",
                    o, t->out.cbChannel, i, kPatches[i] & 0xffffff, yp[y * kPitch + x], c[0], c[1], want[i][0],
                    want[i][1], want[i][2]);
            }
            dumpLastJob("BGRA -> 420f");
        }
    }
    if (outOrder == 2) {
        const uint8_t *p = gDst.va;
        const uint8_t *uv = gDst.va + static_cast<size_t>(kPitch) * 32;
        snprintf(line, size,
                 "V4 FAIL BGRA -> 420f (BT.709 full) not the reference in either chroma order (%u samples off); "
                 "white patch Y %u, red patch Y %u Cb %u Cr %u",
                 bad, p[8 * kPitch + 24], p[8 * kPitch + 40], uv[4 * kPitch + 40], uv[4 * kPitch + 41]);
        return false;
    }
    t->out = orders[outOrder];
    t->in = t->out;

    // That 420f (the patches, as the engine wrote them) is the source of the 4:2:0-in checks.
    memcpy(gSrc.va, gDst.va, static_cast<uint64_t>(kPitch) * 48);
    if (!yccSource(t, line, size, note, noteSize))
        return false;

    // A fill of a 420f surface (Core Animation's border clears): the background colour through the output matrix, or
    // converted by the driver into the output channels.
    for (uint32_t through = 1;; through = 0) {
        t->fillThroughMatrix = through;
        TegraVICOp fill = {};
        fill.fill = 1;
        fill.dstW = 64;
        fill.dstH = 32;
        fill.fill10[0] = 0xc0 << 2;
        fill.fill10[1] = 0x40 << 2;
        fill.fill10[2] = 0x20 << 2;
        fill.fillAlpha10 = 1023;
        const char *why = nullptr;
        if (run(nullptr, &dst, &fill, t, &why)) {
            snprintf(line, size, "V4 FAIL 420f fill: %s", why);
            return false;
        }
        TegraVICMatrix m;
        tegra_vic_matrix_rgb_to_ycc(TEGRA_VIC_YCC_709, true, &t->out, &m);
        uint32_t want[8][3];
        for (uint32_t *patch : want)
            expectYcc(&m, &t->out, 0xffc04020u, patch);
        if (checkNv12(64, 32, 16, 16, want, 4, &bad))
            break;
        if (!through) {
            const uint8_t *p = gDst.va;
            const uint8_t *uv = gDst.va + static_cast<size_t>(kPitch) * 32;
            snprintf(line, size, "V4 FAIL 420f fill of RGB c0 40 20 is Y %u Cb %u Cr %u, want %u %u %u", p[0], uv[0],
                     uv[1], want[0][0], want[0][1], want[0][2]);
            return false;
        }
    }
    return true;
}

bool scaling(TegraVICTraits *t, char *line, size_t size)
{
    drawPatches(kPitch);
    const TegraVICImage src = bgra(gSrc, 64, 32, kPitch);
    const TegraVICImage dst = bgra(gDst, 32, 16, kPitch);
    for (uint32_t bilinear = 1;; bilinear = 0) {
        t->bilinear = bilinear;
        TegraVICOp op = copyOp(64, 32, 32, 16, 0);
        op.nearest = 0;
        const char *why = nullptr;
        if (run(&src, &dst, &op, t, &why)) {
            snprintf(line, size, "V4 FAIL 2:1 downscale: %s", why);
            return false;
        }
        uint32_t bad = 0;
        for (uint32_t i = 0; i < 8; i++) {
            for (uint32_t y = (i / 4) * 8 + 2; y < (i / 4) * 8 + 6; y++) {
                for (uint32_t x = (i % 4) * 8 + 2; x < (i % 4) * 8 + 6; x++) {
                    const uint32_t got =
                        reinterpret_cast<const uint32_t *>(gDst.va + static_cast<size_t>(y) * kPitch)[x];
                    for (uint32_t c = 0; c < 24; c += 8) {
                        if (!near((got >> c) & 0xff, (kPatches[i] >> c) & 0xff))
                            bad++;
                    }
                }
            }
        }
        if (!bad)
            return true;
        if (!bilinear) {
            snprintf(line, size, "V4 FAIL 2:1 downscale of flat patches is off (%u samples) with either filter", bad);
            return false;
        }
    }
}

bool pitch(TegraVICTraits *t, char *line, size_t size)
{
    // Row pitches the engine reads: 144 bytes (36 BGRA pixels: 16-byte aligned, as a CoreVideo buffer's 720-byte rows
    // may be), then 192 (64-byte aligned, as a 720-pixel BGRA row's 2880), each copied 1:1 into 256-byte rows. The
    // smallest that copies exactly is the rule; with neither, surfaces need 256-byte rows (others are refused).
    static const uint32_t kNarrow[2][2] = {{144, 16}, {192, 64}};
    t->pitchAlign = 256;
    for (uint32_t i = 0; i < 2 && t->pitchAlign == 256; i++) {
        const uint32_t rowBytes = kNarrow[i][0];
        const uint32_t w = rowBytes / 4;
        drawPatches(rowBytes, w);
        const TegraVICImage src = bgra(gSrc, w, 32, rowBytes);
        const TegraVICImage dst = bgra(gDst, w, 32, kPitch);
        const TegraVICOp op = copyOp(w, 32, w, 32, 0);
        TegraVICTraits probe = *t;
        probe.pitchAlign = kNarrow[i][1];
        const char *why = nullptr;
        if (run(&src, &dst, &op, &probe, &why)) {
            snprintf(line, size, "V4 FAIL %u-byte pitch copy: %s", rowBytes, why);
            return false;
        }
        uint32_t bad = 0;
        uint32_t firstX = 0;
        uint32_t firstY = 0;
        uint32_t got = 0;
        for (uint32_t y = 0; y < 32; y++) {
            for (uint32_t x = 0; x < w; x++) {
                const uint32_t px = reinterpret_cast<const uint32_t *>(gDst.va + static_cast<size_t>(y) * kPitch)[x];
                if (px != kPatches[(y / 16) * 4 + x / 16] && !bad++)
                    firstX = x, firstY = y, got = px;
            }
        }
        if (!bad) {
            t->pitchAlign = kNarrow[i][1];
        } else {
            char text[200];
            snprintf(
                text, sizeof(text),
                "TegraVIC: diag %u-byte source pitch: %u of %u pixels off in a 1:1 copy, first at %u,%u wrote %#x, "
                "want %#x",
                rowBytes, bad, w * 32, firstX, firstY, got, kPatches[(firstY / 16) * 4 + firstX / 16]);
            probeNote(nullptr, text);
        }
    }
    return true;
}

// Screen Recording in small, and a landscape video turned for a detached layer: a 64x32 BGRA patch card turned a quarter
// each way (transforms 4 and 7, the two the display-planes gate needs) into a 32x64 420f, the transposed 4:2:0 output
// geometry (luma and chroma planes described before the transpose). Not a gate: if either is off, transforms 4-7 are
// dropped (such jobs are refused and callers fall back), one line says why in `note`, and V4 goes on.
void recording(TegraVICTraits *t, char *note, size_t size)
{
    static const uint32_t kTurns[2] = {TEGRA_VIC_ROT90,
                                       static_cast<uint32_t>(TEGRA_VIC_ROT90) | TEGRA_VIC_FLIP_H | TEGRA_VIC_FLIP_V};
    for (const uint32_t turn : kTurns) {
        if (!(t->rotations & (1u << turn)))
            continue;  // unproven: such requests are refused anyway
        drawPatches(kPitch);
        const TegraVICImage src = bgra(gSrc, 64, 32, kPitch);
        const TegraVICImage dst = nv12(gDst, 32, 64);
        const TegraVICOp op = copyOp(64, 32, 32, 64, turn);
        const char *why = nullptr;
        uint32_t bad = 0;
        if (!run(&src, &dst, &op, t, &why)) {
            TegraVICMatrix m;
            tegra_vic_matrix_rgb_to_ycc(TEGRA_VIC_YCC_709, true, &t->out, &m);
            // Patch (px, py) of the 64 x 32 source (4 x 2 patches of 16) lands at output patch (u, v) of the 32 x 64
            // output (2 x 4): the transform of the patch grid.
            uint32_t want[8][3];
            for (uint32_t i = 0; i < 8; i++) {
                uint32_t u = 0;
                uint32_t v = 0;
                tegra_vic_transform_point(turn, 4, 2, i % 4, i / 4, &u, &v);
                expectYcc(&m, &t->out, kPatches[i], want[v * 2 + u]);
            }
            if (checkNv12(32, 64, 16, 16, want, 2, &bad))
                continue;
        }
        const uint8_t *yp = gDst.va;
        const uint8_t *uv = gDst.va + static_cast<uint64_t>(kPitch) * 64;
        snprintf(note, size, "off: transform %u into 420f %s (%u samples off; output patch 0,0 Y %u Cb %u Cr %u)", turn,
                 why ? why : "not the patches turned", bad, yp[8 * kPitch + 8], uv[4 * kPitch + 8], uv[4 * kPitch + 9]);
        dumpLastJob("BGRA -> 420f turned");
        t->rotations &= 0x0f;
        return;
    }
}

}  // namespace

extern "C" int tegra_vic_self_test(TegraVICTraits *traits)
{
    char line[300];
    char note[300] = "";
    char turnsNote[200] = "";
    if (tegra_vic_buffer(64LL * 1024, &gSrc) || tegra_vic_buffer(64LL * 1024, &gDst) ||
        tegra_vic_buffer(4096, &gPageA) || tegra_vic_buffer(4096, &gPageB)) {
        IOLog("TegraVIC: V3 FAIL no self-test buffers in the arena\n");
        return -12;
    }
    tegra_vic_lock();
    const uint64_t began = switch_now_us();
    if (!smmuProof(line, sizeof(line))) {
        tegra_vic_unlock();
        IOLog("TegraVIC: %s\n", line);
        return -5;
    }
    IOLog("TegraVIC: %s\n", line);
    TegraVICTraits t = {};
    t.pitchAlign = 256;
    gJobs = 0;
    gBusyUs = 0;
    const bool ok = transforms(&t, line, sizeof(line)) && colour(&t, line, sizeof(line), note, sizeof(note)) &&
                    scaling(&t, line, sizeof(line)) && pitch(&t, line, sizeof(line));
    if (ok) {
        recording(&t, turnsNote, sizeof(turnsNote));
        const TegraVICProbeEnv e = probeEnv();
        tegra_vic_probe_placement(&e, &t);
        t.blockLinearIn = (t.yccIn & TEGRA_VIC_YCC_IN_COPY) != 0u && t.in.cbChannel == t.out.cbChannel ? 1u : 0u;
    }
    tegra_vic_unlock();
    if (!ok) {
        dumpLastJob("V4");
        IOLog("TegraVIC: %s\n", line);
        return -5;
    }
    *traits = t;
    char turns[220];
    if (t.rotations & 0xf0) {
        snprintf(turns, sizeof(turns), "with rectangles %s, surface %s the transpose",
                 tegra_vic_probe_when(t.preTransposeRects), tegra_vic_probe_when(t.preTransposeSurface));
    } else {
        snprintf(turns, sizeof(turns), "%s", turnsNote[0] ? turnsNote : "none: see the transpose diag lines");
    }
    IOLog("TegraVIC: V4 ok transforms %#x of 0xff (quarter turns %s), sub-rectangles %#x, RGB to 420f BT.709 full (Cb "
          "in channel %u), fill %s, %s scaling, pitch %u-byte aligned; %u jobs, engine %llu us, %llu us in all\n",
          t.rotations, turns, t.subRects, t.out.cbChannel,
          t.fillThroughMatrix ? "through the matrix" : "converted here", t.bilinear ? "2-tap" : "1-tap only",
          t.pitchAlign, gJobs, gBusyUs, switch_now_us() - began);
    IOLog("TegraVIC: V4 %s\n", note);
    return 0;
}
