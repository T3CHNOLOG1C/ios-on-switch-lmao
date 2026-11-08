// SPDX-License-Identifier: MIT
// The raw device's direct compile of a state the pack lacks (raw_direct.h), in process: the state's record made from m2v's
// translation of its functions and NVK's stage compile, at its creation, as the pack generator makes the pack's records. A state
// whose fragment function reads its color attachments under the pixel interlock gets its barrier form.
#pragma once
#import <Metal/Metal.h>
#include <string>
#include "raw_compiler.h"
#include "raw_direct.h"

namespace raw {

class Pack;

class Direct {
public:
    explicit Direct(const Pack &pack);

    // The record of the state `descriptor` (of m2v's functions) describes, whose pack key is `key`; `textures`: the format of
    // the textures it is planned for, as the probe binds them (pack/probe.mm: BGRA8 but for the raw path's own code reads)
    Compiler::Outcome make(MTLRenderPipelineDescriptor *descriptor, const std::string &key, RuntimeRecord &out,
                           std::string &why, MTLPixelFormat textures = MTLPixelFormatBGRA8Unorm);

private:
    PackRules rules_;
    DirectNvk nvk_;

    Compiler::Outcome form(MTLRenderPipelineDescriptor *descriptor, bool barrier, MTLPixelFormat textures,
                           const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                           std::string &why);
};

}  // namespace raw
