// SPDX-License-Identifier: MIT
// selcover: every selector a kind's system code sends to a Metal object (drivers/NativeMetal/abi/kinds/<kind>.json, "sent") is
// answered by the object this process's raw device hands out: a method of the facade's class or of the frontend's, or of the m2v
// object the facade carries (RMTwinObject: compute states, depth-stencil states). A gap is a selector that reaches
// forwardingTargetForSelector: (an m2v twin made for it, or a command buffer recorded for m2v whole) or, on the device, the base
// class: _MTLDevice's own method on the iPad, which the kind's table names ("base"); here the base is NSObject
// (NMB_DEVICE_BASE_NSOBJECT), so such a selector shows unanswered as on the device it would answer _MTLDevice's default.
// Gaps print per kind and object; the run fails on a gap the baseline lacks and on a baseline line that is no longer a gap.
// usage: selcover <pack> <cache> <nvk dylib> <default.metallib> <baseline> <kinds/<kind>.json>...
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

namespace {

void missing(const char *, const char *)
{}

// Whether `object`'s class answers `sel` with a method of its own or an ancestor's below NSObject
bool answers(id object, SEL sel)
{
    Method m = class_getInstanceMethod(object_getClass(object), sel);
    if (!m)
        return false;
    Method root = class_getInstanceMethod([NSObject class], sel);
    return !root || method_getImplementation(root) != method_getImplementation(m);
}

struct Objects {
    std::map<std::string, id> byProtocol;

    bool make(id<MTLDevice> dev, NSString *metallib)
    {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:64
                                                                                     height:64
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> texture = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        id<MTLRenderCommandEncoder> render = [cb renderCommandEncoderWithDescriptor:pass];
        [render endEncoding];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit endEncoding];
        id<MTLComputeCommandEncoder> compute = [cb computeCommandEncoder];
        [compute endEncoding];
        NSError *error = nil;
        id<MTLLibrary> library = [dev newLibraryWithURL:[NSURL fileURLWithPath:metallib] error:&error];
        id<MTLFunction> function =
            library.functionNames.count ? [library newFunctionWithName:library.functionNames[0]] : nil;
        MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
        ds.depthCompareFunction = MTLCompareFunctionLess;
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.size = 1 << 20;
        MTLArgumentDescriptor *argument = [MTLArgumentDescriptor argumentDescriptor];
        argument.dataType = MTLDataTypePointer;
        byProtocol = {
            {"MTLDevice", dev},
            {"MTLCommandQueue", queue},
            {"MTLCommandBuffer", cb},
            {"MTLRenderCommandEncoder", render},
            {"MTLBlitCommandEncoder", blit},
            {"MTLComputeCommandEncoder", compute},
            {"MTLTexture", texture},
            {"MTLBuffer", [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]},
            {"MTLSamplerState", [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]]},
            {"MTLDepthStencilState", [dev newDepthStencilStateWithDescriptor:ds]},
            {"MTLLibrary", library},
            {"MTLFunction", function},
            {"MTLHeap", [dev newHeapWithDescriptor:hd]},
            {"MTLSharedEvent", [dev newSharedEvent]},
            {"MTLFence", [dev newFence]},
            {"MTLArgumentEncoder", [dev newArgumentEncoderWithArguments:@[ argument ]]},
            {"MTLBinaryArchive", [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error]},
        };
        // Made only from a pack state or a compute function the host corpus lacks: the classes the facade answers them with
        if (Class c = NSClassFromString(@"RMPipeline"))
            byProtocol["MTLRenderPipelineState"] = class_createInstance(c, 0);
        if (Class c = NSClassFromString(@"NBComputePipeline"))
            byProtocol["MTLComputePipelineState"] = class_createInstance(c, 0);
        bool all = true;
        for (const auto &[protocol, object] : byProtocol)
            if (!object) {
                fprintf(stderr, "selcover: no %s\n", protocol.c_str());
                all = false;
            }
        return all;
    }
};

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 7) {
            fprintf(stderr,
                    "usage: selcover <pack> <cache> <nvk dylib> <default.metallib> <baseline> <kind json>...\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        NSString *why = nil;
        id<MTLDevice> dev = m2v ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        Objects objects;
        if (!dev || !objects.make(dev, @(argv[4]))) {
            fprintf(stderr, "selcover: no device or objects (%s)\n", m2v ? why.UTF8String : nmb_last_error());
            return 1;
        }
        std::set<std::string> gaps, baseline;
        NSString *text = [NSString stringWithContentsOfFile:@(argv[5]) encoding:NSUTF8StringEncoding error:nil];
        for (NSString *line in [text componentsSeparatedByString:@"\n"])
            if (line.length && ![line hasPrefix:@"#"])
                baseline.insert(line.UTF8String);
        for (int k = 6; k < argc; k++) {
            NSDictionary *kind = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[k])]
                                                                 options:0
                                                                   error:nil];
            NSString *name = kind[@"kind"];
            NSDictionary *base = kind[@"MTLDevice"][@"methods"];
            NSDictionary<NSString *, NSDictionary *> *sent = kind[@"sent"];
            unsigned total = 0, answered = 0;
            for (NSString *protocol in [sent.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
                const auto it = objects.byProtocol.find(protocol.UTF8String);
                id object = it == objects.byProtocol.end() ? nil : it->second;
                id carried = [object respondsToSelector:@selector(rmTwin)] &&
                                     [object isKindOfClass:NSClassFromString(@"RMTwinObject")]
                                 ? [object performSelector:@selector(rmTwin)]
                                 : nil;
                unsigned open = 0;
                for (NSString *selector in [sent[protocol].allKeys sortedArrayUsingSelector:@selector(compare:)]) {
                    total++;
                    SEL sel = sel_registerName(selector.UTF8String);
                    if (object && (answers(object, sel) || (carried && answers(carried, sel)))) {
                        answered++;
                        continue;
                    }
                    const char *how = !object                                   ? "none"
                                      : [protocol isEqualToString:@"MTLDevice"] ? (base[selector] ? "base" : "none")
                                                                                : "forward";
                    gaps.insert(std::string(name.UTF8String) + " " + protocol.UTF8String + " " + selector.UTF8String +
                                " " + how);
                    open++;
                }
                if (open)
                    printf("%s %s (%s): %u of %lu unanswered\n", name.UTF8String, protocol.UTF8String,
                           object ? object_getClassName(object) : "-", open, (unsigned long)sent[protocol].count);
            }
            printf("%s: %u selectors sent, %u answered, %u gaps\n", name.UTF8String, total, answered, total - answered);
        }
        unsigned added = 0, gone = 0;
        for (const std::string &g : gaps)
            if (!baseline.count(g)) {
                printf("NEW GAP %s\n", g.c_str());
                added++;
            }
        for (const std::string &b : baseline)
            if (!gaps.count(b)) {
                printf("NO LONGER A GAP (remove it from the baseline) %s\n", b.c_str());
                gone++;
            }
        printf("%s: selcover (%u new gaps, %u baseline lines answered now)\n", added || gone ? "FAIL" : "PASS", added,
               gone);
        return added || gone ? 1 : 0;
    }
}
