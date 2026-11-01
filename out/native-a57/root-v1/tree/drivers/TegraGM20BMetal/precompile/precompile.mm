// r6-precompile: the shader cache backboardd's Core Animation writes on the Switch, made on this Mac before the boot. Every
// function specialization (S lines) and render pipeline state (R lines) of the device journals (pipelines.log, the frontend's) is
// made again through the shipped frontend + m2v (host build of the same sources: same cache build directory) and NVK (host build
// of the same sources, its LC_UUID set to the iOS dylib's: same pipelineCacheUUID), over the GM20B mock (mock_iokit.c, chipset
// 0x12e as the T210B01 reports), into an empty cache directory: m2v's translations, its creation answers, and the driver record
// (NAK's SM53 code) of every pipeline a state's creation makes (m2v makes the pipeline its first draw is predicted to take inside
// the creation, pipeline/predicted.rs). No draw is needed: on the r3v and r4d boots every driver record the device wrote is one a
// creation made. Fails (exit 1) when any specialization or state is refused. The journals of every process are harvested
// (harvest.py), but a journal line does not name its library: a specialization or state of a function QuartzCore's library does
// not have (Core Image's kernels in iconservicesagent, an app's own library) is another library's, skipped and counted.
// The cache is shared by every process: m2v's names hash the build, the key and the device's profile, none of them per process
// (r4d-b: iconservicesagent and backboardd wrote the same record names, byte for byte).
// The last line is the frontend's rates line over the replay (cache=h<hits>/m<misses>/w<writes>, drv=l<driver loads>/...): with a
// seed beneath an empty cache (METAL2VULKAN_SHADER_CACHE_SEED), every state of the seed shows as a hit and writes nothing.
// usage: precompile <host libvulkan_nouveau.dylib> <cache dir> <QuartzCore default.metallib> <pipelines.log>...
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <set>
#include <stdio.h>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
static void missing(const char *cls, const char *sel) { fprintf(stderr, "MISSING -[%s %s]\n", cls, sel); }

static std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size()) {
        size_t e = s.find(' ', at);
        if (e == std::string::npos) e = s.size();
        if (e > at) out.push_back(s.substr(at, e - at));
        at = e + 1;
    }
    return out;
}

