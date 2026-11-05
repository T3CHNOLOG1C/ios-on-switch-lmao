// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca): a widget snapshot's texture as the render server samples it, RGB8_444_2P (542) over a two-plane
// 4:4:4 surface ('_RB4': green codes in plane 0, the blue and red pairs in plane 1), drawn through the facade over the mock GPU
// whose copy engine nvdec runs into host memory (NVDEC_WRITETHROUGH). The command buffer that samples it must stay on the raw
// path (the rates line: c=1, m2v=0), and the image the raw path samples must hold, texel for texel, what Metal reads of the
// format (m2v ycbcr.rs, "Codes"): red Cr, green Y, blue Cb, alpha 255, at odd sizes and padded rows.
// usage: planarframe <pack> <cache> <nvk host dylib>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <vector>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

static void missing(const char *, const char *) {}

static IOSurfaceRef planarSurface(uint32_t w, uint32_t h, uint32_t lumaRow, uint32_t chromaRow)
{
    const uint32_t lumaBytes = lumaRow * h, chromaOffset = (lumaBytes + 4095) & ~4095u, chromaBytes = chromaRow * h;
    NSDictionary *luma = @{
        (id)kIOSurfacePlaneWidth : @(w),
        (id)kIOSurfacePlaneHeight : @(h),
        (id)kIOSurfacePlaneBytesPerElement : @1,
        (id)kIOSurfacePlaneBytesPerRow : @(lumaRow),
        (id)kIOSurfacePlaneOffset : @0,
        (id)kIOSurfacePlaneSize : @(lumaBytes)
    };
    NSDictionary *chroma = @{
        (id)kIOSurfacePlaneWidth : @(w),
        (id)kIOSurfacePlaneHeight : @(h),
        (id)kIOSurfacePlaneBytesPerElement : @2,
        (id)kIOSurfacePlaneBytesPerRow : @(chromaRow),
        (id)kIOSurfacePlaneOffset : @(chromaOffset),
        (id)kIOSurfacePlaneSize : @(chromaBytes)
    };
    NSDictionary *props = @{
        (id)kIOSurfaceWidth : @(w),
        (id)kIOSurfaceHeight : @(h),
        (id)kIOSurfacePixelFormat : @(uint32_t('_RB4')),
        (id)kIOSurfacePlaneInfo : @[ luma, chroma ],
        (id)kIOSurfaceAllocSize : @(((chromaOffset + chromaBytes) + 16383) & ~16383u)
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: planarframe <pack> <cache> <nvk dylib>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        NSString *why = nil;
        id<MTLDevice> dev = m2v ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        if (!dev) {
            fprintf(stderr, "NO DEVICE: %s\n", m2v ? why.UTF8String : nmb_last_error());
            return 1;
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        int failed = 0;
        struct Case {
            uint32_t w, h, lumaRow, chromaRow;
        };
        for (Case c : {Case{100, 60, 128, 256}, Case{37, 23, 64, 128}, Case{256, 128, 256, 512}}) {
            IOSurfaceRef s = planarSurface(c.w, c.h, c.lumaRow, c.chromaRow);
            if (!s) {
                printf("FAIL no '_RB4' surface %ux%u\n", c.w, c.h);
                failed++;
                continue;
            }
            IOSurfaceLock(s, 0, nullptr);
            uint8_t *y = (uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 0),
                    *cbcr = (uint8_t *)IOSurfaceGetBaseAddressOfPlane(s, 1);
            for (uint32_t r = 0; r < c.h; r++)
                for (uint32_t x = 0; x < c.w; x++) {
                    y[r * c.lumaRow + x] = uint8_t(x * 7 + r * 13 + 1);
                    cbcr[r * c.chromaRow + 2 * x] = uint8_t(x * 3 + r * 5 + 40);       // blue (Cb)
                    cbcr[r * c.chromaRow + 2 * x + 1] = uint8_t(x * 11 + r * 2 + 90);  // red (Cr)
                }
            IOSurfaceUnlock(s, 0, nullptr);
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(542)
                                                                                         width:c.w
                                                                                        height:c.h
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> t = [dev newTextureWithDescriptor:d iosurface:s plane:0];
            size_t pitch = 0;
            if (!t || !raw_metal_texture_image(t, &pitch)) {
                printf("FAIL %ux%u: no raw planar texture\n", c.w, c.h);
                failed++;
                CFRelease(s);
                continue;
            }
            memset((void *)raw_metal_texture_image(t, nullptr), 0, pitch * c.h);
            // a pass that samples it (as Core Animation binds a snapshot): the conversion runs before it
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:64
                                                                                         height:64
                                                                                      mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> target = [dev newTextureWithDescriptor:td];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
            [e setFragmentTexture:t atIndex:0];
            [e endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const uint8_t *img = (const uint8_t *)raw_metal_texture_image(t, nullptr);
            size_t bad = 0;
            for (uint32_t r = 0; r < c.h; r++)
                for (uint32_t x = 0; x < c.w; x++) {
                    const uint8_t *p = img + r * pitch + 4 * x;
                    const uint8_t want[4] = {cbcr[r * c.chromaRow + 2 * x + 1], y[r * c.lumaRow + x],
                                             cbcr[r * c.chromaRow + 2 * x], 255};
                    if (memcmp(p, want, 4)) {
                        if (!bad && getenv("PLANAR_DEBUG"))
                            fprintf(stderr, "  (%u,%u) got %02x%02x%02x%02x want %02x%02x%02x%02x\n", x, r, p[0], p[1],
                                    p[2], p[3], want[0], want[1], want[2], want[3]);
                        bad++;
                    }
                }
            printf("%s 542 over '_RB4' %ux%u (rows %u/%u): %zu of %u texels differ\n", bad ? "FAIL" : "OK  ", c.w, c.h,
                   c.lumaRow, c.chromaRow, bad, c.w * c.h);
            failed += bad != 0;
            CFRelease(s);
        }
        char line[1024];
        if (raw_metal_rates(line, sizeof line)) printf("rates: %s\n", line);
        printf("planarframe: %d case(s) failed\n", failed);
        return failed ? 1 : 0;
    }
}
