// SPDX-License-Identifier: MIT
// nvdec's compute class (MAXWELL_COMPUTE_B, subchannel 1): a dispatch's canonical record, as nvdec.h describes for draws. Its QMD
// (Mesa src/nouveau/headers/nvidia/classes/cla1c0qmd.h, QMDV01_07: what NAK's nak_fill_qmd writes for GM20B) is printed field
// by field, with its addresses replaced by what they hold: the program by its code, each constant buffer by its content.
#include "nvdec.h"

#include <cstring>

namespace nvdec {
namespace {

// MAXWELL_COMPUTE_B methods (clb1c0.h)
enum : uint32_t {
    kWaitForIdle = 0x0110,
    kInvalidateShaderCaches = 0x021c,
    kInvalidateTextureHeaderCacheNoWfi = 0x0244,
    kInvalidateSkedCaches = 0x0298,
    kSendPcasA = 0x02b4,
    kInvalidateTextureDataCacheNoWfi = 0x1288,
    kInvalidateSamplerCacheNoWfi = 0x1424,
    kProgramRegionA = 0x1608,
    kProgramRegionB = 0x160c,
};

uint32_t field(const uint32_t *q, unsigned lo, unsigned hi)
{
    uint64_t v = q[lo / 32];
    if (hi / 32 != lo / 32)
        v |= uint64_t(q[hi / 32]) << 32;
    const unsigned width = hi - lo + 1;
    return uint32_t((v >> (lo % 32)) & ((width == 32) ? 0xffffffffull : ((1ull << width) - 1)));
}

std::string hex64(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "%llx", (unsigned long long)v);
    return b;
}

}  // namespace

const ProgramInfo *Decoder::computeProgram(uint64_t at, std::string &id)
{
    if (computeByHead_.empty())
        for (const auto &kv : programs)
            if (kv.second.image.size() > 80 + kComputeHead)
                computeByHead_.emplace(fnv(kv.second.image.data() + 80, kComputeHead), &kv.second);
    uint8_t head[256];
    if (!read(at, head, sizeof head)) {
        id = "unmapped";
        return nullptr;
    }
    auto range = computeByHead_.equal_range(fnv(head, kComputeHead));
    for (auto it = range.first; it != range.second; ++it) {
        const ProgramInfo &p = *it->second;
        const size_t code = p.image.size() - 80;
        std::vector<uint8_t> b(code), data(p.data.size());
        read(at, b.data(), code);
        if (memcmp(b.data(), p.image.data() + 80, code))
            continue;
        if (!data.empty() && !(read(at + ((code + 0xff) & ~size_t(0xff)), data.data(), data.size()) && data == p.data))
            continue;
        id = p.id;
        return &p;
    }
    id = "unknown:" + hex64(fnv(head, sizeof head));
    return nullptr;
}

// The compute class's barriers carry the names nvdec gives the 3D class's
void Decoder::computeMethod(uint32_t mthd, uint32_t data)
{
    switch (mthd) {
    case kWaitForIdle:
        barrier("WFI");
        break;
    case kInvalidateShaderCaches:
        barrier("SHINV");
        if (data & 1)
            barrier("ICINV");
        break;
    case kInvalidateTextureHeaderCacheNoWfi:
        barrier("TICINV");
        break;
    case kInvalidateSamplerCacheNoWfi:
        barrier("TSCINV");
        break;
    case kInvalidateTextureDataCacheNoWfi:
        barrier("TEXINV");
        break;
    case kInvalidateSkedCaches:
        barrier("SKEDINV");
        break;
    case kSendPcasA:
        dispatch(uint64_t(data) << 8);
        break;
    default:
        break;
    }
}

