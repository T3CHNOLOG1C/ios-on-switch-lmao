// SPDX-License-Identifier: MIT
// raw GM20B path, over the mock GPU: what the raw device hands the GPU stays valid. A view frees its own texture header alone,
// never what its parent owns (a planar or YCbCr texture's planes, a stand-in), and a converted texture has no raw view.
// Build it with -fsanitize=address: the old view freed its parent's planes, which the parent then used and freed again.
// usage: faultcheck <pack.bin>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include "../runtime/raw_gpu.h"

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

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: faultcheck <pack.bin>\n");
        return 2;
    }
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
    if (failures) {
        std::printf("FAIL: faultcheck (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: faultcheck\n");
    return 0;
}