// "name:type:hex" or "#index:type:hex" items (the journal's constants text) into constant values.
static MTLFunctionConstantValues *constants(const std::vector<std::string> &items) {
    MTLFunctionConstantValues *v = [MTLFunctionConstantValues new];
    for (const std::string &item : items) {
        size_t a = item.rfind(':'), b = a == std::string::npos ? a : item.rfind(':', a - 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        std::string name = item.substr(0, b), type = item.substr(b + 1, a - b - 1), hex = item.substr(a + 1);
        uint8_t bytes[64] = {};
        for (size_t i = 0; i + 1 < hex.size() && i / 2 < sizeof bytes; i += 2) bytes[i / 2] = (uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16);
        if (name[0] == '#') [v setConstantValue:bytes type:(MTLDataType)atoi(type.c_str()) atIndex:strtoul(name.c_str() + 1, nullptr, 10)];
        else [v setConstantValue:bytes type:(MTLDataType)atoi(type.c_str()) withName:@(name.c_str())];
    }
    return v;
}

// The functions QuartzCore's library has.
static std::set<std::string> gNames;
static bool known(const std::vector<std::string> &stage) { return stage.empty() || stage[0] == "-" || gNames.count(stage[0]); }

// A stage's function as the journal names it: no constants text is -newFunctionWithName: (no S line is written for it), else
// -newFunctionWithName:constantValues:error: with exactly those values.
static id<MTLFunction> function(id<MTLLibrary> qc, const std::vector<std::string> &stage) {
    if (stage.empty() || stage[0] == "-") return nil;
    if (stage.size() == 1) return [qc newFunctionWithName:@(stage[0].c_str())];
    NSError *e = nil;
    id<MTLFunction> f = [qc newFunctionWithName:@(stage[0].c_str())
                                 constantValues:constants(std::vector<std::string>(stage.begin() + 1, stage.end())) error:&e];
    if (!f) fprintf(stderr, "function %s: %s\n", stage[0].c_str(), e.localizedDescription.UTF8String);
    return f;
}

struct State {
    std::string key, label;
    MTLRenderPipelineDescriptor *d;
    id<MTLRenderPipelineState> state;
};

// An R line's descriptor (frontend journalRenderPipeline): samples, topology class, depth/stencil formats, label, color
// attachments (format:writeMask:blend:srcRGB:dstRGB:opRGB:srcA:dstA:opA), vertex attributes (format:offset:buffer:stride:step:rate),
// then both functions with their constants. False for a malformed line; `foreign` when a function is another library's.
static bool parseState(id<MTLLibrary> qc, const std::string &line, State &s, bool &foreign) {
    size_t bar1 = line.find(" | v "), bar2 = line.find(" | f "), bar3 = line.find(" | ", bar2 + 5);
    if (bar1 == std::string::npos || bar2 == std::string::npos) return false;
    std::vector<std::string> head = split(line.substr(0, bar1));
    std::vector<std::string> v = split(line.substr(bar1 + 5, bar2 - bar1 - 5));
    std::vector<std::string> f = split(line.substr(bar2 + 5, (bar3 == std::string::npos ? line.size() : bar3) - bar2 - 5));
    if ((foreign = !known(v) || !known(f))) return false;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor *vd = nil;
    std::string key;
    for (const std::string &h : head) {
        if (h.rfind("label=", 0) == 0) { s.label = h.substr(6); if (s.label != "-") d.label = @(s.label.c_str()); }
        else if (h.rfind("samples=", 0) == 0) d.rasterSampleCount = strtoul(h.c_str() + 8, nullptr, 10);
        else if (h.rfind("topo=", 0) == 0) d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)strtoul(h.c_str() + 5, nullptr, 10);
        else if (h.rfind("ds=", 0) == 0) {
            unsigned a = 0, b = 0;
            sscanf(h.c_str() + 3, "%u/%u", &a, &b);
            d.depthAttachmentPixelFormat = (MTLPixelFormat)a;
            d.stencilAttachmentPixelFormat = (MTLPixelFormat)b;
        } else if (h[0] == 'c' && isdigit(h[1])) {
            unsigned i, x[9] = {};
            if (sscanf(h.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6, x + 7, x + 8) != 10 || i >= 8) continue;
            MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
            c.pixelFormat = (MTLPixelFormat)x[0]; c.writeMask = x[1]; c.blendingEnabled = x[2];
            c.sourceRGBBlendFactor = (MTLBlendFactor)x[3]; c.destinationRGBBlendFactor = (MTLBlendFactor)x[4];
            c.rgbBlendOperation = (MTLBlendOperation)x[5]; c.sourceAlphaBlendFactor = (MTLBlendFactor)x[6];
            c.destinationAlphaBlendFactor = (MTLBlendFactor)x[7]; c.alphaBlendOperation = (MTLBlendOperation)x[8];
        } else if (h[0] == 'a' && isdigit(h[1])) {
            unsigned i, x[6] = {};
            if (sscanf(h.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5) != 7 || i >= 31 || x[2] >= 31) continue;
            if (!vd) vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[i].format = (MTLVertexFormat)x[0]; vd.attributes[i].offset = x[1]; vd.attributes[i].bufferIndex = x[2];
            vd.layouts[x[2]].stride = x[3]; vd.layouts[x[2]].stepFunction = (MTLVertexStepFunction)x[4]; vd.layouts[x[2]].stepRate = x[5];
        } else continue;
        if (h.rfind("label=", 0) != 0) key += h + " ";
    }
    d.vertexDescriptor = vd;
    d.vertexFunction = function(qc, v);
    d.fragmentFunction = function(qc, f);
    if (!d.vertexFunction || (!d.fragmentFunction && !f.empty() && f[0] != "-")) return false;
    s.key = key + line.substr(bar1, (bar3 == std::string::npos ? line.size() : bar3) - bar1);
    s.d = d;
    return true;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 5) { fprintf(stderr, "usage: precompile <nvk dylib> <cache dir> <default.metallib> <pipelines.log>...\n"); return 2; }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) { fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error()); return 1; }
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        if (!qc) { fprintf(stderr, "library: %s\n", error.localizedDescription.UTF8String); return 1; }
        for (NSString *name in qc.functionNames) gNames.insert(name.UTF8String);
        char rates[1024];
        native_bridge_metal_rates(rates, sizeof rates);   // the replay's rates start here

        // The journals, oldest first: each specialization and state once, in the order the device first made it.
        std::vector<std::pair<std::string, std::vector<std::string>>> specializations;
        std::set<std::string> seenS, seenR;
        unsigned foreignS = 0, foreignR = 0;
        std::vector<std::string> stateLines;
        for (int a = 4; a < argc; a++) {
            NSString *text = [NSString stringWithContentsOfFile:@(argv[a]) encoding:NSUTF8StringEncoding error:nil];
            if (!text) text = [NSString stringWithContentsOfFile:@(argv[a]) encoding:NSISOLatin1StringEncoding error:nil];
            if (!text) { fprintf(stderr, "cannot read %s\n", argv[a]); return 1; }
            for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
                std::string line = l.UTF8String ?: "";
                if (line.find(" ok=1 ") == std::string::npos) continue;
                if (line.rfind("S ", 0) == 0) {
                    size_t n = line.find(" name="), k = line.find(" k="), bar = line.find(" | ", k == std::string::npos ? 0 : k);
                    if (n == std::string::npos || k == std::string::npos) continue;
                    if (!gNames.count(line.substr(n + 6, k - n - 6))) { foreignS++; continue; }
                    std::string name = line.substr(n + 6, k - n - 6), items = line.substr(k + 3, (bar == std::string::npos ? line.size() : bar) - k - 3);
                    if (seenS.insert(name + " " + items).second) specializations.push_back({name, split(items)});
                } else if (line.rfind("R ", 0) == 0) {
                    stateLines.push_back(line);
                }
            }
        }
        const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        unsigned functions = 0, functionFailures = 0;
        for (auto &s : specializations) {
            NSError *e = nil;
            id<MTLFunction> f = [qc newFunctionWithName:@(s.first.c_str()) constantValues:constants(s.second) error:&e];
            if (f) functions++;
            else { functionFailures++; fprintf(stderr, "specialization %s: %s\n", s.first.c_str(), e.localizedDescription.UTF8String); }
        }
        const uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        std::vector<State> states;
        unsigned refused = 0;
        for (const std::string &line : stateLines) {
            State s;
            bool foreign = false;
            if (!parseState(qc, line, s, foreign)) { if (foreign) foreignR++; else refused++; continue; }
            if (!seenR.insert(s.key).second) continue;
            s.state = [dev newRenderPipelineStateWithDescriptor:s.d error:&error];
            if (!s.state) { refused++; fprintf(stderr, "REFUSED %s: %s\n", s.label.c_str(), error.localizedDescription.UTF8String); continue; }
            states.push_back(s);
        }
        const uint64_t t2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        printf("precompile: %u specializations (%u failed) in %.0f ms, %zu states (%u refused) in %.0f ms; other libraries' lines "
               "skipped: %u specializations, %u states\n", functions, functionFailures, (t1 - t0) / 1e6, states.size(), refused,
               (t2 - t1) / 1e6, foreignS, foreignR);
        if (native_bridge_metal_rates(rates, sizeof rates)) printf("rates: %s\n", rates);
        return functionFailures || refused ? 1 : 0;
    }
}
