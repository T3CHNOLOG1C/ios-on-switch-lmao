// SPDX-License-Identifier: MIT
// probeloop: the runtime's probe (runtime/RawProbe.h) of a corpus' Core Animation states, round after round in one process, as
// a process that completes thousands of states does. Each probe captures four m2v command buffers it never commits; what they
// allocate (a descriptor pool each, a BO) goes when they do, so the BOs held after the last round are those held after the
// first, past the mock's first 65536 allocations (its handles never repeat, as the kext's; its slots do). Then a probe whose
// descriptor pool's BO the kernel refuses: NVK's nvk_CreateDescriptorPool fails, the probe fails (a refusal of this process's,
// never kept as the state's), and the next one comes out as before.
// usage: probeloop <nvk dylib> <cache dir> <default.metallib> <corpus.log> <pack.bin> [rounds]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstdio>
#include <vector>
#include "../pack/probe_states.h"
#include "../runtime/RawProbe.h"
#include "NativeBridgeMetalFrontend.h"
#include "TegraGM20BABI.h"
#include "bridge/NativeMetalObjects.h"

extern "C" unsigned long mock_iokit_calls(unsigned method);
extern "C" void mock_iokit_held(uint64_t *allocated, uint64_t *imported);
extern "C" void mock_iokit_fail_next(unsigned method, unsigned count, int kr);

namespace {

void missing(const char *, const char *)
{}

struct Held {
    unsigned long bos;
    uint64_t bytes;
};

Held held()
{
    uint64_t allocated = 0, imported = 0;
    mock_iokit_held(&allocated, &imported);
    return {mock_iokit_calls(TegraGM20BMethodBoAlloc) - mock_iokit_calls(TegraGM20BMethodBoFree), allocated};
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 6) {
            fprintf(stderr,
                    "usage: probeloop <nvk dylib> <cache dir> <default.metallib> <corpus.log> <pack.bin> [rounds]\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        raw::Pack pack;
        std::string err;
        if (!dev || !pack.load(argv[5], 0, err)) {
            fprintf(stderr, "probeloop: %s\n", dev ? err.c_str() : nmb_last_error());
            return 1;
        }
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        NSString *text = [NSString stringWithContentsOfFile:@(argv[4]) encoding:NSUTF8StringEncoding error:nil];
        NSRegularExpression *lib = [NSRegularExpression regularExpressionWithPattern:@" lib=\\S+" options:0 error:nil];

        struct Twin {
            id<MTLRenderPipelineState> state;
            MTLRenderPipelineDescriptor *descriptor;
            std::string key;
        };

        std::vector<Twin> twins;
        for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
            if (![l hasPrefix:@"R "] || ![l containsString:@" ok=1 "])
                continue;
            NSString *plain = [lib stringByReplacingMatchesInString:l
                                                            options:0
                                                              range:NSMakeRange(0, l.length)
                                                       withTemplate:@""];
            probe::State s;
            if (!probe::parseState(qc, plain.UTF8String, s))
                continue;
            if (id<MTLRenderPipelineState> twin = [dev newRenderPipelineStateWithDescriptor:s.d error:&error])
                twins.push_back({twin, s.d, s.key});
        }
        if (!qc || twins.empty()) {
            fprintf(stderr, "probeloop: no states\n");
            return 1;
        }
        raw::Probe probe(dev, pack);
        auto make = [&](const Twin &t, std::string &why) {
            @autoreleasepool {
                raw::RuntimeRecord out;
                return probe.make(t.state, t.descriptor, t.key, out, why);
            }
        };
        // the first rounds' transient BOs (the probe's payloads, m2v's first pools) settle by the warm-up's end
        const unsigned rounds = argc > 6 ? unsigned(atoi(argv[6])) : 1100, warm = std::min(10u, rounds / 2);
        unsigned failed = 0;
        Held settled = {};
        raw::Compiler::Outcome usual = raw::Compiler::Outcome::Failed;
        for (unsigned r = 0; r < rounds; r++) {
            for (const Twin &t : twins) {
                std::string why;
                const raw::Compiler::Outcome o = make(t, why);
                failed += o == raw::Compiler::Outcome::Failed;
                if (&t == &twins[0])
                    usual = o;
            }
            if (r == warm)
                settled = held();
        }
        const Held last = held();
        printf("%u rounds of %zu states, %lu BO allocations: %u probes failed; BOs held after round %u %lu (%llu KiB), "
               "after the last %lu (%llu KiB)\n",
               rounds, twins.size(), mock_iokit_calls(TegraGM20BMethodBoAlloc), failed, warm + 1, settled.bos,
               (unsigned long long)(settled.bytes >> 10), last.bos, (unsigned long long)(last.bytes >> 10));
        std::string why;
        mock_iokit_fail_next(TegraGM20BMethodBoAlloc, 1, kIOReturnNoMemory);
        const raw::Compiler::Outcome refused = make(twins[0], why);
        printf("a probe whose descriptor pool the kernel refused: outcome %d (%s)\n", int(refused), why.c_str());
        const raw::Compiler::Outcome next = make(twins[0], why);
        printf("the next probe: outcome %d (%s), as before %d\n", int(next), why.c_str(), int(usual));
        const bool ok = !failed && last.bos <= settled.bos && last.bytes <= settled.bytes &&
                        refused == raw::Compiler::Outcome::Failed && next == usual;
        printf("%s: probeloop\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
}
