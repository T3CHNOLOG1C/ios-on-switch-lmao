// SPDX-License-Identifier: MIT
// directcheck: the direct compile (runtime/raw_direct.h) of every state of a corpus against the pack. Each state's record is made
// from m2v's translation and plans and NVK's stage compile, with no pipeline, command buffer or capture: every state the pack holds
// a record for must come out byte for byte the same (its barrier form made as on a device without the pixel interlock), every one
// it set aside refused. The states are pack/probe.mm's: Core Animation's lines, another framework's (lib=, under PROBE_LIB_ROOT)
// and, with PROBE_OWN_LIB, the raw path's own.
// usage: directcheck <nvk dylib> <cache dir> <default.metallib> <corpus.log> <pack.bin>
//   DIRECTCHECK_MET=<file>: appends the key hash and form of each pack record made the same (a sweep over every bucket checks
//   that it met every record of the pack)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <chrono>
#include <cstdio>
#include <set>
#include "../pack/probe_states.h"
#include "../runtime/RawDirect.h"
#include "NativeBridgeMetalFrontend.h"
#include "TwoPlaneStandIn.h"
#include "record_compare.h"

namespace {

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

// probe.mm's tagged textures of the raw path's own states that read codes (pack/xr.metal) or the two-plane stand-in
MTLPixelFormat texturesOf(const std::string &line)
{
    const std::pair<const char *, MTLPixelFormat> formats[] = {{"f xr_bgr10_frag", MTLPixelFormatR32Uint},
                                                               {"f xr_bgra10_frag", MTLPixelFormatRGBA16Uint},
                                                               {"f xr_planes_frag", MTLPixelFormatRG32Uint},
                                                               {"f two_plane_store", MTLPixelFormatRGBA16Float}};
    for (const auto &[fragment, format] : formats)
        if (line.find(std::string("| ") + fragment) != std::string::npos)
            return format;
    return MTLPixelFormatBGRA8Unorm;
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 6) {
            fprintf(stderr, "usage: directcheck <nvk dylib> <cache dir> <default.metallib> <corpus.log> <pack.bin>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        raw::Pack pack;
        std::string err;
        if (!dev || !pack.load(argv[5], 0, err)) {
            fprintf(stderr, "directcheck: %s\n", dev ? err.c_str() : nmb_last_error());
            return 1;
        }
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        NSString *text = [NSString stringWithContentsOfFile:@(argv[4]) encoding:NSUTF8StringEncoding error:nil];
        if (!qc || !text) {
            fprintf(stderr, "directcheck: library or corpus unread\n");
            return 1;
        }
        id<MTLLibrary> own = nil, standIn = nil;
        if (const char *path = getenv("PROBE_OWN_LIB"); path && *path) {
            own = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
            dispatch_data_t data = dispatch_data_create(kTwoPlaneStandInLibrary, sizeof kTwoPlaneStandInLibrary, nullptr,
                                                        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            standIn = own ? [dev newLibraryWithData:data error:&error] : nil;
            if (!own || !standIn) {
                fprintf(stderr, "directcheck: own library: %s\n", error.localizedDescription.UTF8String);
                return 1;
            }
        }
        raw::Direct direct(pack);
        FILE *met = getenv("DIRECTCHECK_MET") ? fopen(getenv("DIRECTCHECK_MET"), "a") : nullptr;
        uint32_t states = 0, same = 0, barriers = 0, refused = 0, bad = 0;
        double ms = 0;
        std::set<std::string> seen;
        std::map<std::string, uint32_t> differences;
        for (const std::string &marked : probe::corpusLines(text, own != nil)) {
            probe::State s;
            const bool ours = marked.rfind("@O ", 0) == 0, framework = marked.rfind("@L ", 0) == 0;
            const size_t pathEnd = framework ? marked.find(' ', 3) : 0;
            const std::string line = ours ? marked.substr(3) : framework ? marked.substr(pathEnd + 1) : marked;
            id<MTLLibrary> library = !ours ? qc : line.find("| f two_plane_") != std::string::npos ? standIn : own;
            if (framework && !(library = probe::frameworkLibrary(dev, marked.substr(3, pathEnd - 3))))
                continue;
            if (!probe::parseState(library, line, s))
                continue;
            if (framework)
                s.key += " | l " + probe::frameworkName(marked.substr(3, pathEnd - 3));
            while (!s.key.empty() && s.key.back() == ' ')
                s.key.pop_back();
            if (!seen.insert(s.key).second)
                continue;
            states++;
            const MTLPixelFormat textures = texturesOf(line);
            const raw::PackPipeline *pp = pack.find(raw::packKeyHash(s.key));
            const bool barrier = pp && (pp->flags & raw::PipelineBarrierForm);
            raw::RuntimeRecord rec;
            std::string why;
            const auto began = std::chrono::steady_clock::now();
            const raw::Compiler::Outcome outcome = direct.make(s.d, s.key, rec, why, textures);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            std::string verdict;
            if (outcome == raw::Compiler::Outcome::Failed) {
                verdict = " failed (" + why + ")";
            } else if (pp) {
                verdict = outcome == raw::Compiler::Outcome::Made ? recordcompare::differences(pack, *pp, rec)
                                                                  : " refused (" + why + ")";
                (barrier ? barriers : same) += verdict.empty();
                if (met && verdict.empty())
                    fprintf(met, "%016llx %u\n", (unsigned long long)pp->keyHash, pp->flags & raw::PipelineBarrierForm);
            } else {
                verdict = outcome == raw::Compiler::Outcome::Made ? " made, and the pack set it aside" : "";
                refused += verdict.empty();
            }
            if (!verdict.empty()) {
                bad++;
                differences[verdict.substr(0, verdict.find('['))]++;
                printf("DIFFERS%s%s: %s\n", barrier ? " (barrier)" : "", verdict.c_str(), s.key.substr(0, 160).c_str());
            }
        }
        if (met)
            fclose(met);
        for (const auto &[what, n] : differences)
            printf("  %u x%s\n", n, what.c_str());
        printf("directcheck: %u states made directly: %u records the same as the pack's, %u the same as its barrier forms, "
               "%u set aside and refused, %u differ; %.1f ms a state\n",
               states, same, barriers, refused, bad, states ? ms / states : 0.0);
        return bad || !states ? 1 : 0;
    }
}
