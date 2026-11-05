// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca): the raw sampler header (runtime/raw_gpu.cpp samplerHeader) against NVK's for the same Metal sampler
// descriptors (pack/mksamplers.py over a PROBE_SAMPLERS run of the twin). usage: samplercheck <samplers.txt>
#include <cstdio>
#include <cstring>
#include "../runtime/raw_gpu.h"
int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "r") : nullptr;
    if (!f) return 2;
    unsigned n = 0, bad = 0, i, mn, mg, mp, s, t, r, an, lod;
    uint32_t want[8];
    while (fscanf(f, "%u %u %u %u %u %u %u %u %u %x %x %x %x %x %x %x %x", &i, &mn, &mg, &mp, &s, &t, &r, &an, &lod,
                  &want[0], &want[1], &want[2], &want[3], &want[4], &want[5], &want[6], &want[7]) == 17) {
        raw::SamplerDesc d;
        d.minFilter = uint8_t(mn), d.magFilter = uint8_t(mg), d.mipFilter = uint8_t(mp);
        d.sAddress = uint8_t(s), d.tAddress = uint8_t(t), d.rAddress = uint8_t(r), d.maxAnisotropy = uint8_t(an);
        if (lod) d.lodMin = 0.5f, d.lodMax = 3.25f;
        uint32_t got[8];
        raw::samplerHeader(d, got);
        n++;
        if (memcmp(got, want, 32)) {
            bad++;
            if (bad <= 8) {
                printf("sampler %u (min %u mag %u mip %u addr %u/%u/%u aniso %u lod %u):\n  nvk", i, mn, mg, mp, s, t,
                       r, an, lod);
                for (uint32_t w : want) printf(" %08x", w);
                printf("\n  raw");
                for (uint32_t w : got) printf(" %08x", w);
                printf("\n");
            }
        }
    }
    printf("samplers %u, identical %u\n", n, n - bad);
    return bad ? 1 : 0;
}