// SEND_PCAS_A: the QMD at `qmd`. The record names the work, not where it lies: the QMD's every field but the program offset
// and the constant buffer addresses (QMDV01_07_PROGRAM_OFFSET, CONSTANT_BUFFER_ADDR_*), then the program and what each valid
// constant buffer holds (cb0, NVK's root table: its cs words, set addresses and push words, as nvdec prints a draw's)
void Decoder::dispatch(uint64_t qmd)
{
    dispatches++;
    uint32_t q[64];
    read(qmd, q, sizeof q);
    if (onDispatch)
        onDispatch(*this, qmd);
    std::string bar;
    for (auto &b : pendingBarriers_)
        bar += (bar.empty() ? "" : ",") + b;
    pendingBarriers_.clear();
    fprintf(out_, "DISPATCH grid=%u,%u,%u block=%u,%u,%u barriers=%s\n", field(q, 384, 415), field(q, 416, 431),
            field(q, 432, 447), field(q, 592, 607), field(q, 608, 623), field(q, 624, 639), bar.c_str());
    uint32_t masked[64];
    memcpy(masked, q, sizeof masked);
    masked[8] = 0;
    for (int i = 0; i < 8; i++) {
        masked[29 + 2 * i] = 0;
        masked[30 + 2 * i] &= ~0xffu;
    }
    fprintf(out_, "  qmd=%s smem=%x l1=%u regs=%u barriers=%u slm=%x/%x crs=%x sass=%x version=%u.%u\n",
            hex64(fnv(masked, sizeof masked)).c_str(), field(q, 544, 561), field(q, 669, 671), field(q, 1496, 1503),
            field(q, 1467, 1471), field(q, 1440, 1463), field(q, 1472, 1495), field(q, 1504, 1527),
            field(q, 1528, 1535), field(q, 580, 583), field(q, 576, 579));
    const uint64_t region = (uint64_t(reg_[1][kProgramRegionA >> 2] & 0xff) << 32) | reg_[1][kProgramRegionB >> 2];
    std::string prog;
    computeProgram(region + q[8], prog);
    fprintf(out_, "  prog=%s\n", prog.c_str());
    static const bool probe = getenv("NVDEC_KERNELS") != nullptr;
    if (probe)
        probeKernel(q, prog);
    layout_ = nullptr;
    for (int i = 0; i < 8; i++) {
        if (!field(q, 640 + i, 640 + i))
            continue;
        const uint64_t addr = (uint64_t(field(q, 960 + 64 * i, 967 + 64 * i)) << 32) | q[29 + 2 * i];
        const uint32_t size = field(q, 975 + 64 * i, 991 + 64 * i);
        if (i == 0) {
            uint8_t cs[0x18];
            read(addr, cs, sizeof cs);
            fprintf(out_, "  cb0 root cs=%s size=%x\n", hex64(fnv(cs, sizeof cs)).c_str(), size);
            for (uint32_t set = 0; set < 2; set++)
                fprintf(out_, "  cb0 root set%u=%s\n", set,
                        contentHash(addr + 0x38 + 16 * set, 16, 0, false, true).c_str());
            fprintf(out_, "  cb0 root push=%s\n", contentHash(addr + 0x700, 0x80, 1, false, false).c_str());
        } else {
            fprintf(out_, "  cb%d size=%x data=%s\n", i, size, contentHash(addr, size, 1, true, false).c_str());
        }
    }
}

// NVDEC_KERNELS: what a probe dispatch shows of its kernel (raw_compute.h KernelRecord), for the pack's generator: the QMD, the
// program, and set 0's entries, found by the probe's tags (each buffer slot's bytes start 0xC0DE0000 | slot; the range a
// descriptor gives a 4096-byte payload is the function's window on it)
void Decoder::probeKernel(const uint32_t *q, const std::string &prog)
{
    std::string line = "K qmd=";
    for (int i = 0; i < 64; i++) {
        char w[12];
        snprintf(w, sizeof w, "%s%x", i ? "," : "", q[i]);
        line += w;
    }
    fprintf(out_, "%s prog=%s\n", line.c_str(), prog.c_str());
    if (!field(q, 640, 640))
        return;
    const uint64_t root = (uint64_t(field(q, 960, 967)) << 32) | q[29];
    uint32_t r[0x800 / 4];
    read(root, r, sizeof r);
    std::string push = "K push";
    for (int k = 0; k < 32; k++)
        if (r[0x700 / 4 + k]) {
            char w[24];
            snprintf(w, sizeof w, " %d=%x", k, r[0x700 / 4 + k]);
            push += w;
        }
    fprintf(out_, "%s\n", push.c_str());
    const uint64_t set = (uint64_t(r[0x38 / 4 + 1]) << 32) | r[0x38 / 4];
    const uint32_t setBytes = r[0x38 / 4 + 2];
    if (!set || setBytes > 0x1000)
        return;
    std::vector<uint32_t> w(setBytes / 4);
    read(set, w.data(), setBytes);
    for (uint32_t k = 0; k + 4 <= w.size(); k += 4) {
        const uint64_t va = (uint64_t(w[k + 1]) << 32) | w[k];
        uint32_t tag = 0;
        if (va >> 32 && read(va, &tag, 4) && (tag & 0xffff0000u) == 0xc0de0000u)
            fprintf(out_, "K entry %u buffer %u %x\n", k * 4, tag & 0xff, w[k + 2]);
    }
}

}  // namespace nvdec
