// SPDX-License-Identifier: MIT
// See raw_compute.h. Method numbers are NVIDIA's (Mesa src/nouveau/headers/nvidia/classes/clb1c0.h, cla1c0qmd.h, MIT).
#include "raw_compute.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace raw {
namespace {

enum : uint32_t {
    WAIT_FOR_IDLE = 0x0110,
    INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI = 0x0244,
    INVALIDATE_SKED_CACHES = 0x0298,
    SEND_PCAS_A = 0x02b4,
    SEND_SIGNALING_PCAS_B = 0x02bc,
    INVALIDATE_SHADER_CACHES_NO_WFI_3D = 0x0da4,
    INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI_3D = 0x1338,
    INVALIDATE_SAMPLER_CACHE_NO_WFI = 0x1424,
};

constexpr uint32_t kRootBytes = 0x800, kSetBytes = 0x100, kRootSets = 0x38, kRootPush = 0x700, kUploadChunk = 0x100;
constexpr uint32_t kPcasInvalidateSchedule = 3;
constexpr uint32_t kInvalidateData = 1u << 4, kInvalidateConstant = 1u << 12;
constexpr uint32_t kEncoderWords = 1 << 12;

// QMDV01_07 (cla1c0qmd.h) words
constexpr uint32_t kQmdProgramOffset = 8, kQmdRasterWidth = 12, kQmdRasterHeightDepth = 13, kQmdCbufValid = 20;

constexpr uint32_t qmdCbufLo(uint32_t i)
{
    return 29 + 2 * i;
}

std::mutex gStatesLock;
std::map<std::pair<const Device *, const ComputeRecord *>, ComputeState> gStates;

}  // namespace

const ComputeState *placeCompute(Device &dev, const ComputeRecord &r)
{
    std::lock_guard<std::mutex> hold(gStatesLock);
    auto it = gStates.find({&dev, &r});
    if (it != gStates.end())
        return &it->second;
    const size_t dataAt = r.data.empty() ? r.code.size() : (r.code.size() + 0xff) & ~size_t(0xff);
    std::vector<uint8_t> slot(dataAt + r.data.size(), 0);
    memcpy(slot.data(), r.code.data(), r.code.size());
    if (!r.data.empty())
        memcpy(slot.data() + dataAt, r.data.data(), r.data.size());
    uint64_t at = 0;
    {
        std::lock_guard<std::mutex> l(dev.lock_);
        if (!dev.heapPlace(slot.data(), uint32_t(slot.size()), at)) {
            dev.stats.heapFull++;
            return nullptr;
        }
    }
    dev.stats.programsPlaced++;
    ComputeState k;
    k.rec = &r;
    k.program = uint32_t(at);
    k.data = r.data.empty() ? 0 : dev.heap_.va + at + dataAt;
    return &gStates.emplace(std::make_pair(&dev, &r), k).first->second;
}

ComputeEncoder::ComputeEncoder(CommandBuffer &cb, Device &dev) : cb_(cb), dev_(dev)
{
    uint64_t va;
    start_ = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.push_, kEncoderWords * 4, 256, va));
    p_ = Push{start_, start_ + kEncoderWords};
    // the scheduler's cached QMDs (the upload ring reuses their addresses) and the compute class's header caches, as NVK opens
    // a command buffer's compute work (nvk_cmd_buffer_begin_compute)
    p_.m(WAIT_FOR_IDLE, 0);
    p_.m(INVALIDATE_SKED_CACHES, 0, SUBC_COMPUTE);
    p_.m(INVALIDATE_SAMPLER_CACHE_NO_WFI, 0, SUBC_COMPUTE);
    p_.m(INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0, SUBC_COMPUTE);
}

void ComputeEncoder::room(uint32_t words)
{
    if (p_.room() >= words + 16)
        return;
    cb_.segment(start_, p_.at);
    dev_.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_.push_.bo.cpu);
    uint64_t va;
    start_ = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.push_, kEncoderWords * 4, 256, va));
    p_ = Push{start_, start_ + kEncoderWords};
}

void ComputeEncoder::setBytes(uint32_t slot, const void *bytes, uint32_t length)
{
    if (slot >= 31)
        return;
    uint64_t va;
    const uint32_t chunk = std::max((length + kUploadChunk - 1) & ~(kUploadChunk - 1), kUploadChunk);
    uint8_t *m = dev_.alloc(dev_.upload_, chunk, kUploadChunk, va);
    memcpy(m, bytes, length);
    memset(m + length, 0, chunk - length);
    bufs_[slot] = {va, length, nullptr, 0};
    dev_.stats.uploadBytes += chunk;
}

void ComputeEncoder::setBuffer(uint32_t slot, const Buffer *b, uint64_t offset)
{
    if (slot >= 31)
        return;
    if (!b) {
        bufs_[slot] = {};
        return;
    }
    bufs_[slot] = {b->va + offset, uint32_t(std::min<uint64_t>(b->length - std::min(offset, b->length), 0xffffffffu)),
                   b, offset};
}

