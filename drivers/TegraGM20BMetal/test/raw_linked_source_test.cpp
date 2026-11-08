// SPDX-License-Identifier: MIT
// Guards on a render pipeline whose functions call linked functions by name (iOS 27's IconRendering: custom effects call
// custom_fn), which no host model runs:
//  - the frontend hands each stage's vertexLinkedFunctions/fragmentLinkedFunctions to the bridge as it hands a compute
//    descriptor's linkedFunctions, and the bridge gives them to m2v's function descriptors (no m2v change: its render
//    pipeline takes a stage's LinkedFunctions already);
//  - the C and Rust forms of the descriptor list the same fields in the same order;
//  - the raw path's twin links the twins of those functions, and its record is keyed by their AIR, never a pack state's.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <regex>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;

namespace {

TEST(RawLinkedSource, TheFrontendHandsEachStagesLinkedFunctionsToTheBridge)
{
    const std::string frontend = SourceText("../../NativeMetal/NativeBridgeMetalFrontend.mm");
    const std::string render = Body(frontend, "static bool renderDescriptor(MTLRenderPipelineDescriptor *d,");
    EXPECT_THAT(render, HasSubstr("stageLinked(d.vertexLinkedFunctions, linked,"));
    EXPECT_THAT(render, HasSubstr("stageLinked(d.fragmentLinkedFunctions, linked,"));
    for (const char *field :
         {"p->vertex_linked =", "p->vertex_private_linked =", "p->fragment_linked =", "p->fragment_private_linked ="})
        EXPECT_THAT(render, HasSubstr(field));
    const std::string stage = Body(frontend, "static bool stageLinked(MTLLinkedFunctions *functions,");
    EXPECT_THAT(stage, HasSubstr("linkedHandles(functions.functions, linked)"));
    EXPECT_THAT(stage, HasSubstr("linkedHandles(functions.privateFunctions, linked)"));
    // the async path makes the state on a worker only when every function, linked ones too, is this device's
    EXPECT_THAT(Body(frontend, "static bool asyncRenderPipelineWanted("),
                HasSubstr("d.vertexLinkedFunctions, d.fragmentLinkedFunctions"));

    const std::string bridge = SourceText("../../NativeMetal/bridge/NativeMetalObjects.rs");
    const std::string convert = Body(bridge, "fn render_descriptor(descriptor: *const NmbRenderPipelineDescriptor)");
    EXPECT_THAT(convert, HasSubstr("function_descriptor().with_linked(vertex_linked)"));
    EXPECT_THAT(convert, HasSubstr("function_descriptor().with_linked(fragment_linked)"));
}

std::vector<std::string> fields(const std::string &body, const std::regex &field)
{
    std::vector<std::string> out;
    for (auto it = std::sregex_iterator(body.begin(), body.end(), field); it != std::sregex_iterator(); ++it)
        out.push_back((*it)[1]);
    return out;
}

TEST(RawLinkedSource, TheDescriptorsCAndRustFormsAgree)
{
    const std::string c = Between(SourceText("../../NativeMetal/bridge/NativeMetalObjects.h"),
                                  "struct NmbRenderPipelineDescriptor {", "};");
    const std::string rust = Between(SourceText("../../NativeMetal/bridge/NativeMetalObjects.rs"),
                                     "pub struct NmbRenderPipelineDescriptor {", "\n}");
    const std::vector<std::string> namesC = fields(c, std::regex(R"((\w+_(?:linked|count));)"));
    EXPECT_EQ(namesC, fields(rust, std::regex(R"(pub (\w+_(?:linked|count)):)")));
    EXPECT_EQ(namesC.size(), 8u);
}

TEST(RawLinkedSource, TheRawRecordIsTheLinkedFunctionsToo)
{
    const std::string states = SourceText("../raw/runtime/RawMetalStates.mm");
    const std::string twin = Body(states, "static MTLRenderPipelineDescriptor *twinDescriptor(");
    EXPECT_THAT(twin, HasSubstr("t.vertexLinkedFunctions = twinLinked(d.vertexLinkedFunctions);"));
    EXPECT_THAT(twin, HasSubstr("t.fragmentLinkedFunctions = twinLinked(d.fragmentLinkedFunctions);"));
    EXPECT_THAT(Body(states, "- (RMPipeline *)pipelineFor:"), HasSubstr("!linksFunctions(d)"));

    const std::string key =
        Body(SourceText("../raw/runtime/RawComplete.mm"), "bool Completion::keyOf(Ask &a, CacheKey &k)");
    EXPECT_THAT(key, HasSubstr("a.descriptor.vertexLinkedFunctions"));
    EXPECT_THAT(key, HasSubstr("a.descriptor.fragmentLinkedFunctions"));
    EXPECT_THAT(key, HasSubstr("a.key += \" | linked\" + linked;"));
}

// A state the pack holds is this device's archive hit: every way the facade asks m2v for its twin (at once, with
// reflection, through a completion handler) drops the creation's archives and FailOnBinaryArchiveMiss, so a command buffer
// that falls back never meets a twin m2v refused (device boot 10: "foreign render pipeline", 833 failed frames).
TEST(RawLinkedSource, APackedStatesTwinNeverFailsOnTheArchive)
{
    const std::string states = SourceText("../raw/runtime/RawMetalStates.mm");
    EXPECT_THAT(Body(states, "static MTLRenderPipelineDescriptor *packedTwinDescriptor("),
                HasSubstr("t.binaryArchives = nil;"));
    EXPECT_THAT(Body(states, "static MTLPipelineOption packedTwinOptions("),
                HasSubstr("o & ~MTLPipelineOptionFailOnBinaryArchiveMiss"));
    EXPECT_THAT(Body(states, "static id<MTLRenderPipelineState> pipelineTwin("),
                HasSubstr("packedTwinOptions(o, true)"));
    const std::string reflected = Between(states, "    if (reflection) {", "\n    }\n");
    EXPECT_THAT(reflected, HasSubstr("packedTwinOptions(o, packed)"));
    const std::string handler =
        Between(states, "completionHandler:(MTLNewRenderPipelineStateWithReflectionCompletionHandler)handler", "\n}\n");
    EXPECT_THAT(handler, HasSubstr("packedTwinOptions(o, packed)"));
    EXPECT_THAT(handler, HasSubstr("packedTwinDescriptor(d, packed)"));
}

}  // namespace
