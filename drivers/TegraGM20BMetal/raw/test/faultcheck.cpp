// SPDX-License-Identifier: MIT
// raw GM20B path, over the mock GPU (its decoder: what reached the GPU): what the raw device hands the GPU stays valid.
// - A view frees its own texture header alone, never what its parent owns (a planar or YCbCr texture's planes, a stand-in), and
//   a converted texture has no raw view. Build it with -fsanitize=address: the old view freed its parent's planes, which the
//   parent then used and freed again.
// - An instanced draw of thousands of instances stays in its push reservations (test/build.sh's harness builds stop a push
//   that ran past one) and reaches the GPU whole.
// usage: faultcheck <pack.bin>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include "../runtime/raw_gpu.h"

extern "C" uint64_t cap_draws(void);
extern "C" uint32_t cap_reg3d(uint32_t mthd);

namespace {

using Expect = std::function<void(bool, const char *)>;

constexpr uint32_t kBGRA8 = 80, kBGRA8sRGB = 81, kR8 = 10;

void views(raw::Device &dev, const Expect &expect)
{
    raw::Texture parent;
    if (!dev.createTexture(64, 64, kBGRA8, parent)) {
        expect(false, "a texture to view");
        return;
    }
    // what a converted texture owns, by hand: no host pack has the conversion states that make one
    parent.luma = new raw::Texture;
    parent.standIn = new raw::Texture;
    expect(dev.createTexture(64, 64, kR8, *parent.luma) && dev.createTexture(64, 64, kBGRA8, *parent.standIn),
           "the parent's plane and stand-in");
    const uint32_t lumaTic = parent.luma->tic;
    raw::Texture view;
    expect(dev.createView(parent, kBGRA8sRGB, view) && view.tic && view.tic != parent.tic,
           "a view with its own header");
    dev.destroyTexture(view);
    expect(parent.luma && parent.luma->tic == lumaTic && parent.standIn && parent.standIn->tic,
           "the view's end left the parent's plane and stand-in alone");
    raw::Texture again;
    expect(dev.createView(parent, kBGRA8sRGB, again), "the parent viewed again");
    dev.destroyTexture(again);
    parent.planar = true;
    expect(!dev.createView(parent, kBGRA8sRGB, again),
           "a planar texture has no raw view (Metal views it as 542 alone)");
    parent.planar = false;
    parent.ycbcr = 1;
    expect(!dev.createView(parent, kBGRA8sRGB, again), "a YCbCr texture has no raw view");
    parent.ycbcr = 0;
    dev.destroyTexture(parent);
}

// A pass over targets of the formats of a pack state that fetches vertices and tests no depth, and that state
struct Pass {
    raw::Pipeline *state = nullptr;
    raw::Texture targets[8];
    raw::PassDesc desc;

    bool make(raw::Device &dev)
    {
        const raw::PackPipeline *recs = dev.pack().section<raw::PackPipeline>(raw::PackPipelines);
        for (uint32_t i = 0; i < dev.pack().header()->pipelines && !state; i++)
            if (raw::Pipeline *p = dev.pipeline(recs[i].keyHash); p && !p->depthFormat && p->rec->vertexSlot < 31)
                state = p;
        if (!state)
            return false;
        for (uint32_t j = 0; j < 8; j++)
            if (const uint32_t f = state->colorFormats[j]) {
                if (!dev.createTexture(64, 64, f, targets[j]))
                    return false;
                desc.color[j].texture = &targets[j];
                desc.count = j + 1;
            }
        return desc.count != 0;
    }
};

void instances(raw::Device &dev, Pass &pass, const Expect &expect)
{
    constexpr uint32_t kInstances = 4000;
    raw::Buffer indices;
    if (!dev.createBuffer(64, indices)) {
        expect(false, "an index buffer");
        return;
    }
    const uint16_t tri[3] = {0, 1, 2};
    memcpy(indices.cpu, tri, sizeof tri);
    const uint64_t before = cap_draws();
    raw::CommandBuffer *cb = dev.begin();
    raw::RenderEncoder *e = cb ? cb->render(pass.desc) : nullptr;
    const char *why = nullptr;
    bool drawn = false;
    if (e) {
        e->setPipeline(pass.state);
        drawn = e->draw(3, 0, 3, kInstances, 0, &why) && e->drawIndexed(3, 3, 2, &indices, 0, kInstances, 0, 0, &why);
        e->end();
    }
    uint32_t seq = 0;
    expect(drawn && cb->commit(seq) && dev.wait(seq), "a draw and an indexed draw of 4000 instances each, submitted");
    expect(cap_draws() - before == 2 * kInstances, "every instance of both reached the GPU");
    dev.destroyBuffer(indices);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: faultcheck <pack.bin>\n");
        return 2;
    }
    setenv("RAWCAP_LOG", "/dev/null", 0);  // the mock decodes every submission
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    int failures = 0;
    const Expect expect = [&](bool ok, const char *what) {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    views(dev, expect);
    Pass pass;
    if (!pass.make(dev)) {
        std::fprintf(stderr, "no pack state to draw with\n");
        return 1;
    }
    instances(dev, pass, expect);
    if (failures) {
        std::printf("FAIL: faultcheck (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: faultcheck\n");
    return 0;
}
