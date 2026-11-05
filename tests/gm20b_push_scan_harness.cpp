// SPDX-License-Identifier: MIT
// drivers/TegraGM20B/TegraGM20BPushScan.h on the host: the kernel's tegra-gpu-stats method-stream reader over streams laid out
// as NVK writes them for GM20B (a Core Animation-like submission: the command buffer prologue, a pass into a pitch-linear
// 720x1280 target, its draws in a push of their own, barriers, NVK's copy-engine WFI, a method continued across two pushes;
// a multisampled pass; a push the kernel cannot read), checking every counter and each push's profile class. A push the
// profile may stamp after must end on a method boundary: the stamp would otherwise land inside a method's data.
//   gm20b_push_scan_harness   prints PASS: push scan
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../drivers/TegraGM20B/TegraGM20BPushScan.h"

using namespace tgm_scan;

static int failures;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
            failures++;                                                               \
        }                                                                             \
    } while (0)

// NV906F DMA headers
static uint32_t inc(uint32_t subc, uint32_t mthd, uint32_t count) { return (1u << 29) | (count << 16) | (subc << 13) | (mthd >> 2); }
static uint32_t one_inc(uint32_t subc, uint32_t mthd, uint32_t count) { return (5u << 29) | (count << 16) | (subc << 13) | (mthd >> 2); }
static uint32_t immd(uint32_t subc, uint32_t mthd, uint32_t data) { return (4u << 29) | (data << 16) | (subc << 13) | (mthd >> 2); }

using Push = std::vector<uint32_t>;

// nvk_BeginCommandBuffer's prologue for a primary (begin_compute + begin_graphics), as the r15 host twin dumped it
static void prologue(Push &p)
{
    for (uint32_t w : {0x800020a6u, 0x80002509u, 0x80002091u, 0x20020509u, 0u, 0u, 0x90000369u}) p.push_back(w);
}

// one color target and the pass's begin: SET_COLOR_TARGET_A..MEMORY(0), SET_CT_SELECT, SET_ZT_SELECT, SET_ANTI_ALIAS
static void begin_pass(Push &p, uint32_t width, uint32_t height, bool pitch, uint32_t samples_log2, bool depth)
{
    p.push_back(inc(0, 0x0800, 6));
    for (uint32_t w : {0x3eu, 0xf7e0c000u, width, height, 0xcfu, pitch ? (1u << 12) : 0u}) p.push_back(w);
    p.push_back(immd(0, 0x121c, 1));
    p.push_back(immd(0, 0x1538, depth ? 1 : 0));
    p.push_back(one_inc(0, 0x3800 + 10 * 8, 1));
    p.push_back((0xf0u << 16) | (samples_log2 << 4));
}

static void draws(Push &p, int n)
{
    for (int i = 0; i < n; i++) {
        p.push_back(one_inc(0, 0x3800 + 11 * 8, 4));  // NVK_MME_DRAW and its arguments
        for (uint32_t w : {6u, 1u, 0u, 0u}) p.push_back(w);
    }
}

struct Result {
    PushCounts c = {};
    std::vector<uint8_t> cls;
};

static Result scan(const std::vector<const Push *> &pushes)
{
    Result r;
    PushParse p = {};
    bool whole = true;
    for (const Push *push : pushes) r.cls.push_back(scanPush(r.c, p, whole, push ? push->data() : nullptr, push ? push->size() : 0));
    return r;
}

