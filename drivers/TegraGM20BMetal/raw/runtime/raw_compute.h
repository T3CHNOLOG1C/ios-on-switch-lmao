// SPDX-License-Identifier: MIT
// The raw GM20B path's compute work: Metal kernels as MAXWELL_COMPUTE_B launches on the raw channel, beside its 3D and copy
// work. The pack's context init binds the class on subchannel 1 with the raw heap as its program region and the raw texture
// and sampler pools (raw_pack.h PackInit), so a dispatch is a QMD in the upload ring and SEND_PCAS: no compute state lives in
// the channel between dispatches.
//
// A kernel is NVK's work for one Metal kernel at one threadgroup shape (m2v compiles the local size, and the threadgroup
// memory it declares, into the program): NAK's program, the QMD NVK filled for it (nak_fill_qmd), which of its constant
// buffers are NVK's root table and set 0, and where set 0 takes each Metal buffer, texture and sampler (ComputeEntry). A dispatch
// writes those as NVK's nvk_cmd_upload_qmd does: its own root table (cs group count, the set's address, the push words) and set
// 0 in the upload ring, and a QMD of the template with the program's heap offset, the grid and the two addresses.
//
// Order. A compute encoder is ordered after the work encoded before it and before the work after it on the GPU, as Metal orders
// a serial encoder: a wait for idle where it starts (the render and copy work before it has written), and where it ends a wait
// for idle with the texture and constant caches invalidated (what the 3D and copy engines read next was written through L2).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "raw_gpu.h"

namespace raw {

constexpr uint32_t kQmdWords = 64;

enum ComputeEntryKind : uint8_t {
    ComputeBuffer,
    ComputeTexture,
    ComputeSampler
};

// A word range of set 0 and the Metal argument it holds: a buffer's {address, range} descriptor (its range the bound bytes
// padded to 16, at most `window` bytes where the function proves it reads no further: m2v's descriptor_range), a texture's
// header index or a sampler's (tic | tsc << 20, either alone)
struct ComputeEntry {
    uint16_t offset;
    uint8_t kind;
    uint8_t slot;
    uint32_t window;
};

struct ComputeRecord {
    std::string key;
    std::vector<uint8_t> code, data;  // NAK's program, and its shader data (uploaded 256-aligned after the code)
    uint32_t qmd[kQmdWords] = {};  // NVK's QMD, its program offset, grid and constant buffer addresses as it left them
    uint8_t cbufType[8] = {};      // each constant buffer slot's raw_pack.h PackCbufType (CbufRoot, CbufSet, ...)
    std::vector<ComputeEntry> entries;
    uint32_t pushMask = 0, push[32] = {};  // root table [0x700, 0x780): the words m2v's push ranges write, and which
};

// A record's program placed in the raw heap (Device::kernel)
struct ComputeState {
    const ComputeRecord *rec = nullptr;
    uint32_t program = 0;  // QMD PROGRAM_OFFSET: from the program region (the heap's start)
    uint64_t data = 0;     // its shader data's address, 0: none
};

class ComputeEncoder {
public:
    ComputeEncoder(CommandBuffer &cb, Device &dev);

    void setState(const ComputeState *k)
    {
        state_ = k;
    }

    void setBytes(uint32_t slot, const void *bytes, uint32_t length);
    void setBuffer(uint32_t slot, const Buffer *buffer, uint64_t offset);
    void setTexture(uint32_t slot, const Texture *t);
    void setSampler(uint32_t slot, const Sampler *s);
    // dispatchThreadgroups: `groups` threadgroups of the kernel's shape. False: refused (*why), nothing encoded
    bool dispatch(const uint32_t groups[3], const char **why);
    void end();

private:
    struct Bound {
        uint64_t va = 0;
        uint32_t size = 0;
        const Buffer *buffer = nullptr;
        uint64_t offset = 0;
    };

    CommandBuffer &cb_;
    Device &dev_;
    Push p_;
    uint32_t *start_ = nullptr;
    const ComputeState *state_ = nullptr;
    Bound bufs_[31];
    const Texture *textures_[31] = {};
    const Sampler *samplers_[16] = {};
    void room(uint32_t words);
};

// The record's program in the heap, placed at its first use (null: no room)
const ComputeState *placeCompute(Device &dev, const ComputeRecord &r);

}  // namespace raw
