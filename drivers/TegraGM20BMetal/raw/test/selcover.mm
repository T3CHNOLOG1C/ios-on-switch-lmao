// SPDX-License-Identifier: MIT
// selcover: who answers every selector a kind's system code sends to a Metal object (drivers/NativeMetal/abi/kinds/<kind>.json,
// "sent") on the object this process's raw device hands out:
//   raw     a method of the facade's class (the device: of the raw device class itself, not one it inherits);
//   zero    the facade answers it zero (answerZero), as m2v's object would: neither class has a method for it;
//   m2v     m2v answers it: the facade's twin or the m2v object it carries (forwarded to it, or recorded for m2v's replay),
//           or, on the device, the m2v device class the raw device class inherits;
//   m2vobj  the device hands out m2v's own object for the protocol (argument encoders, events, fences, binary archives);
//   base    the device only: _MTLDevice's own method on the iPad (the kind's table, "base"), which the host does not have
//           (NMB_DEVICE_BASE_NSOBJECT);
//   none    nothing answers it (no object of the protocol here, or a device selector no class has).
// Every line that is not raw or zero is a gap. Gaps print per kind and object; the run fails on a gap the baseline lacks and on a
// baseline line that is no longer one. Then every zero-argument selector the facade answers zero is sent to its object: the
// rates line must count each one answered zero and none forwarded to a twin (tw=../f0).
// usage: selcover <pack> <cache> <nvk dylib> <default.metallib> <baseline> <kinds/<kind>.json>...
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

namespace {

void missing(const char *, const char *)
{}

// Whether `cls` answers `sel` with a method of its own or an ancestor's below NSObject
bool answers(Class cls, SEL sel)
{
    Method m = cls ? class_getInstanceMethod(cls, sel) : nullptr;
    if (!m)
        return false;
    Method root = class_getInstanceMethod([NSObject class], sel);
    return !root || method_getImplementation(root) != method_getImplementation(m);
}

bool facadeClass(Class cls)
{
    const char *name = class_getName(cls);
    return !strncmp(name, "RM", 2) || !strcmp(name, "RawGM20BMetalDevice");
}

struct Object {
    id object;
    Class twin;  // the m2v class that answers what the facade's class forwards (nil: none)
};

Class twinClassOf(id object)
{
    if (Ivar carried = class_getInstanceVariable(object_getClass(object), "_twin"))
        if (id twin = object_getIvar(object, carried))
            return object_getClass(twin);
    if (object_getClass(object) == NSClassFromString(@"RMCommandBuffer"))
        return NSClassFromString(@"NBCommandBuffer");
    Class meta = object_getClass(object);
    return [meta respondsToSelector:@selector(twinClass)] ? [meta performSelector:@selector(twinClass)] : nil;
}

struct Objects {
    std::map<std::string, Object> byProtocol;

    void put(const char *protocol, id object, Class twin = nil)
    {
        byProtocol[protocol] = {object, object ? (twin ?: twinClassOf(object)) : nil};
    }

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
        put("MTLDevice", dev);
        put("MTLCommandQueue", queue);
        put("MTLCommandBuffer", cb);
        put("MTLRenderCommandEncoder", render);
        put("MTLBlitCommandEncoder", blit);
        put("MTLComputeCommandEncoder", compute);
        put("MTLTexture", texture);
        put("MTLBuffer", [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]);
        put("MTLSamplerState", [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]]);
        put("MTLDepthStencilState", [dev newDepthStencilStateWithDescriptor:ds]);
        put("MTLLibrary", library);
        put("MTLFunction", function);
        put("MTLHeap", [dev newHeapWithDescriptor:hd]);
        put("MTLSharedEvent", [dev newSharedEvent]);
        put("MTLFence", [dev newFence]);
        put("MTLArgumentEncoder", [dev newArgumentEncoderWithArguments:@[ argument ]]);
        put("MTLBinaryArchive", [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error]);
        // Made only from a pack state or a kernel the host corpus lacks: the facade's classes, with the m2v class each carries
        put("MTLRenderPipelineState", class_createInstance(NSClassFromString(@"RMPipeline"), 0));
        put("MTLComputePipelineState", class_createInstance(NSClassFromString(@"RMComputeState"), 0),
            NSClassFromString(@"NBComputePipeline"));
        bool all = true;
        for (const auto &[protocol, o] : byProtocol)
            if (!o.object) {
                fprintf(stderr, "selcover: no %s\n", protocol.c_str());
                all = false;
            }
        return all;
    }
};

