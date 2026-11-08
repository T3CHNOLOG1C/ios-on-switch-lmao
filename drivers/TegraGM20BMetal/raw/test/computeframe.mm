// SPDX-License-Identifier: MIT
// computeframe: one Metal kernel dispatched through the shipped frontend + m2v + NVK and through the raw path's compute encoder
// (runtime/raw_compute.h), over the mock GPU; test/computeeq.sh compares the two decodes (pack/eqcheck.py's DISPATCH records).
//   computeframe nvk <nvk dylib> <cache dir> <metallib> <kernel> <threads x,y,z> <groups x,y,z>
//     with NVDEC_KERNELS set, its log's K lines are the kernel's record (pack/nvdec_compute.cpp)
//   computeframe raw <pack.bin> <nvk log> <programs.bin> <groups x,y,z>
// Both bind every buffer slot 0..kSlots-1 to the same tagged bytes (pack/probe.mm's scheme).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "../runtime/raw_compute.h"

extern "C" void pushcount_report(const char *what);

namespace {

constexpr uint32_t kSlots = 8, kTagBytes = 4096;

std::vector<uint32_t> tag(uint32_t slot)
{
    std::vector<uint32_t> t(kTagBytes / 4);
    t[0] = 0xC0DE0000u | slot;
    for (uint32_t k = 1; k < t.size(); k++)
        t[k] = 0xA0000000u | slot << 16 | k;
    return t;
}

bool size3(const char *text, uint32_t v[3])
{
    return sscanf(text, "%u,%u,%u", &v[0], &v[1], &v[2]) == 3;
}

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

int runNvk(char **argv)
{
    uint32_t threads[3], groups[3];
    if (!size3(argv[6], threads) || !size3(argv[7], groups))
        return 2;
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    id<MTLDevice> dev = native_bridge_metal_open(argv[2], argv[3], 0, missing);
    if (!dev) {
        fprintf(stderr, "NO DEVICE %s\n", nmb_last_error());
        return 1;
    }
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[4])] error:&e];
    id<MTLFunction> f = [lib newFunctionWithName:@(argv[5])];
    id<MTLComputePipelineState> ps = f ? [dev newComputePipelineStateWithFunction:f error:&e] : nil;
    if (!ps) {
        fprintf(stderr, "NO STATE %s\n", e.localizedDescription.UTF8String ?: "no function");
        return 1;
    }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:ps];
    for (uint32_t s = 0; s < kSlots; s++)
        [enc setBytes:tag(s).data() length:kTagBytes atIndex:s];
    [enc dispatchThreadgroups:MTLSizeMake(groups[0], groups[1], groups[2])
        threadsPerThreadgroup:MTLSizeMake(threads[0], threads[1], threads[2])];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    pushcount_report("computeframe nvk");
    return cb.status == MTLCommandBufferStatusCompleted ? 0 : 1;
}

uint64_t fnv(const uint8_t *p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// The program named `id` in programs.bin (pack/mkprograms.py: 'NVPG', code, cbuf count, data, cbuf types, 80-byte header,
// code, data; nvdec names it by the FNV-1a of header + code, then data)
bool program(const char *path, const std::string &id, raw::ComputeRecord &r)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (size_t at = 0; at + 16 <= b.size();) {
        uint32_t code, count, data;
        memcpy(&code, &b[at + 4], 4);
        memcpy(&count, &b[at + 8], 4);
        memcpy(&data, &b[at + 12], 4);
        const uint8_t *types = &b[at + 16], *image = types + count, *bytes = image + 80 + code;
        char name[24];
        snprintf(name, sizeof name, "%llx", (unsigned long long)fnv(bytes, data, fnv(image, 80 + code)));
        if (id == name) {
            r.code.assign(image + 80, image + 80 + code);
            r.data.assign(bytes, bytes + data);
            for (uint32_t c = 0; c < count && c < 8; c++)
                r.cbufType[c] = types[c];
            return true;
        }
        at += 16 + count + 80 + code + data;
    }
    return false;
}

// The first dispatch's K lines of an NVDEC_KERNELS log
bool record(const char *log, const char *programs, raw::ComputeRecord &r)
{
    std::ifstream in(log);
    std::string line, prog;
    bool qmd = false;
    while (std::getline(in, line)) {
        if (!qmd && line.rfind("K qmd=", 0) == 0) {
            std::istringstream words(line.substr(6, line.find(' ', 6) - 6));
            std::string w;
            for (uint32_t i = 0; i < raw::kQmdWords && std::getline(words, w, ','); i++)
                r.qmd[i] = uint32_t(strtoul(w.c_str(), nullptr, 16));
            prog = line.substr(line.find(" prog=") + 6);
            qmd = true;
        } else if (qmd && line.rfind("K push", 0) == 0) {
            std::istringstream words(line.substr(6));
            std::string w;
            while (words >> w) {
                const int k = atoi(w.c_str());
                r.pushMask |= 1u << k;
                r.push[k] = uint32_t(strtoul(w.c_str() + w.find('=') + 1, nullptr, 16));
            }
        } else if (qmd && line.rfind("K entry ", 0) == 0) {
            unsigned offset, slot, bytes;
            if (sscanf(line.c_str(), "K entry %u buffer %u %x", &offset, &slot, &bytes) == 3)
                r.entries.push_back(
                    {uint16_t(offset), raw::ComputeBuffer, uint8_t(slot), bytes < kTagBytes ? bytes : 0});
        } else if (qmd && line.rfind("DISPATCH", 0) == 0) {
            break;
        }
    }
    return qmd && program(programs, prog, r);
}

int runRaw(char **argv)
{
    uint32_t groups[3];
    if (!size3(argv[5], groups))
        return 2;
    raw::ComputeRecord rec;
    if (!record(argv[3], argv[4], rec)) {
        fprintf(stderr, "no kernel record in %s\n", argv[3]);
        return 1;
    }
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[2], 0, err)) {
        fprintf(stderr, "NO DEVICE %s\n", err.c_str());
        return 1;
    }
    const raw::ComputeState *k = raw::placeCompute(dev, rec);
    raw::CommandBuffer *cb = dev.begin();
    if (!k || !cb) {
        fprintf(stderr, "no kernel or command buffer\n");
        return 1;
    }
    raw::ComputeEncoder enc(*cb, dev);
    enc.setState(k);
    for (uint32_t s = 0; s < kSlots; s++)
        enc.setBytes(s, tag(s).data(), kTagBytes);
    const char *why = nullptr;
    if (!enc.dispatch(groups, &why)) {
        fprintf(stderr, "refused: %s\n", why);
        return 1;
    }
    enc.end();
    uint32_t seq = 0;
    if (!cb->commit(seq) || !dev.wait(seq))
        return 1;
    pushcount_report("computeframe raw");
    fprintf(stderr, "raw: %llu dispatches, %llu programs placed\n", (unsigned long long)dev.stats.dispatches.load(),
            (unsigned long long)dev.stats.programsPlaced.load());
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc >= 8 && !strcmp(argv[1], "nvk"))
            return runNvk(argv);
        if (argc >= 6 && !strcmp(argv[1], "raw"))
            return runRaw(argv);
        fprintf(stderr, "usage: computeframe nvk <nvk> <cache> <metallib> <kernel> <threads x,y,z> <groups x,y,z>\n"
                        "       computeframe raw <pack.bin> <nvk log> <programs.bin> <groups x,y,z>\n");
        return 2;
    }
}
