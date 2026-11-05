// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca, r8a): the raw device's memory follows what Core Animation holds, not its peak. A burst of shared
// buffers (write-combined GPU-pool chunks, r17) and textures (pool chunks) is made and freed, over the mock GPU; after it the
// device keeps one empty write-combined chunk and a chunk's worth of free pool memory, not the burst's (r8a: the compositor
// held 107 MiB of the GPU pool at 15:16 and 63 after, the raw pool 320 MB in 20 chunks to the end, and Maps was refused
// a 4 MiB BO with 33 MiB of the pool free).
// usage: poolcheck <pack.bin>
#include <cstdio>
#include <string>
#include <vector>
#include "../runtime/raw_gpu.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: poolcheck <pack.bin>\n");
        return 2;
    }
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    int failures = 0;
    auto expect = [&](bool ok, const char *what) {
        std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
        failures += !ok;
    };
    const uint64_t poolBefore = dev.stats.poolBytes.load();
    // the burst: 48 MiB of shared buffers (Core Animation's buffer pool at a busy screen) and 96 MiB of textures
    std::vector<raw::Buffer> buffers(192);
    unsigned wc = 0;
    for (raw::Buffer &b : buffers) wc += dev.createBuffer(256 << 10, b, true) && b.wc;
    std::vector<raw::Texture> textures(24);
    unsigned made = 0;
    for (raw::Texture &t : textures) made += dev.createTexture(1024, 1024, 80, t);  // 4 MiB each
    const uint64_t wcPeak = dev.stats.wcBytes.load(), poolPeak = dev.stats.poolBytes.load();
    std::printf("burst: %u write-combined buffers (%llu KiB), %u textures, pool %llu -> %llu MiB\n", wc,
                (unsigned long long)(wcPeak >> 10), made, (unsigned long long)(poolBefore >> 20),
                (unsigned long long)(poolPeak >> 20));
    expect(wc == buffers.size() && made == textures.size(), "the burst was made");
    for (raw::Buffer &b : buffers) dev.destroyBuffer(b);
    for (raw::Texture &t : textures) dev.destroyTexture(t);
    const uint64_t poolAfter = dev.stats.poolBytes.load();
    std::printf("after: write-combined %llu KiB in buffers, %u chunks (%llu MiB) held; pool %llu MiB\n",
                (unsigned long long)(dev.stats.wcBytes.load() >> 10), dev.wcChunksHeld(),
                (unsigned long long)(dev.wcChunkBytesHeld() >> 20), (unsigned long long)(poolAfter >> 20));
    expect(dev.stats.wcBytes.load() == 0, "no write-combined buffer bytes left");
    expect(dev.wcChunksHeld() <= 1, "at most one empty write-combined chunk kept (was: every chunk of the burst)");
    expect(poolAfter <= poolBefore + (32ull << 20),
           "the pool back to a chunk's worth past its start (was: the burst's)");
    // and it grows again for the next burst
    raw::Buffer again;
    expect(dev.createBuffer(256 << 10, again, true) && again.wc, "a shared buffer after the trim is write-combined");
    dev.destroyBuffer(again);
    if (failures) {
        std::printf("FAIL: poolcheck (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: poolcheck\n");
    return 0;
}