void ComputeEncoder::setTexture(uint32_t slot, const Texture *t)
{
    if (slot < 31)
        textures_[slot] = t;
}

void ComputeEncoder::setSampler(uint32_t slot, const Sampler *s)
{
    if (slot < 16)
        samplers_[slot] = s;
}

bool ComputeEncoder::dispatch(const uint32_t groups[3], const char **why)
{
    if (!state_) {
        *why = "a dispatch without a kernel";
        return false;
    }
    if (!groups[0] || !groups[1] || !groups[2])
        return true;
    if (groups[1] > 0xffff || groups[2] > 0xffff) {
        *why = "a grid taller or deeper than 65535 threadgroups";
        return false;
    }
    const ComputeRecord &r = *state_->rec;
    uint64_t setVa = 0;
    uint32_t *set = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.upload_, kSetBytes, 256, setVa));
    memset(set, 0, kSetBytes);
    for (const ComputeEntry &e : r.entries) {
        uint32_t *at = set + e.offset / 4;
        if (e.kind == ComputeBuffer) {
            const Bound &b = bufs_[e.slot];
            uint32_t range = std::min<uint32_t>((b.size + 15) & ~15u, 0x10000u);
            if (e.window && e.window < range)
                range = e.window;
            if (b.buffer && !b.buffer->wc && b.buffer->cpu)
                cb_.clean(b.buffer->cpu + b.offset, std::min<uint64_t>(range, b.size));
            at[0] = uint32_t(b.va);
            at[1] = uint32_t(b.va >> 32);
            at[2] = b.va ? range : 0;
            at[3] = 0;
        } else if (e.kind == ComputeTexture) {
            const Texture *t = textures_[e.slot];
            if (t && !t->tic) {
                *why = "a texture the raw path cannot sample";
                return false;
            }
            at[0] |= t ? t->tic : 0;
        } else {
            const Sampler *s = samplers_[e.slot];
            at[0] |= (s ? s->tsc : dev_.neutralTsc_) << 20;
        }
    }
    uint64_t rootVa = 0;
    uint32_t *root = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.upload_, kRootBytes, 256, rootVa));
    memset(root, 0, kRootBytes);
    root[3] = groups[0];
    root[4] = groups[1];
    root[5] = groups[2];
    root[kRootSets / 4 + 0] = uint32_t(setVa);
    root[kRootSets / 4 + 1] = uint32_t(setVa >> 32);
    root[kRootSets / 4 + 2] = kSetBytes;
    for (uint32_t m = r.pushMask; m; m &= m - 1)
        root[kRootPush / 4 + __builtin_ctz(m)] = r.push[__builtin_ctz(m)];
    uint64_t qmdVa = 0;
    uint32_t *qmd = reinterpret_cast<uint32_t *>(dev_.alloc(dev_.upload_, kQmdWords * 4, 256, qmdVa));
    memcpy(qmd, r.qmd, sizeof r.qmd);
    qmd[kQmdProgramOffset] = state_->program;
    qmd[kQmdRasterWidth] = groups[0];
    qmd[kQmdRasterHeightDepth] = groups[1] | groups[2] << 16;
    for (uint32_t i = 0; i < 8; i++) {
        if (!(qmd[kQmdCbufValid] >> i & 1))
            continue;
        uint64_t va = 0;
        switch (cbufKind(r.cbufType[i])) {
        case CbufRoot:
            va = rootVa;
            break;
        case CbufSet:
            va = setVa;
            break;
        case CbufShaderData:
            va = state_->data;
            break;
        default:
            *why = "a kernel reading a constant buffer the raw path does not bind";
            return false;
        }
        qmd[qmdCbufLo(i)] = uint32_t(va);
        qmd[qmdCbufLo(i) + 1] = (qmd[qmdCbufLo(i) + 1] & ~0xffu) | uint32_t(va >> 32);
    }
    dev_.stats.uploadBytes += kSetBytes + kRootBytes + kQmdWords * 4;
    room(8);
    p_.m(SEND_PCAS_A, uint32_t(qmdVa >> 8), SUBC_COMPUTE);
    p_.m(SEND_SIGNALING_PCAS_B, kPcasInvalidateSchedule, SUBC_COMPUTE);
    dev_.stats.dispatches++;
    return true;
}

void ComputeEncoder::end()
{
    room(8);
    p_.m(WAIT_FOR_IDLE, 0, SUBC_COMPUTE);
    p_.m(INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI_3D, 0);
    p_.m(INVALIDATE_SHADER_CACHES_NO_WFI_3D, kInvalidateData | kInvalidateConstant);
    cb_.segment(start_, p_.at);
    dev_.push_.head = uint64_t(reinterpret_cast<uint8_t *>(p_.at) - dev_.push_.bo.cpu);
    dev_.stats.pushBytes += uint64_t(p_.at - start_) * 4;
}

}  // namespace raw
