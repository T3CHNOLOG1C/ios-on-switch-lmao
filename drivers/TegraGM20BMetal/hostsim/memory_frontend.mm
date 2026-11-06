// Host check of what a Metal client's GPU memory costs it (NativeBridgeMetalFrontend.mm over the bridge over NVK over the mock
// GM20B), in the mock's BOs as iOS charges them (mock_iokit.h): at device open, for a widget-like client (per surface a 520x800
// BGRA8 render target and a Depth32Float_Stencil8 memoryless depth/stencil, as Core Animation draws a widget), and once the
// client let its resources go and the GPU was idle (GpuMemoryRelease.h).
// usage: memory_frontend <libvulkan_nouveau.dylib> <cache dir> [surfaces]; one line per step, then an ok/FAIL line per check;
// exits non-zero on a FAIL.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "mock_iokit.h"

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static int failures;

// The mock's BOs, and what the device holds of them (nmb_device_memory: its blocks, then its memoryless backings).
struct Step {
    mock_memory bos;
    uint64_t held, backings;
};

static Step step(const char *what)
{
    Step s = {};
    mock_iokit_memory(&s.bos);
    uint64_t device[3] = {};
    native_bridge_metal_memory(device);
    s.held = device[0];
    s.backings = device[2];
    const mock_memory &m = s.bos;
    printf("%-26s iokit_mapped %6llu KiB  cached %6llu KiB  coherent %6llu KiB  BoAllocs %3llu+%-3llu  m2v holds %6llu "
           "KiB, %llu memoryless backings\n",
           what, (unsigned long long)(m.mapped.cached >> 10), (unsigned long long)(m.live.cached >> 10),
           (unsigned long long)(m.live.coherent >> 10), (unsigned long long)m.allocs[0],
           (unsigned long long)m.allocs[1], (unsigned long long)(s.held >> 10), (unsigned long long)s.backings);
    return s;
}

static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    failures += !ok;
}

static id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, MTLStorageMode storage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:520
                                                                                height:800
                                                                             mipmapped:NO];
    d.storageMode = storage;
    d.usage = MTLTextureUsageRenderTarget | (storage == MTLStorageModeMemoryless ? 0 : MTLTextureUsageShaderRead);
    return [device newTextureWithDescriptor:d];
}

static bool render(id<MTLCommandQueue> queue, NSArray *targets, NSArray *depths, NSArray *scratch)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (NSUInteger i = 0; i < targets.count; i++) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = targets[i];
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.depthAttachment.texture = depths[i];
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.stencilAttachment.texture = depths[i];
        pass.stencilAttachment.loadAction = MTLLoadActionClear;
        pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
        // Two memoryless colors of one shape in one pass: their shared backing cannot serve both.
        for (NSUInteger j = 0; j < scratch.count; j++) {
            pass.colorAttachments[1 + j].texture = scratch[j];
            pass.colorAttachments[1 + j].loadAction = MTLLoadActionClear;
            pass.colorAttachments[1 + j].storeAction = MTLStoreActionDontCare;
        }
        [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
    }
    [cb commit];
    [cb waitUntilCompleted];
    return cb.status == MTLCommandBufferStatusCompleted;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 3)
            return 2;
        const int surfaces = argc > 3 ? atoi(argv[3]) : 4;
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        setenv("M2V_BLOCK_MIB", "4", 0);  // a client outside the render server (TegraGM20BMetalPlugin.mm)
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!device) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        step("device open");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        step("command queue");
        @autoreleasepool {
            NSMutableArray *targets = [NSMutableArray array], *depths = [NSMutableArray array];
            for (int i = 0; i < surfaces; i++)
                [targets addObject:texture(device, MTLPixelFormatBGRA8Unorm, MTLStorageModePrivate)];
            step("render targets made");
            id<MTLTexture> depth = texture(device, MTLPixelFormatDepth32Float_Stencil8, MTLStorageModeMemoryless);
            check(depth != nil, "a memoryless Depth32Float_Stencil8 texture is made");
            if (!depth)
                return 1;
            [depths addObject:depth];
            const Step first = step("first memoryless depth");
            for (int i = 1; i < surfaces; i++)
                [depths addObject:texture(device, MTLPixelFormatDepth32Float_Stencil8, MTLStorageModeMemoryless)];
            const Step all = step("every memoryless depth");
            check(all.bos.allocs[0] == first.bos.allocs[0] && all.backings == 1,
                  "memoryless textures of one size and format share one backing");
            NSArray *scratch = @[
                texture(device, MTLPixelFormatBGRA8Unorm, MTLStorageModeMemoryless),
                texture(device, MTLPixelFormatBGRA8Unorm, MTLStorageModeMemoryless)
            ];
            check(render(queue, targets, depths, @[]), "passes over the shared depth/stencil complete");
            check(render(queue, targets, depths, scratch),
                  "a pass with two memoryless colors of one backing completes");
            step("surfaces drawn");
        }
        const Step released = step("surfaces released");
        // GpuMemoryRelease's idle deadline (1 s after the last command buffer completed) with its leeway.
        usleep(1500 * 1000);
        const Step idle = step("idle");
        // What the device held in cached BOs (its coherent ones are the mock's coherent bytes that went) left the process's
        // iokit_mapped with them; NVK keeps its own.
        const uint64_t coherent = released.bos.live.coherent - idle.bos.live.coherent;
        const uint64_t dropped = released.bos.mapped.cached - idle.bos.mapped.cached;
        check(idle.held == 0 && released.held >= coherent && dropped >= released.held - coherent,
              "the idle blocks go back to the kernel");
        mock_iokit_report();
    }
    return failures ? 1 : 0;
}
