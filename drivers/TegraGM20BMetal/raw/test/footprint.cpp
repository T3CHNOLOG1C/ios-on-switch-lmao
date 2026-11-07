// SPDX-License-Identifier: MIT
// What the raw device costs a process, over the mock GPU: the bytes of kernel BOs it holds (allocations, and imports of its own
// memory) and of its malloc heap, at its open and after its first state, texture and command buffer. Every record of the pack is
// found by its key, in either record order (mkpack.py sorts them; an older pack goes through an index).
// usage: footprint <pack.bin>
#include <malloc/malloc.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "../runtime/raw_gpu.h"

extern "C" void mock_iokit_held(uint64_t *allocated, uint64_t *imported);
extern "C" unsigned long mock_iokit_calls(unsigned method);

namespace {
int failures;

void expect(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

uint64_t heapInUse()
{
    malloc_statistics_t s = {};
    malloc_zone_statistics(nullptr, &s);
    return s.size_in_use;
}

struct Sample {
    uint64_t allocated, imported, heap;
    unsigned long channels;
};

Sample sample()
{
    Sample s = {};
    mock_iokit_held(&s.allocated, &s.imported);
    s.heap = heapInUse();
    s.channels = mock_iokit_calls(TegraGM20BMethodChannelCreate);
    return s;
}

void report(const char *stage, const Sample &base, const Sample &now)
{
    std::printf("%-22s BOs %6llu KiB allocated, %6llu KiB imported; malloc +%llu KiB; channels %lu\n", stage,
                (unsigned long long)(now.allocated >> 10), (unsigned long long)(now.imported >> 10),
                (unsigned long long)((now.heap - base.heap) >> 10), now.channels);
}

bool everyRecordFound(const raw::Pack &pack)
{
    const raw::PackPipeline *p = pack.section<raw::PackPipeline>(raw::PackPipelines);
    for (uint32_t i = 0; i < pack.header()->pipelines; i++) {
        const raw::PackForm form =
            p[i].flags & raw::PipelineBarrierForm ? raw::PackForm::Barrier : raw::PackForm::Default;
        const raw::PackPipeline *found = pack.find(p[i].keyHash, form);
        if (!found || found->keyHash != p[i].keyHash)
            return false;
    }
    return !pack.find(0x0123456789abcdefull);
}
}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: footprint <pack.bin>\n");
        return 2;
    }
    const Sample base = sample();
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    const Sample opened = sample();
    report("open", base, opened);
    expect(everyRecordFound(dev.pack()), "every record of the pack is found by its key");
    const raw::PackPipeline *first = dev.pack().section<raw::PackPipeline>(raw::PackPipelines);
    dev.pipeline(first->keyHash);
    report("first state", base, sample());
    raw::Texture t;
    dev.createTexture(256, 256, 80, t);
    report("first texture", base, sample());
    raw::CommandBuffer *cb = dev.begin();
    uint32_t seq = 0;
    const bool committed = cb && cb->commit(seq);
    report("first command buffer", base, sample());
    expect(committed && dev.wait(seq), "an empty command buffer runs");
    dev.destroyTexture(t);
    if (failures) {
        std::printf("FAIL: footprint (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: footprint\n");
    return 0;
}
