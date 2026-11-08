// SPDX-License-Identifier: MIT
// See RawDirect.h.
#import "RawDirect.h"

#include <cstring>
#include "NativeBridgeMetalFrontend.h"
#include "raw_pack.h"

// Weak, so the host benchmark links without the frontend
extern "C" bool native_bridge_metal_render_direct(id descriptor, bool barrier, NSUInteger textureFormat,
                                                  NSUInteger textureSamples,
                                                  bool (*each)(const struct NmbDirectState *, void *), void *ctx,
                                                  const char **why) __attribute__((weak_import));
extern "C" void *native_bridge_metal_nvk_symbol(const char *name) __attribute__((weak_import));

namespace raw {
namespace {

struct Asked {
    const DirectNvk &nvk;
    const PackRules &rules;
    const std::string &key;
    const std::vector<uint32_t> &attributes;
    RuntimeRecord &out;
    std::string why;
    Compiler::Outcome outcome = Compiler::Outcome::Failed;
};

bool made(const NmbDirectState *state, void *ctx)
{
    Asked &a = *static_cast<Asked *>(ctx);
    a.outcome = directRecord(*state, a.nvk, a.rules, a.key, a.attributes, a.out, a.why);
    return true;
}

}  // namespace

Direct::Direct(const Pack &pack)
    : rules_(PackRules::of(pack)),
      nvk_(native_bridge_metal_nvk_symbol ? DirectNvk::find(native_bridge_metal_nvk_symbol) : DirectNvk{})
{}

Compiler::Outcome Direct::form(MTLRenderPipelineDescriptor *descriptor, bool barrier, ProbeTextures textures,
                               const std::string &key, const std::vector<uint32_t> &attributes, RuntimeRecord &out,
                               std::string &why)
{
    Asked a = {nvk_, rules_, key, attributes, out, {}};
    const char *refused = nullptr;
    if (!native_bridge_metal_render_direct(descriptor, barrier, textures.format, textures.samples, made, &a,
                                           &refused)) {
        why = std::string("m2v refused its translation: ") + (refused ? refused : "");
        return refused && !strncmp(refused, "retry: ", 7) ? Compiler::Outcome::Failed : Compiler::Outcome::Refused;
    }
    why = a.why;
    return a.outcome;
}

Compiler::Outcome Direct::make(MTLRenderPipelineDescriptor *descriptor, const std::string &key, RuntimeRecord &out,
                               std::string &why, ProbeTextures textures)
{
    if (!native_bridge_metal_render_direct || !nvk_.complete()) {
        why = "the frontend or NVK has no direct compile";
        return Compiler::Outcome::Failed;
    }
    std::vector<uint32_t> attributes;
    for (MTLVertexAttribute *a in descriptor.vertexFunction.vertexAttributes)
        if (a.isActive)
            attributes.push_back(uint32_t(a.attributeIndex));
    Compiler::Outcome outcome = form(descriptor, false, textures, key, attributes, out, why);
    if (outcome != Compiler::Outcome::Refused || why.rfind("storage form", 0) != 0)
        return outcome;
    outcome = form(descriptor, true, textures, key, attributes, out, why);
    if (outcome == Compiler::Outcome::Made)
        out.pipeline.flags |= PipelineBarrierForm;
    return outcome;
}

}  // namespace raw
