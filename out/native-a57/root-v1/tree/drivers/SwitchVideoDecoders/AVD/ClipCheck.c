// SPDX-License-Identifier: BSD-2-Clause-Patent
// See ClipCheck.h.
#include "ClipCheck.h"
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../test/VDECClip.h"

typedef struct {
    int count, match, errors, outOfOrder, last, first;
    uint32_t firstCrc;
} Result;

static uint32_t crc32Update(uint32_t crc, const uint8_t *p, size_t n)  // zlib's CRC-32 (reflected 0xedb88320)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
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
    uint32_t crc = 0;
    for (int row = 0; row < kClipHeight; row++) crc = crc32Update(crc, y + (size_t)row * pitch, kClipWidth);
    CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    const int index = (int)pts.value;
    if (index >= 0 && index < kClipFrames && crc == kClipLumaCrc[index]) {
        r->match++;
    } else if (r->first < 0) {
        r->first = index;
        r->firstCrc = crc;
    }
    if (index < r->last) r->outOfOrder++;
    r->last = index;
    r->count++;
}

// Annex-B NAL units -> the parameter sets and 4-byte length-prefixed access units (a slice with first_mb_in_slice 0
// starts a picture).
static int split(const uint8_t *sets[2], size_t setSizes[2], uint8_t *buffer, uint8_t *samples[], size_t sizes[],
                 int maxSamples)
{
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
    if (!n) return 0;
    length[n - 1] = (size_t)(kClip + sizeof(kClip) - nal[n - 1]);
    int count = 0;
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        const int type = nal[i][0] & 0x1f;
        if (type == 7 || type == 8) {
            sets[type - 7] = nal[i], setSizes[type - 7] = length[i];
            continue;
        }
        if (type != 1 && type != 5) continue;
        if (nal[i][1] & 0x80) {  // first_mb_in_slice == 0
            if (count == maxSamples) break;
            samples[count] = buffer + used;
            sizes[count++] = 0;
        }
        if (!count) continue;
        uint8_t *p = buffer + used;
        p[0] = (uint8_t)(length[i] >> 24), p[1] = (uint8_t)(length[i] >> 16), p[2] = (uint8_t)(length[i] >> 8);
        p[3] = (uint8_t)length[i];
        memcpy(p + 4, nal[i], length[i]);
        used += length[i] + 4;
        sizes[count - 1] += length[i] + 4;
    }
    return count;
}

bool clip_check(ClipCheckMode mode, CFDictionaryRef extraSpec, char *line, size_t size)
{
    const bool hardware = mode == ClipCheckHardware;
    const char *name = hardware ? "hardware" : "software";
    static uint8_t buffer[sizeof(kClip) + 4096];
    const uint8_t *sets[2] = {NULL, NULL};
    size_t setSizes[2] = {0, 0}, sizes[64];
    uint8_t *samples[64];
    const int count = split(sets, setSizes, buffer, samples, sizes, 64);
    CMVideoFormatDescriptionRef format = NULL;
    if (!count || !sets[0] || !sets[1] ||
        CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, sets, setSizes, 4, &format)) {
        snprintf(line, size, "%s: could not build the clip's format description FAIL", name);
        return false;
    }

    const void *specKeys[] = {hardware ? kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder
                                       : kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder};
    const void *specValues[] = {hardware ? kCFBooleanTrue : kCFBooleanFalse};
    CFMutableDictionaryRef spec = extraSpec ? CFDictionaryCreateMutableCopy(NULL, 0, extraSpec)
                                            : CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(spec, specKeys[0], specValues[0]);
    int32_t pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pf = CFNumberCreate(NULL, kCFNumberSInt32Type, &pixelFormat);
    const void *attrKeys[] = {kCVPixelBufferPixelFormatTypeKey};
    const void *attrValues[] = {pf};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, attrKeys, attrValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    Result result = {.last = -1, .first = -1};
    VTDecompressionOutputCallbackRecord callback = {output, &result};
    VTDecompressionSessionRef session = NULL;
    OSStatus status = VTDecompressionSessionCreate(NULL, format, spec, attrs, &callback, &session);
    CFRelease(spec), CFRelease(attrs), CFRelease(pf);
    if (status) {
        CFRelease(format);
        snprintf(line, size, "%s: VTDecompressionSessionCreate failed %d%s FAIL", name, (int)status,
                 hardware ? " (no hardware H.264 decoder: AVD.videodecoder not registered in this process)" : "");
        return false;
    }
    CFBooleanRef usingHardware = NULL;
    VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL,
                          &usingHardware);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int failed = 0;
    for (int i = 0; i < count; i++) {
        CMBlockBufferRef block = NULL;
        CMSampleBufferRef sample = NULL;
        CMBlockBufferCreateWithMemoryBlock(NULL, samples[i], sizes[i], kCFAllocatorNull, NULL, 0, sizes[i], 0, &block);
        CMSampleTimingInfo timing = {CMTimeMake(1, 30), CMTimeMake(i < kClipFrames ? kClipDisplay[i] : i, 30),
                                     CMTimeMake(i, 30)};
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
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    CFRelease(format);
    const double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    const bool onHardware = usingHardware == kCFBooleanTrue;
    if (usingHardware) CFRelease(usingHardware);
    const bool ok = result.match == kClipFrames && result.count == kClipFrames && (!hardware || onHardware);
    char mismatch[96] = "";
    if (result.first >= 0)
        snprintf(mismatch, sizeof(mismatch), "; first mismatch frame %d luma CRC %#010x, expected %#010x", result.first,
                 result.firstCrc, result.first < kClipFrames ? kClipLumaCrc[result.first] : 0);
    snprintf(line, size,
             "%s: %s decoder, %d/%d frames match x264, %d out (%d before an earlier frame), %d decode errors, "
             "%d callback errors, %.1f ms%s %s",
             name, onHardware ? "hardware" : "software", result.match, kClipFrames, result.count, result.outOfOrder,
             failed, result.errors, ms, mismatch, ok ? "OK" : "FAIL");
    return ok;
}
