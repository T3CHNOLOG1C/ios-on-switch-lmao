// SPDX-License-Identifier: MIT
// raw GM20B path (r7-raw, r8-rawca): test/frame.h's frames through the shipped Metal frontend + m2v + NVK host twin, over the mock GPU (capture/cap.cpp decodes
// every submission when RAWCAP_LOG is set). Also the "current path" encode benchmark: CPU time per frame on the encoding thread.
// usage: nvkframe <nvk dylib> <cache dir> <default.metallib> <corpus.log> <frames> [blurPasses draws uniformBytes]
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <algorithm>
#include <mach/mach.h>
#include "../test/frame.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#ifdef RAW_FACADE  // the same frames through the raw path's Metal facade (runtime/RawMetal.mm) instead of the frontend + m2v + NVK
#include "../runtime/RawMetal.h"
#define FRAME_PATH "raw-facade"
#else
#define FRAME_PATH "nvk"
#endif

extern "C" void pushcount_report(const char *what);
// What Core Animation does with every command buffer it makes: no content protection (setProtectionOptions:, SPI)
static void caCommandBuffer(id<MTLCommandBuffer> cb)
{
    static const SEL protection = NSSelectorFromString(@"setProtectionOptions:");
    if ([(id)cb respondsToSelector:protection]) ((void (*)(id, SEL, uint64_t))objc_msgSend)(cb, protection, 0);
}

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static uint64_t threadNs()
{
    thread_basic_info_data_t t{};
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    mach_port_t self = mach_thread_self();
    thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&t, &n);
    mach_port_deallocate(mach_task_self(), self);
    return ((uint64_t)t.user_time.seconds * 1000000 + t.user_time.microseconds +
            (uint64_t)t.system_time.seconds * 1000000 + t.system_time.microseconds) *
           1000;
}
static MTLFunctionConstantValues *constants(const std::vector<std::string> &items)
{
    MTLFunctionConstantValues *v = [MTLFunctionConstantValues new];
    for (const std::string &item : items) {
        size_t a = item.rfind(':'), b = a == std::string::npos ? a : item.rfind(':', a - 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        std::string name = item.substr(0, b), type = item.substr(b + 1, a - b - 1), hex = item.substr(a + 1);
        uint8_t bytes[64] = {};
        for (size_t i = 0; i + 1 < hex.size() && i / 2 < sizeof bytes; i += 2)
            bytes[i / 2] = (uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16);
        if (name[0] == '#')
            [v setConstantValue:bytes
                           type:(MTLDataType)atoi(type.c_str())
                        atIndex:strtoul(name.c_str() + 1, nullptr, 10)];
        else
            [v setConstantValue:bytes type:(MTLDataType)atoi(type.c_str()) withName:@(name.c_str())];
    }
    return v;
}
static id<MTLFunction> function(id<MTLLibrary> qc, const std::vector<std::string> &stage)
{
    if (stage.empty()) return nil;
    if (stage.size() == 1) return [qc newFunctionWithName:@(stage[0].c_str())];
    NSError *e = nil;
    return [qc newFunctionWithName:@(stage[0].c_str())
                    constantValues:constants(std::vector<std::string>(stage.begin() + 1, stage.end()))
                             error:&e];
}
static MTLRenderPipelineDescriptor *descriptor(id<MTLLibrary> qc, const std::string &line)
{
    size_t bar1 = line.find(" | v "), bar2 = line.find(" | f "), bar3 = line.find(" | ", bar2 + 5);
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor *vd = nil;
    for (const std::string &h : frame::split(line.substr(0, bar1))) {
        if (h.rfind("label=", 0) == 0) {
            if (h != "label=-") d.label = @(h.substr(6).c_str());
        } else if (h.rfind("samples=", 0) == 0)
            d.rasterSampleCount = strtoul(h.c_str() + 8, nullptr, 10);
        else if (h.rfind("topo=", 0) == 0)
            d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)strtoul(h.c_str() + 5, nullptr, 10);
        else if (h[0] == 'c' && isdigit(h[1])) {
            unsigned i, x[9] = {};
            if (sscanf(h.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6,
                       x + 7, x + 8) != 10)
                continue;
            MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
            c.pixelFormat = (MTLPixelFormat)x[0];
            c.writeMask = x[1];
            c.blendingEnabled = x[2];
            c.sourceRGBBlendFactor = (MTLBlendFactor)x[3];
            c.destinationRGBBlendFactor = (MTLBlendFactor)x[4];
            c.rgbBlendOperation = (MTLBlendOperation)x[5];
            c.sourceAlphaBlendFactor = (MTLBlendFactor)x[6];
            c.destinationAlphaBlendFactor = (MTLBlendFactor)x[7];
            c.alphaBlendOperation = (MTLBlendOperation)x[8];
        } else if (h[0] == 'a' && isdigit(h[1])) {
            unsigned i, x[6] = {};
            if (sscanf(h.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5) != 7) continue;
            if (!vd) vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[i].format = (MTLVertexFormat)x[0];
            vd.attributes[i].offset = x[1];
            vd.attributes[i].bufferIndex = x[2];
            vd.layouts[x[2]].stride = x[3];
            vd.layouts[x[2]].stepFunction = (MTLVertexStepFunction)x[4];
            vd.layouts[x[2]].stepRate = x[5];
        }
    }
    d.vertexDescriptor = vd;
    d.vertexFunction = function(qc, frame::split(line.substr(bar1 + 5, bar2 - bar1 - 5)));
    d.fragmentFunction =
        function(qc, frame::split(line.substr(bar2 + 5, (bar3 == std::string::npos ? line.size() : bar3) - bar2 - 5)));
    return d;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 6) {
            fprintf(stderr,
                    "usage: nvkframe <nvk> <cache> <metallib> <corpus> <frames> [blurPasses draws uniformBytes]\n");
            return 2;
        }
        const unsigned frames = atoi(argv[5]);
        frame::Shape shape;
        if (argc > 6) shape.blurPasses = atoi(argv[6]);
        if (argc > 7) shape.draws = atoi(argv[7]);
        if (argc > 8) shape.uniformBytes = atoi(argv[8]);
        if (argc > 9) shape.vertexBytes = atoi(argv[9]);
