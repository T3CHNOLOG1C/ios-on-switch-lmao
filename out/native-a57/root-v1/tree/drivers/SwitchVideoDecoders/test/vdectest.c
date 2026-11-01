// SPDX-License-Identifier: BSD-2-Clause-Patent
// vdectest: decodes an embedded H.264 clip (320x240, 30 frames, High profile with B-pyramid) through VideoToolbox twice,
// once requiring a hardware decoder (on the Switch: AVD.videodecoder on NVDEC) and once with hardware disabled (Apple's
// H264SW), and compares every output frame's luma CRC with x264's reconstruction in display order. Results go to the
// unified log (subsystem com.ioswitch.avd) and, on the device, to /private/var/ioswitch/vdectest.log.
//
// On the device launchd starts it once when the TegraNVDEC service appears (com.ioswitch.vdectest.plist, IOKit
// matching), so it runs only with switch-nvdec=3 and the overlay's VideoDecoders union on. It also runs on a Mac,
// which checks the test itself against Apple's decoders:
//   cc -O2 -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox vdectest.c
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <TargetConditionals.h>
#include <VideoToolbox/VideoToolbox.h>
#include <os/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#if TARGET_OS_IPHONE
#include <dispatch/dispatch.h>
#include <xpc/xpc.h>
// libxpc exports it on iOS too; the SDK only marks it unavailable to apps.
extern void vdectest_take_events(const char *stream, dispatch_queue_t queue, void (^handler)(xpc_object_t))
    __asm__("_xpc_set_event_stream_handler");
#endif
#include "VDECClip.h"

typedef struct {
    int count, match, errors, outOfOrder, last, first;
    uint32_t firstCrc;
} Result;

static os_log_t gLog;
static FILE *gFile;