const char *who(const Object &o, SEL sel, NSDictionary *base)
{
    if (!o.object)
        return "none";
    Class cls = object_getClass(o.object);
    if (!facadeClass(cls))
        return "m2vobj";
    if (strcmp(class_getName(cls), "RawGM20BMetalDevice") == 0) {
        Class inherited = class_getSuperclass(cls);
        Method own = class_getInstanceMethod(cls, sel);
        if (own && (!answers(inherited, sel) ||
                    method_getImplementation(own) != class_getMethodImplementation(inherited, sel)))
            return "raw";
        if (answers(inherited, sel))
            return "m2v";
        return base[NSStringFromSelector(sel)] ? "base" : "none";
    }
    if (answers(cls, sel))
        return "raw";
    return o.twin && [o.twin instancesRespondToSelector:sel] ? "m2v" : "zero";
}

// The rates line after one more command buffer (the line needs one), its counters taken
void rates(id<MTLCommandQueue> queue, char *line, size_t size)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    [cb commit];
    [cb waitUntilCompleted];
    *line = 0;
    raw_metal_rates(line, size);
}

// Sends each zero-argument selector the facade answers zero to its object; false unless the rates line counts them all answered
// zero and no twin forward
bool sendZeros(const std::map<std::string, Object> &objects, const std::set<std::pair<std::string, std::string>> &zeros)
{
    char line[4096];
    id<MTLCommandQueue> queue = objects.at("MTLCommandQueue").object;
    rates(queue, line, sizeof line);
    unsigned sent = 0;
    for (const auto &[protocol, selector] : zeros) {
        const Object &o = objects.at(protocol);
        SEL sel = sel_registerName(selector.c_str());
        if (strchr(selector.c_str(), ':') || class_getInstanceMethod(object_getClass(o.object), sel))
            continue;
        NSMethodSignature *signature = [o.object methodSignatureForSelector:sel];
        if (!signature) {
            printf("NO SIGNATURE -[%s %s]\n", object_getClassName(o.object), selector.c_str());
            return false;
        }
        NSInvocation *invocation = [NSInvocation invocationWithMethodSignature:signature];
        invocation.selector = sel;
        [invocation invokeWithTarget:o.object];
        sent++;
    }
    rates(queue, line, sizeof line);
    const char *zero = strstr(line, " zero="), *twins = strstr(line, " tw="),
               *forwards = twins ? strstr(twins, "/f") : nullptr;
    const unsigned answered = zero ? unsigned(atoi(zero + 6)) : ~0u;
    const unsigned forwarded = forwards ? unsigned(atoi(forwards + 2)) : ~0u;
    printf("sent %u zero-argument selectors the facade answers zero: %u answered zero, %u forwarded to a twin\n", sent,
           answered, forwarded);
    return answered == sent && forwarded == 0;
}

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
        std::set<std::pair<std::string, std::string>> zeros;
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
            std::map<std::string, unsigned> totals;
            for (NSString *protocol in [sent.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
                const auto it = objects.byProtocol.find(protocol.UTF8String);
                const Object o = it == objects.byProtocol.end() ? Object{nil, nil} : it->second;
                unsigned open = 0;
                for (NSString *selector in [sent[protocol].allKeys sortedArrayUsingSelector:@selector(compare:)]) {
                    const char *how = who(o, sel_registerName(selector.UTF8String),
                                          [protocol isEqualToString:@"MTLDevice"] ? base : nil);
                    totals[how]++;
                    if (!strcmp(how, "zero"))
                        zeros.insert({protocol.UTF8String, selector.UTF8String});
                    if (!strcmp(how, "raw") || !strcmp(how, "zero"))
                        continue;
                    gaps.insert(std::string(name.UTF8String) + " " + protocol.UTF8String + " " + selector.UTF8String +
                                " " + how);
                    open++;
                }
                if (open)
                    printf("%s %s (%s): %u of %lu not raw's\n", name.UTF8String, protocol.UTF8String,
                           o.object ? object_getClassName(o.object) : "-", open, (unsigned long)sent[protocol].count);
            }
            printf("%s:", name.UTF8String);
            for (const auto &[how, count] : totals)
                printf(" %s %u", how.c_str(), count);
            printf("\n");
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
        const bool zerosOk = sendZeros(objects.byProtocol, zeros);
        const bool pass = !added && !gone && zerosOk;
        printf("%s: selcover (%u new gaps, %u baseline lines answered now%s)\n", pass ? "PASS" : "FAIL", added, gone,
               zerosOk ? "" : ", zero answers miscounted");
        return pass ? 0 : 1;
    }
}