#ifdef RAW_FACADE
        // argv[1]: the pack. FRAME_FALLBACK=<nvk dylib>: the m2v device under it (the render server's arrangement), else none
        id<MTLDevice> fallback = nil;
        if (const char *nvk = getenv("FRAME_FALLBACK")) {
            setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
            fallback = native_bridge_metal_open(nvk, argv[2], 0, missing);
            if (!fallback) {
                fprintf(stderr, "NO M2V DEVICE: %s\n", nmb_last_error());
                return 1;
            }
        }
        NSString *why = nil;
        id<MTLDevice> dev = raw_metal_device_open(argv[1], 0, fallback, &why);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", why.UTF8String);
            return 1;
        }
#else
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
#endif
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        std::vector<frame::State> all = frame::states(argv[4]);
        std::vector<id<MTLRenderPipelineState>> uber, blur, extra;
        for (auto &s : all) {
            if (s.extra) {
                if (id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:descriptor(qc, s.line)
                                                                                       error:&error]) {
                    extra.push_back(p);
                    printf("STATE extra %s\n", s.key.c_str());
                }
                continue;
            }
            if (!s.uber && !s.blur) continue;
            if (s.uber && uber.size() >= shape.mainStates) continue;
            if (s.blur && blur.size() >= 4 && !getenv("FRAME_ALL_STATES")) continue;
            id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:descriptor(qc, s.line)
                                                                               error:&error];
            if (!p) {
                fprintf(stderr, "REFUSED %s\n", s.key.c_str());
#ifdef RAW_FACADE
                fprintf(stderr, "FACADE  %s\n", raw_metal_pipeline_key(descriptor(qc, s.line)).UTF8String);
#endif
                return 1;
            }
            (s.uber ? uber : blur).push_back(p);
            printf("STATE %s %s\n", s.uber ? "uber" : "blur", s.key.c_str());
        }
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        id<MTLTexture> tex[frame::TEX_COUNT];
        IOSurfaceRef surfaces[frame::TEX_COUNT] = {};
        for (int t = 0; t < frame::TEX_COUNT; t++) {
            frame::TexDesc td = frame::texture(t);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                         width:td.w
                                                                                        height:td.h
                                                                                     mipmapped:NO];
            d.usage = td.renderTarget ? MTLTextureUsageRenderTarget | (td.memoryless ? 0 : MTLTextureUsageShaderRead)
                                      : MTLTextureUsageShaderRead;
            if (td.iosurface) {
                NSDictionary *p = @{
                    (id)kIOSurfaceWidth : @(td.w),
                    (id)kIOSurfaceHeight : @(td.h),
                    (id)kIOSurfaceBytesPerElement : @4,
                    (id)kIOSurfaceBytesPerRow : @(td.w * 4),
                    (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA')
                };
                surfaces[t] = IOSurfaceCreate((__bridge CFDictionaryRef)p);
                d.storageMode = MTLStorageModeShared;
                tex[t] = [dev newTextureWithDescriptor:d iosurface:surfaces[t] plane:0];
            } else if (!td.memoryless && getenv("FRAME_INPLACE")) {
                // m2v's in-place texture over page-aligned memory: what the raw path's pool textures are to m2v (their twins)
                const NSUInteger row = (td.w * 4 + 255) & ~NSUInteger(255), page = (NSUInteger)getpagesize();
                const NSUInteger bytes = (row * td.h + page - 1) / page * page;
                void *mem = nullptr;
                posix_memalign(&mem, page, bytes);
                memset(mem, 0, bytes);
                id<MTLBuffer> b = [dev newBufferWithBytesNoCopy:mem
                                                         length:bytes
                                                        options:MTLResourceStorageModeShared
                                                    deallocator:nil];
                d.storageMode = MTLStorageModeShared;
                tex[t] = native_bridge_metal_texture_in_place(b, d, 0, row);
                if (!tex[t]) {
                    fprintf(stderr, "in-place texture %d refused\n", t);
                    return 1;
                }
            } else {
                d.storageMode = td.memoryless ? MTLStorageModeMemoryless : MTLStorageModePrivate;
                tex[t] = [dev newTextureWithDescriptor:d];
            }
        }
        id<MTLSamplerState> smp[2];
        for (int s = 0; s < 2; s++) {
            frame::SamplerSpec sp = frame::sampler(s);
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = (MTLSamplerMinMagFilter)sp.minmag;
            sd.mipFilter = (MTLSamplerMipFilter)sp.mip;
            sd.sAddressMode = sd.tAddressMode = sd.rAddressMode = (MTLSamplerAddressMode)sp.address;
            smp[s] = [dev newSamplerStateWithDescriptor:sd];
        }
        std::vector<double> cpu;
        std::vector<uint8_t> bytes;
        id<MTLBuffer> ring =
            [dev newBufferWithLength:frame::kRingBytes
                             options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        NSUInteger ringAt = 0;
        id<MTLBuffer> indices = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
        for (unsigned f = 0; f < frames; f++) {
            @autoreleasepool {
                const uint64_t c0 = threadNs();
                id<MTLCommandBuffer> cb = [queue commandBuffer];
                caCommandBuffer(cb);
                unsigned drawIndex = 0;
                for (const frame::Pass &pass : frame::passes(f, shape, blur.size(), uber.size())) {
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    for (int c = 0; c < 2; c++) {
                        rp.colorAttachments[c].texture = tex[pass.color[c]];
                        rp.colorAttachments[c].loadAction = pass.load[c] == 2   ? MTLLoadActionClear
                                                            : pass.load[c] == 1 ? MTLLoadActionLoad
                                                                                : MTLLoadActionDontCare;
                        rp.colorAttachments[c].storeAction =
                            frame::texture(pass.color[c]).memoryless ? MTLStoreActionDontCare : MTLStoreActionStore;
                        rp.colorAttachments[c].clearColor =
                            MTLClearColorMake(pass.clear[0], pass.clear[1], pass.clear[2], pass.clear[3]);
                    }
                    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                    bool bound[3][31] = {};
                    for (const frame::Draw &d : pass.draws) {
                        [e setRenderPipelineState:d.blur ? blur[d.state] : uber[d.state]];
                        for (const frame::Bytes &b : d.bytes) {
                            frame::fill(bytes, b, f, drawIndex);
                            if (shape.ring) {
                                if (ringAt + b.length > frame::kRingBytes) ringAt = 0;
                                memcpy((uint8_t *)ring.contents + ringAt, bytes.data(), b.length);
                                if (!bound[b.stage][b.slot]) {
                                    if (b.stage == 1)
                                        [e setVertexBuffer:ring offset:ringAt atIndex:b.slot];
                                    else
                                        [e setFragmentBuffer:ring offset:ringAt atIndex:b.slot];
                                    bound[b.stage][b.slot] = true;
                                } else if (b.stage == 1)
                                    [e setVertexBufferOffset:ringAt atIndex:b.slot];
                                else
                                    [e setFragmentBufferOffset:ringAt atIndex:b.slot];
                                ringAt = (ringAt + b.length + 255) & ~NSUInteger(255);
                                continue;
                            }
                            if (b.stage == 1)
                                [e setVertexBytes:bytes.data() length:b.length atIndex:b.slot];
                            else
                                [e setFragmentBytes:bytes.data() length:b.length atIndex:b.slot];
                        }
                        for (int t = 0; t < 16; t++)
                            if (d.textures[t] >= 0) [e setFragmentTexture:tex[d.textures[t]] atIndex:t];
                        for (int s = 0; s < 16; s++)
                            if (d.samplers[s] >= 0) [e setFragmentSamplerState:smp[d.samplers[s]] atIndex:s];
                        if (getenv("FRAME_SAMPLER_SLOT"))
                            [e setFragmentSamplerState:smp[0] atIndex:atoi(getenv("FRAME_SAMPLER_SLOT"))];
                        [e setScissorRect:(MTLScissorRect){d.scissor[0], d.scissor[1], d.scissor[2], d.scissor[3]}];
                        // FRAME_INDEXED: every third main draw indexed as Core Animation's meshes are (CA::OGL::MetalContext::draw:
                        // 16-bit indices written into its buffer pool at 4-byte alignment, drawIndexedPrimitives on triangles)
                        if (getenv("FRAME_INDEXED") && !d.blur && drawIndex % 3 == 1) {
                            static NSUInteger ibAt = 0;
                            if (ibAt + 64 > 65536) ibAt = 0;
                            static const uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
                            memcpy((uint8_t *)indices.contents + ibAt, quad, sizeof quad);
                            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                          indexCount:6
                                           indexType:MTLIndexTypeUInt16
                                         indexBuffer:indices
                                   indexBufferOffset:ibAt];
                            ibAt += 12 + (drawIndex % 5) * 4;  // 4-byte aligned, rarely 256
                        } else
                            [e drawPrimitives:(MTLPrimitiveType)d.primitive
                                  vertexStart:d.first
                                  vertexCount:d.count
                                instanceCount:d.instances];
                        drawIndex++;
                    }
                    [e endEncoding];
                }
                // FRAME_EXTRA: every odd frame, a second command buffer of one pass drawing an extra state (m2v's, on the facade)
                // into BLUR0, then the next frame's raw work reads BLUR0 (its blur passes sample it)
                if ((f & 1) && !extra.empty()) {
                    [cb commit];
                    cb = [queue commandBuffer];
                    caCommandBuffer(cb);
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    rp.colorAttachments[0].texture = tex[frame::BLUR0];
                    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    rp.colorAttachments[1].texture = tex[frame::BLUR_MEMORYLESS];
                    rp.colorAttachments[1].loadAction = MTLLoadActionDontCare;
                    rp.colorAttachments[1].storeAction = MTLStoreActionDontCare;
                    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
                    [e setRenderPipelineState:extra[(f / 2) % extra.size()]];
                    frame::Bytes vb = {1, 1, shape.vertexBytes};
                    frame::fill(bytes, vb, f, 0);
                    [e setVertexBytes:bytes.data() length:vb.length atIndex:1];
                    for (int slot = 0; slot < 8; slot++) {
                        frame::Bytes u = {2, slot, shape.uniformBytes};
                        frame::fill(bytes, u, f, slot);
                        [e setVertexBytes:bytes.data() length:u.length atIndex:slot == 1 ? 7 : slot];
                        [e setFragmentBytes:bytes.data() length:u.length atIndex:slot];
                    }
                    for (int t = 0; t < 16; t++) [e setFragmentTexture:tex[frame::IMAGE1] atIndex:t];
                    for (int s2 = 0; s2 < 16; s2++) [e setFragmentSamplerState:smp[s2 & 1] atIndex:s2];
                    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
                    [e endEncoding];
                }
                [cb commit];
                cpu.push_back((threadNs() - c0) / 1e6);
                [cb waitUntilCompleted];
                if (cb.error) {
                    fprintf(stderr, "frame %u: %s\n", f, cb.error.localizedDescription.UTF8String);
                    return 1;
                }
            }
        }
        const size_t warm = cpu.size() > 20 ? 10 : 0;
        std::vector<double> c(cpu.begin() + warm, cpu.end());
        std::sort(c.begin(), c.end());
        double sum = 0;
        for (double x : c) sum += x;
        if (!c.empty())
            printf(FRAME_PATH " frames=%zu blur=%u draws=%u: encode cpu mean %.3f ms p50 %.3f p90 %.3f\n", c.size(),
                   shape.blurPasses, shape.draws, sum / c.size(), c[c.size() / 2], c[c.size() * 9 / 10]);
        pushcount_report("nvkframe");
    }
    return 0;
}
