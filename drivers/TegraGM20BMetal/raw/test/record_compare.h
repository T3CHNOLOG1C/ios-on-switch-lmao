// SPDX-License-Identifier: MIT
// Host tests: how a record the runtime made (raw/runtime/raw_record.h) differs from the pack's record of the same state, field by
// field; empty: none.
#pragma once
#include <cstring>
#include <string>
#include <vector>
#include "../runtime/raw_pack.h"
#include "../runtime/raw_record.h"

namespace recordcompare {

inline std::string differences(const raw::Pack &pack, const raw::PackPipeline &pp, const raw::RuntimeRecord &r)
{
    std::string why;
    const raw::PackPipeline &rp = r.pipeline;
    if (pack.string(pp.keyString) != r.keyString)
        why += " key-fields";
    const uint32_t *blob = pack.section<uint32_t>(raw::PackBlob) + pp.blob;
    if (pp.blobDwords != r.blob.size() || memcmp(blob, r.blob.data(), r.blob.size() * 4))
        why += " blob";
    if (pp.entries != rp.entries || memcmp(pp.entry, rp.entry, sizeof pp.entry))
        why += " entries";
    if (pp.pushes != rp.pushes || pp.pushMask != rp.pushMask || memcmp(pp.pushWords, rp.pushWords, sizeof pp.pushWords))
        why += " push";
    if (pp.vertexSlot != rp.vertexSlot || pp.vertexStride != rp.vertexStride)
        why += " vertex";
    if (pp.setBytes != rp.setBytes)
        why += " set-bytes";
    if (pp.flags != rp.flags)
        why += " flags";
    if (pp.keyHash != rp.keyHash)
        why += " key";
    if (!r.extras.empty() || !r.samplers.empty())
        why += " extras";
    for (int k = 0; k < 2; k++) {
        const raw::PackProgram &packed = pack.section<raw::PackProgram>(raw::PackPrograms)[k ? pp.fs : pp.vs];
        raw::PackProgram made;
        const std::vector<uint8_t> slot = raw::programSlot(r.programs[k], made);
        const uint8_t *heap = pack.section<uint8_t>(raw::PackHeap) + packed.heapOffset;
        if (packed.bytes != made.bytes || memcmp(heap, slot.data(), slot.size()) ||
            packed.program - packed.heapOffset != made.program || packed.dataOffset != made.dataOffset ||
            packed.dataBytes != made.dataBytes || packed.cbufs != made.cbufs ||
            memcmp(packed.cbufType, made.cbufType, sizeof made.cbufType))
            why += k ? " pixel-program" : " vertex-program";
    }
    return why;
}

}  // namespace recordcompare
