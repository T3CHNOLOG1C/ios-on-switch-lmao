// Host check of QuartzCore's render pipelines through the Metal frontend (NativeBridgeMetalFrontend.mm, the plugin's own
// code) over the bridge over NVK over the mock GM20B: sheng's 24 authored recipes (drivers/NativeMetal/bridge/
// quartzcore_cache.mm: the 22 _vert_/_frag_ pairs with QuartzCore's 64-byte vertex, path_blit's three attachments, the two
// RGBA16Float blur pairs), every fragment function that reads its framebuffer, and the uber shader under a few constant sets.
// Each pipeline is made with newRenderPipelineStateWithDescriptor: and drawn three times in one pass, then committed.
// usage: qc_frontend <libvulkan_nouveau.dylib> <cache dir> <QuartzCore default.metallib>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
extern "C" void mock_iokit_report(void);

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4)
            return 2;
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        if (!qc) {
            printf("library: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        NSArray<NSString *> *names = qc.functionNames;
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        id<MTLBuffer> zeros = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
        NSMutableDictionary<NSNumber *, id<MTLTexture>> *targets = [NSMutableDictionary dictionary];
        auto target = [&](MTLPixelFormat f) {
            if (!targets[@(f)]) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                              width:64
                                                                                             height:64
                                                                                          mipmapped:NO];
                td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                targets[@(f)] = [dev newTextureWithDescriptor:td];
            }
            return targets[@(f)];
        };
        unsigned made = 0, refused = 0, drawn = 0, failed = 0;
        auto attempt = [&](NSString *label, MTLRenderPipelineDescriptor *d) {
            NSError *e = nil;
            id<MTLRenderPipelineState> state = [dev newRenderPipelineStateWithDescriptor:d error:&e];
            if (!state) {
                ++refused;
                printf("REFUSED %s: %s\n", label.UTF8String, e.localizedDescription.UTF8String);
                return;
            }
            ++made;
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            for (NSUInteger i = 0; i < 8; ++i) {
                MTLPixelFormat f = d.colorAttachments[i].pixelFormat;
                if (f == MTLPixelFormatInvalid)
                    continue;
                pass.colorAttachments[i].texture = target(f);
                pass.colorAttachments[i].loadAction = MTLLoadActionClear;
                pass.colorAttachments[i].storeAction = MTLStoreActionStore;
            }
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
            [enc setRenderPipelineState:state];
            for (NSUInteger i = 0; i < 8; ++i) {
                [enc setVertexBuffer:zeros offset:0 atIndex:i];
                [enc setFragmentBuffer:zeros offset:0 atIndex:i];
            }
            for (int k = 0; k < 3; ++k)
                [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.error) {
                ++failed;
                printf("DRAW FAILED %s: %s\n", label.UTF8String, cb.error.localizedDescription.UTF8String);
            } else
                ++drawn;
        };
        // QuartzCore's own vertex layout (sheng's recipes): float4 position, float2, float2, float4, float; stride 64.
        auto qcVertex = [](MTLRenderPipelineDescriptor *d, bool blur) {
            MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
            if (blur) {
                MTLVertexFormat f[] = {MTLVertexFormatFloat4, MTLVertexFormatFloat2, MTLVertexFormatInvalid,
                                       MTLVertexFormatFloat4};
                NSUInteger o[] = {0, 16, 0, 24};
                for (int i = 0; i < 4; ++i)
                    if (f[i]) {
                        v.attributes[i].format = f[i];
                        v.attributes[i].offset = o[i];
                        v.attributes[i].bufferIndex = 0;
                    }
                v.layouts[0].stride = 40;
            } else {
                MTLVertexFormat f[] = {MTLVertexFormatFloat4, MTLVertexFormatFloat2, MTLVertexFormatFloat2,
                                       MTLVertexFormatFloat4, MTLVertexFormatFloat};
                NSUInteger o[] = {0, 16, 24, 32, 48};
                for (int i = 0; i < 5; ++i) {
                    v.attributes[i].format = f[i];
                    v.attributes[i].offset = o[i];
                    v.attributes[i].bufferIndex = 0;
                }
                v.layouts[0].stride = 64;
            }
            d.vertexDescriptor = v;
        };
        // std_vert1_lp{f,h}'s inputs (its AIR): float4 position, float2 texcoord at 1, float4 (lpf) or half4 (lph) color at 3.
        auto std1Vertex = [](MTLRenderPipelineDescriptor *d, bool half) {
            MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];
            v.attributes[0].format = MTLVertexFormatFloat4;
            v.attributes[0].offset = 0;
            v.attributes[1].format = MTLVertexFormatFloat2;
            v.attributes[1].offset = 16;
            v.attributes[3].format = half ? MTLVertexFormatHalf4 : MTLVertexFormatFloat4;
            v.attributes[3].offset = 24;
            v.layouts[0].stride = 40;
            d.vertexDescriptor = v;
        };
        unsigned recipes = 0;
        for (NSString *name in names) {
            NSRange r = [name rangeOfString:@"_vert_"];
            if (r.location == NSNotFound)
                continue;
            NSString *fragment = [name stringByReplacingCharactersInRange:r withString:@"_frag_"];
            if (![names containsObject:fragment])
                continue;
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [qc newFunctionWithName:name];
            d.fragmentFunction = [qc newFunctionWithName:fragment];
            d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            if ([name hasPrefix:@"path_blit_"]) {
                d.colorAttachments[1].pixelFormat = MTLPixelFormatBGRA8Unorm;
                d.colorAttachments[2].pixelFormat = MTLPixelFormatR32Float;
            }
            qcVertex(d, false);
            ++recipes;
            attempt([NSString stringWithFormat:@"%@+%@", name, fragment], d);
        }
        for (NSString *suffix in @[ @"lpf", @"lph" ]) {
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [qc newFunctionWithName:[@"downsample_blur_vert_" stringByAppendingString:suffix]];
            d.fragmentFunction = [qc newFunctionWithName:[@"narrow_blur_7_frag_" stringByAppendingString:suffix]];
            d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
            qcVertex(d, true);
            ++recipes;
            attempt([@"blur " stringByAppendingString:suffix], d);
        }
        printf("sheng recipes: %u\n", recipes);
        // Every filter that reads the framebuffer, over std_vert1.
        NSArray *fetching = @[
            @"p3_clamp", @"edr_clamp", @"color_edr", @"alpha_linear_premultiply", @"edr_scale", @"harmony",
            @"display_overdrive_linearize", @"display_overdrive_srgb_sampler", @"luma_boost", @"inplace_copy"
        ];
        for (NSString *suffix in @[ @"lpf", @"lph" ]) {
            for (NSString *base in fetching) {
                MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
                d.vertexFunction = [qc newFunctionWithName:[@"std_vert1_" stringByAppendingString:suffix]];
                d.fragmentFunction = [qc newFunctionWithName:[NSString stringWithFormat:@"%@_%@", base, suffix]];
                if (!d.fragmentFunction) {
                    printf("no %s\n", base.UTF8String);
                    continue;
                }
                d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                std1Vertex(d, [suffix isEqualToString:@"lph"]);
                attempt([NSString stringWithFormat:@"fetch %@_%@", base, suffix], d);
            }
        }
        // The uber shader under constant sets: all false, then each bool alone true.
        id<MTLFunction> probe = [qc newFunctionWithName:@"fixed_frag_lph_cph"];
        NSMutableArray<NSString *> *bools = [NSMutableArray array];
        for (NSString *k in probe.functionConstantsDictionary)
            if (probe.functionConstantsDictionary[k].type == MTLDataTypeBool)
                [bools addObject:k];
        [bools sortUsingSelector:@selector(compare:)];
        NSMutableArray *sets = [NSMutableArray arrayWithObject:@""];
        [sets addObjectsFromArray:bools];
        unsigned uber = 0;
        id<MTLFunction> vprobe = [qc newFunctionWithName:@"fixed_vert_lph_gen"];
        auto values = [](id<MTLFunction> f, NSString *on) {
            MTLFunctionConstantValues *v = [MTLFunctionConstantValues new];
            for (NSString *k in f.functionConstantsDictionary) {
                MTLFunctionConstant *c = f.functionConstantsDictionary[k];
                uint8_t bytes[16] = {};
                if (c.type == MTLDataTypeBool)
                    bytes[0] = [k isEqualToString:on];
                [v setConstantValue:bytes type:c.type withName:k];
            }
            return v;
        };
        for (NSString *on in sets) {
            NSError *e = nil;
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = [qc newFunctionWithName:@"fixed_vert_lph_gen"
                                        constantValues:values(vprobe, on)
                                                 error:&e];
            d.fragmentFunction = [qc newFunctionWithName:@"fixed_frag_lph_cph"
                                          constantValues:values(probe, on)
                                                   error:&e];
            if (!d.vertexFunction || !d.fragmentFunction) {
                printf("uber %s: %s\n", on.UTF8String, e.localizedDescription.UTF8String);
                ++refused;
                continue;
            }
            d.colorAttachments[0].pixelFormat =
                MTLPixelFormatBGRA8Unorm;  // the uber vertex function pulls its vertices
            if (on.length && [on containsString:@"point"])
                d.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
            ++uber;
            attempt([@"uber " stringByAppendingString:on.length ? on : @"(none)"], d);
        }
        printf("uber sets: %u (%lu bool constants)\n", uber, (unsigned long)bools.count);
        printf("RESULT made=%u refused=%u drawn=%u draw_failed=%u\n", made, refused, drawn, failed);
        mock_iokit_report();
    }
    return 0;
}