static void report(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void report(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    os_log(gLog, "vdectest: %{public}s", line);
    printf("vdectest: %s\n", line);
    if (gFile) fprintf(gFile, "vdectest: %s\n", line), fflush(gFile);
}

// Frames carry their display position as the presentation time and each output is checked against that frame's CRC.
// Apple's own H.264 decoders put frames out in decode order even with temporal processing (macOS 27, both hardware
// and software), leaving display order to the client, so the order is reported, not required.
static void output(void *refcon, void *frameRefcon, OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image,
                   CMTime pts, CMTime duration)
{
    (void)frameRefcon, (void)flags, (void)duration;
    Result *r = refcon;
    if (status || !image) {
        if (status) r->errors++;
        return;
    }
    CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(image, 0);
    const size_t pitch = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    uLong crc = crc32(0, NULL, 0);
    for (int row = 0; row < kClipHeight; row++) crc = crc32(crc, y + (size_t)row * pitch, kClipWidth);
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const int index = (int)pts.value;
    if (index >= 0 && index < kClipFrames && (uint32_t)crc == kClipLumaCrc[index]) {
        r->match++;
    } else if (r->first < 0) {
        r->first = index;
        r->firstCrc = (uint32_t)crc;
    }
    if (index < r->last) r->outOfOrder++;
    r->last = index;
    r->count++;
}

static void run(bool hardware, CMVideoFormatDescriptionRef format, uint8_t **samples, size_t *sizes, int count)
{
    const void *specKeys[] = {hardware ? kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder
                                       : kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder};
    const void *specValues[] = {hardware ? kCFBooleanTrue : kCFBooleanFalse};
    CFDictionaryRef spec = CFDictionaryCreate(NULL, specKeys, specValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    int32_t pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pf = CFNumberCreate(NULL, kCFNumberSInt32Type, &pixelFormat);
    const void *attrKeys[] = {kCVPixelBufferPixelFormatTypeKey};
    const void *attrValues[] = {pf};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, attrKeys, attrValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    Result result = {.last = -1, .first = -1};
    VTDecompressionOutputCallbackRecord callback = {output, &result};
    VTDecompressionSessionRef session = NULL;
    const char *name = hardware ? "hardware" : "software";
    OSStatus status = VTDecompressionSessionCreate(NULL, format, spec, attrs, &callback, &session);
    CFRelease(spec), CFRelease(attrs), CFRelease(pf);
    if (status) {
        report("%s: VTDecompressionSessionCreate failed %d%s", name, (int)status,
               hardware ? " (no hardware H.264 decoder: AVD.videodecoder not registered or refused)" : "");
        return;
    }
    CFBooleanRef usingHardware = NULL;
    VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &usingHardware);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int failed = 0;
    for (int i = 0; i < count; i++) {
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        CMBlockBufferCreateWithMemoryBlock(NULL, samples[i], sizes[i], kCFAllocatorNull, NULL, 0, sizes[i], 0, &block);
        CMSampleTimingInfo timing = {CMTimeMake(1, 30), CMTimeMake(kClipDisplay[i], 30), CMTimeMake(i, 30)};
        CMSampleBufferCreateReady(NULL, block, format, 1, 1, &timing, 1, &sizes[i], &sample);
        VTDecodeInfoFlags info = 0;
        if (VTDecompressionSessionDecodeFrame(session, sample, kVTDecodeFrame_EnableTemporalProcessing, NULL, &info))
            failed++;
        CFRelease(sample);
        CFRelease(block);
    }
    VTDecompressionSessionFinishDelayedFrames(session);
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    const double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    const bool ok = result.match == kClipFrames && result.count == kClipFrames;
    report("%s: %s decoder, %d/%d frames match x264, %d out (%d before an earlier frame), %d decode errors, "
           "%d callback errors, %.1f ms %s", name, usingHardware == kCFBooleanTrue ? "hardware" : "software",
           result.match, kClipFrames, result.count, result.outOfOrder, failed, result.errors, ms, ok ? "OK" : "FAIL");
    if (result.first >= 0)
        report("%s: first mismatch: frame %d luma CRC %#010x, expected %#010x", name, result.first, result.firstCrc,
               result.first < kClipFrames ? kClipLumaCrc[result.first] : 0);
    if (usingHardware) CFRelease(usingHardware);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
}

static void test(void)
{
    // Annex-B NAL units -> parameter sets and 4-byte length-prefixed access units (a slice with first_mb_in_slice 0
    // starts a picture).
    const uint8_t *nal[256];
    size_t length[256];
    int n = 0;
    for (size_t i = 0; i + 3 <= sizeof(kClip) && n < 256;) {
        if (kClip[i] == 0 && kClip[i + 1] == 0 && kClip[i + 2] == 1) {
            if (n) {
                size_t e = i;
                while (e && !kClip[e - 1]) e--;
                length[n - 1] = (size_t)(kClip + e - nal[n - 1]);
            }
            nal[n++] = kClip + i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    length[n - 1] = (size_t)(kClip + sizeof(kClip) - nal[n - 1]);
    const uint8_t *sets[2] = {NULL, NULL};
    size_t setSizes[2] = {0, 0};
    static uint8_t buffer[sizeof(kClip) + 4096];
    uint8_t *samples[64];
    size_t sizes[64];
    int count = 0;
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        const int type = nal[i][0] & 0x1f;
        if (type == 7 || type == 8) {
            sets[type - 7] = nal[i], setSizes[type - 7] = length[i];
            continue;
        }
        if (type != 1 && type != 5) continue;
        if ((nal[i][1] & 0x80) && count < 64) {   // first_mb_in_slice == 0
            samples[count] = buffer + used;
            sizes[count++] = 0;
        }
        uint8_t *p = buffer + used;
        p[0] = (uint8_t)(length[i] >> 24), p[1] = (uint8_t)(length[i] >> 16), p[2] = (uint8_t)(length[i] >> 8);
        p[3] = (uint8_t)length[i];
        memcpy(p + 4, nal[i], length[i]);
        used += length[i] + 4;
        sizes[count - 1] += length[i] + 4;
    }
    CMVideoFormatDescriptionRef format = NULL;
    if (!sets[0] || !sets[1] ||
        CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, setSizes, 4, &format)) {
        report("could not build the format description");
        return;
    }
    run(true, format, samples, sizes, count);
    run(false, format, samples, sizes, count);
    CFRelease(format);
}

int main(void)
{
    gLog = os_log_create("com.ioswitch.avd", "vdectest");
#if TARGET_OS_IPHONE
    // Launched by an IOKit matching event: take it, so launchd does not start this again for the same service.
    vdectest_take_events("com.apple.iokit.matching", dispatch_get_main_queue(), ^(xpc_object_t event) {
        (void)event;
    });
    gFile = fopen("/private/var/ioswitch/vdectest.log", "a");
#endif
    test();
    if (gFile) fclose(gFile);
    return 0;
}
