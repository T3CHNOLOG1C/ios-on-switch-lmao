// SPDX-License-Identifier: MIT
// raw GM20B path, pack generator: every corpus compute dispatch shape (the journal's D lines: a compute state and the
// threadgroup size it was dispatched with) made through the shipped frontend + m2v + NVK host builds over the mock GM20B, and
// dispatched once with tagged inputs, so mkpack.py can read off each kernel's record from nvdec's K lines
// (pack/nvdec_compute.cpp, NVDEC_KERNELS): NVK's QMD, the program, and where each Metal argument lands in set 0.
//
// Tags: every buffer slot is setBytes of 4096 bytes whose first word is 0xC0DE0000 | slot (the range m2v gives a slot below
// 4096 is the function's window on it); texture slot t is a (16 + t) x (8 + t) RGBA8 texture; sampler slot s has a max LOD
// clamp of 1 + s / 2 (a sampler made before them takes NVK's sampler index 0); threadgroup memory as the line's tgmem=
// field names it (<index>:<length>,...). One command buffer per shape, its grid (3 + i)x5x2 threadgroups for the i-th line
// (its record names the dispatch by it). stdout: "KPROBE <i> tg=<x,y,z>
// grid=<x,y,z> key=<raw_compute_key.h key>" before each one is committed, "KREFUSED <i> <why>" for a shape not made.
// Libraries: lib=<path> under PROBE_LIB_ROOT (the crawl's root), lib=data:<SHA-256> as <PROBE_DATA_LIBS>/<SHA-256>.metallib.
// usage: probecompute <nvk dylib> <cache dir> <corpus.log>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "../runtime/raw_compute_key.h"

extern "C" void pushcount_report(const char *what);

