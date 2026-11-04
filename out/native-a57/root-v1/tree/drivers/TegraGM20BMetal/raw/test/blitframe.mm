// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca): Core Animation's copies (copy_surface, update_surface, average_luma_surface: copyFromTexture; APL
// dimming: copyFromBuffer:toBuffer:; image uploads: copyFromBuffer:toTexture:; readbacks: copyFromTexture:toBuffer:) through a
// device, over the mock GPU whose copy engine nvdec runs into host memory (NVDEC_WRITETHROUGH), checked byte for byte against
// what Metal's copy semantics give, computed on the CPU. Run once on the m2v device (the reference) and once on the facade (the
// raw path; FRAME_FALLBACK: its m2v device), the same program: both must print OK for every case, and the facade must take the
// raw path for every command buffer (its stderr says so if not).
// usage: blitframe <pack|-> <cache> <nvk dylib>       (pack "-": the m2v device alone)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <vector>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static uint8_t pattern(unsigned i)
{
    return uint8_t(i * 131u + (i >> 7) * 17u + 5u);
}

static int check(const char *what, const uint8_t *got, const uint8_t *want, size_t n)
{
    size_t bad = 0, first = n;
    for (size_t i = 0; i < n; i++)
        if (got[i] != want[i]) {
            bad++;
            if (first == n) first = i;
        }
    if (bad && getenv("BLIT_DEBUG"))
        for (size_t i = first; i < n && i < first + 16; i++)
            fprintf(stderr, "  @%zu got %02x want %02x\n", i, got[i], want[i]);
    printf("%s %s (%zu bytes, %zu differ)\n", bad ? "FAIL" : "OK  ", what, n, bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 4) {
            fprintf(stderr, "usage: blitframe <pack|-> <cache> <nvk dylib>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        if (!m2v) {
            fprintf(stderr, "NO M2V DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        id<MTLDevice> dev = m2v;
        if (strcmp(argv[1], "-")) {
            NSString *why = nil;
            dev = raw_metal_device_open(argv[1], 0, m2v, &why);
            if (!dev) {
                fprintf(stderr, "NO RAW DEVICE: %s\n", why.UTF8String);
                return 1;
            }
        }
        int failed = 0;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        auto texture = [&](MTLPixelFormat f, unsigned w, unsigned h) {
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f
                                                                                         width:w
                                                                                        height:h
                                                                                     mipmapped:NO];
            d.storageMode =
                MTLStorageModeShared;  // CPU-readable on both drivers (the raw path's private ones are the same memory)
            d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            return [dev newTextureWithDescriptor:d];
        };
        struct Case {
            MTLPixelFormat f;
            unsigned bpp;
        };
        for (Case c :
             {Case{MTLPixelFormatBGRA8Unorm, 4}, Case{MTLPixelFormatRGBA16Float, 8}, Case{MTLPixelFormatR8Unorm, 1}}) {
            // texture to texture: a region to an offset
            const unsigned sw = 100, sh = 60, dw = 128, dh = 64;
            id<MTLTexture> s = texture(c.f, sw, sh), d = texture(c.f, dw, dh);
            std::vector<uint8_t> src(sw * sh * c.bpp), dst(dw * dh * c.bpp, 0), want;
            for (size_t i = 0; i < src.size(); i++) src[i] = pattern(unsigned(i));
            [s replaceRegion:MTLRegionMake2D(0, 0, sw, sh) mipmapLevel:0 withBytes:src.data() bytesPerRow:sw * c.bpp];
            [d replaceRegion:MTLRegionMake2D(0, 0, dw, dh) mipmapLevel:0 withBytes:dst.data() bytesPerRow:dw * c.bpp];
            want = dst;
            for (unsigned y = 0; y < 30; y++)
                memcpy(&want[((7 + y) * dw + 3) * c.bpp], &src[((5 + y) * sw + 10) * c.bpp], 40 * c.bpp);
            // buffer to texture (rows of 512 bytes) and texture to buffer (rows of 256)
            id<MTLBuffer> up = [dev newBufferWithLength:512 * 20 options:MTLResourceStorageModeShared];
            for (unsigned i = 0; i < 512 * 20; i++) ((uint8_t *)up.contents)[i] = pattern(i + 999);
            for (unsigned y = 0; y < 20; y++)
                memcpy(&want[((40 + y) * dw + 70) * c.bpp], (uint8_t *)up.contents + y * 512, 50 * c.bpp);
            id<MTLBuffer> down = [dev newBufferWithLength:256 * 10 options:MTLResourceStorageModeShared];
            memset(down.contents, 0xee, 256 * 10);
            std::vector<uint8_t> wantDown(256 * 10, 0xee);
            for (unsigned y = 0; y < 10; y++) memcpy(&wantDown[y * 256], &src[((3 + y) * sw + 2) * c.bpp], 30 * c.bpp);

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:s
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(10, 5, 0)
                       sourceSize:MTLSizeMake(40, 30, 1)
                        toTexture:d
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(3, 7, 0)];
            [b copyFromBuffer:up
                       sourceOffset:0
                  sourceBytesPerRow:512
                sourceBytesPerImage:512 * 20
                         sourceSize:MTLSizeMake(50, 20, 1)
                          toTexture:d
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(70, 40, 0)];
            [b copyFromTexture:s
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(2, 3, 0)
                              sourceSize:MTLSizeMake(30, 10, 1)
                                toBuffer:down
                       destinationOffset:0
                  destinationBytesPerRow:256
                destinationBytesPerImage:256 * 10];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            std::vector<uint8_t> got(dw * dh * c.bpp);
            [d getBytes:got.data() bytesPerRow:dw * c.bpp fromRegion:MTLRegionMake2D(0, 0, dw, dh) mipmapLevel:0];
            char what[96];
            snprintf(what, sizeof what, "format %lu: texture copy + buffer-to-texture copy", (unsigned long)c.f);
            failed += check(what, got.data(), want.data(), got.size());
            snprintf(what, sizeof what, "format %lu: texture-to-buffer copy", (unsigned long)c.f);
            failed += check(what, (const uint8_t *)down.contents, wantDown.data(), wantDown.size());
        }
        {
            // buffer to buffer, over 128 KiB (NVK's launch size) and at odd offsets
            const unsigned n = 300000;
            id<MTLBuffer> a = [dev newBufferWithLength:n + 64 options:MTLResourceStorageModeShared];
            id<MTLBuffer> z = [dev newBufferWithLength:n + 64 options:MTLResourceStorageModeShared];
            for (unsigned i = 0; i < n + 64; i++) ((uint8_t *)a.contents)[i] = pattern(i + 7);
            memset(z.contents, 0, n + 64);
            std::vector<uint8_t> want(n + 64, 0);
            memcpy(&want[64], (uint8_t *)a.contents + 16, n - 48);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromBuffer:a sourceOffset:16 toBuffer:z destinationOffset:64 size:n - 48];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            failed += check("buffer-to-buffer copy (300 KB)", (const uint8_t *)z.contents, want.data(), want.size());
        }
        printf("%s: %d case(s) failed\n", strcmp(argv[1], "-") ? "raw facade" : "m2v", failed);
        return failed ? 1 : 0;
    }
}