int main()
{
    // 1. A Core Animation-like submission
    Push p0, p1, p2, p3, p4, p5;
    prologue(p0);
    begin_pass(p0, 720 * 4, 1280, true, 0, false);  // pitch: WIDTH is bytes
    draws(p1, 3);
    p2 = {immd(0, 0x0110, 0), immd(0, 0x021c, 0x14), immd(0, 0x1288, 0)};             // WFI, SHINV+flush, texinv
    p3 = {inc(4, 0x0418, 2), 0, 0, immd(4, 0x0300, 0x186)};                           // NVK's copy-engine WFI
    p4 = {inc(0, 0x1b00, 4), 0x3e, 0x1000};                                           // a report: two of four words here
    p5 = {0, 0x10000002, immd(0, 0x0050, 0), immd(0, 0x0034, 0x0e), immd(0, 0x0da4, 0x10)};  // the rest, SET_REFERENCE,
                                                                                      // L2 sysmem invalidate, shinv
    Result a = scan({&p0, &p1, &p2, &p3, &p4, &p5});
    CHECK(a.c.passes == 1 && a.c.depthPasses == 0 && a.c.msaaPasses == 0);
    CHECK(a.c.targets == 1 && a.c.linearTargets == 1);
    CHECK(a.c.draws == 3 && a.c.clears == 0);
    CHECK(a.c.wfi == 1 && a.c.shaderInval == 1 && a.c.texInval == 1);
    CHECK(a.c.shaderInvalNoWfi == 2);  // the prologue's constant invalidate and p5's
    CHECK(a.c.ceWfi == 1 && a.c.ceCopies == 0 && a.c.ceBytes == 0);
    CHECK(a.c.setRef == 1 && a.c.l2Inval == 1 && a.c.hostWfi == 0);
    CHECK(a.c.unparsed == 0);
    CHECK(a.cls[0] == TGM_PC_PASS);
    CHECK(a.cls[1] == TGM_PC_DRAW + 0 * 4 + 2 + 0);  // full (921600 pixels), pitch, one sample
    CHECK(a.cls[2] == TGM_PC_SYNC);
    CHECK(a.cls[3] == TGM_PC_SYNC);
    CHECK(a.cls[4] == TGM_PC_SKIP);  // its method continues into p5: no stamp may go between them
    CHECK(a.cls[5] == TGM_PC_SYNC);

    // 2. A multisampled pass with depth into a block-linear 1280x720 target, a real copy, a small clear pass
    Push q0, q1, q2, q3;
    prologue(q0);
    begin_pass(q0, 1280, 720, false, 2, true);
    draws(q0, 2);  // inline draws: the push is a draw push
    q1 = {inc(4, 0x0418, 2), 4096, 64, immd(4, 0x0300, (1u << 9) | 0x186)};  // 64 lines of 4 KiB
    q2 = {};
    prologue(q2);
    begin_pass(q2, 128, 64, false, 0, false);
    q2.push_back(one_inc(0, 0x3800 + 2 * 8, 2));  // NVK_MME_CLEAR
    q2.push_back(0x3c);
    q2.push_back(0);
    q3 = {immd(1, 0x02b4, 0)};  // a dispatch
    Result b = scan({&q0, &q1, &q2, &q3});
    CHECK(b.c.passes == 2 && b.c.depthPasses == 1 && b.c.msaaPasses == 1);
    CHECK(b.c.draws == 2 && b.c.clears == 1 && b.c.dispatches == 1);
    CHECK(b.c.ceCopies == 1 && b.c.ceBytes == 4096 * 64 && b.c.ceWfi == 0);
    CHECK(b.cls[0] == TGM_PC_DRAW + 0 * 4 + 0 + 1);  // full, block-linear, multisampled
    CHECK(b.cls[1] == TGM_PC_COPY);
    CHECK(b.cls[2] == TGM_PC_CLEAR + 2 * 4 + 0 + 0);  // small, block-linear, one sample
    CHECK(b.cls[3] == TGM_PC_COMPUTE);

    // 3. A push the kernel cannot read: neither it nor any push after it is stamped
    Result c = scan({&p0, nullptr, &p1, &p2});
    CHECK(c.cls[0] == TGM_PC_PASS);
    CHECK(c.cls[1] == TGM_PC_SKIP && c.cls[2] == TGM_PC_SKIP && c.cls[3] == TGM_PC_SKIP);
    CHECK(c.c.draws == 3);  // still counted

    // 4. An opcode NVK never writes ends the stamps too
    Push bad = {0x00000001u};  // GRP0 use-tert: not one of NVK's
    Result d = scan({&p2, &bad, &p2});
    CHECK(d.cls[0] == TGM_PC_SYNC && d.cls[1] == TGM_PC_SKIP && d.cls[2] == TGM_PC_SKIP && d.c.unparsed == 1);

    if (failures) return 1;
    std::printf("PASS: push scan (%d checks)\n", 4);
    return 0;
}