namespace {

constexpr uint32_t kSlots = 31, kTextures = 31, kSamplers = 16, kTagBytes = 4096;

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

struct Shape {
    uint32_t threads[3] = {};
    std::string form;  // "f" or "d"
    uint64_t options = 0;
    uint32_t maxThreads = 0;
    bool multipleOfWidth = false;
    std::string function, library, constants;
    bool specialized = false;
    std::string threadgroupMemory;  // "<index>:<length>,..." (raw_compute_key.h)
};

// A D line: "D ok=1 tg=x,y,z edge=a,b,c threads=t | form=... | k <name> lib=<library> [spec] <constants> | ..."; false where
// the raw path dispatches no such shape (a grid with an edge, linked functions)
bool parse(const std::string &line, Shape &s, std::string &why)
{
    uint32_t edge[3], threadsForm;
    if (sscanf(line.c_str(), "D ok=1 tg=%u,%u,%u edge=%u,%u,%u threads=%u", &s.threads[0], &s.threads[1], &s.threads[2],
               &edge[0], &edge[1], &edge[2], &threadsForm) != 7) {
        why = "not a D line";
        return false;
    }
    if (edge[0] || edge[1] || edge[2]) {
        why = "a grid with an edge";
        return false;
    }
    if (const size_t at = line.find(" tgmem="); at != std::string::npos && at < line.find(" | "))
        s.threadgroupMemory = line.substr(at + 7, line.find_first_of(" |", at + 7) - at - 7);
    const size_t state = line.find(" | form=");
    const size_t k = line.find(" | k ", state);
    if (state == std::string::npos || k == std::string::npos) {
        why = "no state";
        return false;
    }
    if (line.find(" | l ", k) != std::string::npos || line.find(" | p ", k) != std::string::npos) {
        why = "linked functions";
        return false;
    }
    std::istringstream head(line.substr(state + 3, k - state - 3));
    std::string field;
    while (head >> field) {
        if (field.rfind("form=", 0) == 0)
            s.form = field.substr(5);
        else if (field.rfind("opt=", 0) == 0)
            s.options = strtoull(field.c_str() + 4, nullptr, 10);
        else if (field.rfind("max=", 0) == 0)
            s.maxThreads = uint32_t(strtoul(field.c_str() + 4, nullptr, 10));
        else if (field.rfind("tgm=", 0) == 0)
            s.multipleOfWidth = field[4] == '1';
    }
    const size_t end = line.find(" | ", k + 5);
    std::istringstream fn(line.substr(k + 5, (end == std::string::npos ? line.size() : end) - k - 5));
    fn >> s.function;
    while (fn >> field && field != "|") {
        if (field.rfind("lib=", 0) == 0)
            s.library = field.substr(4);
        else if (field == "spec")
            s.specialized = true;
        else
            s.constants += (s.constants.empty() ? "" : " ") + field;
    }
    return !s.function.empty() && !s.library.empty();
}

// "#<index>:<MTLDataType>:<hex bytes>" or "<name>:<MTLDataType>:<hex bytes>" (RawMetalLibrary.mm constantsText)
MTLFunctionConstantValues *constantValues(const std::string &text)
{
    MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
    std::istringstream in(text);
    std::string c;
    while (in >> c) {
        const size_t a = c.find(':'), b = c.find(':', a + 1);
        if (a == std::string::npos || b == std::string::npos)
            continue;
        const MTLDataType type = MTLDataType(strtoul(c.c_str() + a + 1, nullptr, 10));
        std::vector<uint8_t> bytes;
        for (size_t i = b + 1; i + 1 < c.size(); i += 2)
            bytes.push_back(uint8_t(strtoul(c.substr(i, 2).c_str(), nullptr, 16)));
        if (c[0] == '#')
            [values setConstantValue:bytes.data() type:type atIndex:strtoul(c.c_str() + 1, nullptr, 10)];
        else
            [values setConstantValue:bytes.data() type:type withName:@(c.substr(0, a).c_str())];
    }
    return values;
}

id<MTLLibrary> library(id<MTLDevice> dev, const std::string &name)
{
    static NSMutableDictionary<NSString *, id<MTLLibrary>> *loaded = [NSMutableDictionary dictionary];
    NSString *key = @(name.c_str());
    if (!loaded[key]) {
        std::string path;
        if (name.rfind("data:", 0) == 0) {
            const char *dir = getenv("PROBE_DATA_LIBS");
            path = std::string(dir ? dir : ".") + "/" + name.substr(5) + ".metallib";
        } else {
            const char *root = getenv("PROBE_LIB_ROOT");
            path = std::string(root ? root : "") + name;
        }
        NSError *e = nil;
        if (id<MTLLibrary> l = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path.c_str())] error:&e])
            loaded[key] = l;
    }
    return loaded[key];
}

id<MTLComputePipelineState> state(id<MTLDevice> dev, const Shape &s, id<MTLFunction> f, std::string &why)
{
    NSError *e = nil;
    id<MTLComputePipelineState> made = nil;
    if (s.form == "d") {
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = f;
        d.maxTotalThreadsPerThreadgroup = s.maxThreads;
        d.threadGroupSizeIsMultipleOfThreadExecutionWidth = s.multipleOfWidth;
        made = [dev newComputePipelineStateWithDescriptor:d
                                                  options:MTLPipelineOption(s.options)
                                               reflection:nil
                                                    error:&e];
    } else {
        made = [dev newComputePipelineStateWithFunction:f options:MTLPipelineOption(s.options) reflection:nil error:&e];
    }
    if (!made)
        why = e.localizedDescription.UTF8String ?: "refused";
    return made;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: probecompute <nvk dylib> <cache dir> <corpus.log>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        id<MTLSamplerState> first = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        (void)first;
        NSMutableArray<id<MTLSamplerState>> *samplers = [NSMutableArray array];
        for (uint32_t s = 0; s < kSamplers; s++) {
            MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
            d.lodMaxClamp = 1 + s / 2.0f;
            [samplers addObject:[dev newSamplerStateWithDescriptor:d]];
        }
        NSMutableArray<id<MTLTexture>> *textures = [NSMutableArray array];
        for (uint32_t t = 0; t < kTextures; t++) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:16 + t
                                                                                        height:8 + t
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            [textures addObject:[dev newTextureWithDescriptor:d]];
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        NSString *text = [NSString stringWithContentsOfFile:@(argv[3]) encoding:NSUTF8StringEncoding error:nil];
        std::map<std::string, bool> seen;
        unsigned index = 0, made = 0, refused = 0, failed = 0;
        for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
            if (![l hasPrefix:@"D ok=1 "])
                continue;
            const std::string line = l.UTF8String;
            Shape s;
            std::string why;
            const unsigned i = index++;
            if (!parse(line, s, why)) {
                printf("KREFUSED %u %s\n", i, why.c_str());
                refused++;
                continue;
            }
            id<MTLLibrary> lib = library(dev, s.library);
            NSError *e = nil;
            id<MTLFunction> f = !lib ? nil
                                : s.specialized || !s.constants.empty()
                                    ? [lib newFunctionWithName:@(s.function.c_str())
                                                constantValues:constantValues(s.constants)
                                                         error:&e]
                                    : [lib newFunctionWithName:@(s.function.c_str())];
            const void *bytes = nullptr;
            size_t length = 0;
            std::map<std::string, raw::AirHash> air;
            if (!f || !native_bridge_metal_function_metallib(f, &bytes, &length) ||
                !raw::metallibAirHashes(static_cast<const uint8_t *>(bytes), length, air) || !air.count(s.function)) {
                printf("KREFUSED %u no function %s in %s\n", i, s.function.c_str(), s.library.c_str());
                refused++;
                continue;
            }
            id<MTLComputePipelineState> ps = state(dev, s, f, why);
            if (!ps) {
                printf("KREFUSED %u %s\n", i, why.c_str());
                refused++;
                continue;
            }
            const std::string key = raw::computeKey(air[s.function], s.constants, s.maxThreads, s.multipleOfWidth,
                                                    s.threads, s.threadgroupMemory);
            if (seen[key])
                continue;
            seen[key] = true;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:ps];
            std::vector<uint32_t> tag(kTagBytes / 4);
            for (uint32_t slot = 0; slot < kSlots; slot++) {
                tag[0] = 0xC0DE0000u | slot;
                for (uint32_t k = 1; k < tag.size(); k++)
                    tag[k] = 0xA0000000u | slot << 16 | k;
                [enc setBytes:tag.data() length:kTagBytes atIndex:slot];
            }
            for (uint32_t t = 0; t < kTextures; t++)
                [enc setTexture:textures[t] atIndex:t];
            for (uint32_t t = 0; t < kSamplers; t++)
                [enc setSamplerState:samplers[t] atIndex:t];
            std::istringstream memory(s.threadgroupMemory);
            std::string pair;
            while (std::getline(memory, pair, ','))
                [enc setThreadgroupMemoryLength:strtoul(pair.c_str() + pair.find(':') + 1, nullptr, 10)
                                        atIndex:strtoul(pair.c_str(), nullptr, 10)];
            const uint32_t grid[3] = {3 + i, 5, 2};
            [enc dispatchThreadgroups:MTLSizeMake(grid[0], grid[1], grid[2])
                threadsPerThreadgroup:MTLSizeMake(s.threads[0], s.threads[1], s.threads[2])];
            [enc endEncoding];
            printf("KPROBE %u tg=%u,%u,%u grid=%u,%u,%u key=%s\n", i, s.threads[0], s.threads[1], s.threads[2], grid[0],
                   grid[1], grid[2], key.c_str());
            fflush(stdout);
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) {
                failed++;
                printf("KFAILED %u %s\n", i, cb.error.localizedDescription.UTF8String);
            } else {
                made++;
            }
        }
        fprintf(stderr, "probecompute: %u shapes dispatched, %u refused, %u failed\n", made, refused, failed);
        pushcount_report("probecompute");
    }
    return 0;
}
