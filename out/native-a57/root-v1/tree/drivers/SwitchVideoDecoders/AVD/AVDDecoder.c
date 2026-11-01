// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVD.videodecoder: H.264 on the Tegra X1's NVDEC as VideoToolbox's hardware 'avc1' decoder.
//
// How VideoToolbox finds it (21H461, see HEVCH8.c for the disassembly): VTLoadVideoDecoders dlopens a fixed list of
// paths and calls each one's register function. /System/Library/VideoDecoders/AVD.videodecoder (AVDRegister) is the
// slot of Apple's AVD decoders on later SoCs; iPad7,1 ships no such file, so the userland overlay puts this one there
// (the VideoDecoders union HEVCH8 shares, gate switch-hevc). AVDRegister registers nothing unless the kernel published
// TegraNVDEC (switch-nvdec=3), so the default boot only pays one failed service lookup per process. The list starts with H264SW.videocodec, Apple's NEON H.264 decoder, which registers 'avc1' with
// {IsHardwareAccelerated: false} and rating 0. VTRegisterVideoDecoder leaves IsHardwareAccelerated true and rating 0,
// and _vtSortRegistryItemsByRating ranks rating first, then hardware first, so this decoder is tried first; when its
// factory fails (no TegraNVDEC service, or the sandbox refuses this process its client) VideoToolbox moves on to
// H264SW. A session this decoder cannot take (interlaced, 4:2:2, high bit depth, more than 4096x4096) fails in
// StartSession, which VideoToolbox does not retry with another decoder: a limit of this version.
//
// Per picture: H264NVDEC parses and fills NVDEC's setup, the kernel decodes into a block-linear surface, and the
// picture is copied (de-swizzled) on the CPU into a pixel buffer from the session's pool, NV12 '420v'/'420f'. VIC is
// not used: with the SMMU bypassed it can only write physically contiguous memory, which pool IOSurfaces are not, so
// its output would need this same CPU pass. Frames go out as soon as they are decoded, in decode order, as Apple's
// H.264 decoders do even under kVTDecodeFrame_EnableTemporalProcessing (observed on macOS 27 with test/vdectest):
// clients order by presentation time. H264NVDEC's display-order bumping stays available (kReorder).
#include <CoreFoundation/CoreFoundation.h>
#include <os/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "BlockLinear.h"
#include "H264NVDEC.h"
#include "NVDECClient.h"
#include "VTDecoderSPI.h"

typedef struct {
    pthread_mutex_t lock;
    VTVideoDecoderSession session;
    NVDECClient *client;
    H264Decoder *h264;
    CFDataRef avcC;
    unsigned lengthSize;
    OSType pixelFormat;
    CFTypeRef matrix, primaries, transfer;
} Decoder;

typedef struct {
    CVPixelBufferRef image;
    VTVideoDecoderFrame frame;
    OSStatus status;
} Output;

static os_log_t gLog;
static const bool kReorder = false;  // see above

static Decoder *storage(CMBaseObjectRef object)
{
    return CMBaseObjectGetDerivedStorage(object);
}

// ---- H264Backend: pictures out ---------------------------------------------------------------------------------

static void *decoded(void *ctx, const H264Picture *picture)
{
    Decoder *d = ctx;
    Output *out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    out->frame = picture->token;
    out->status = picture->status ? kVTVideoDecoderBadDataErr : noErr;
    CVPixelBufferPoolRef pool = d->session ? VTDecoderSessionGetPixelBufferPool(d->session) : NULL;
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &out->image) != kCVReturnSuccess) {
        out->status = kVTVideoDecoderMalfunctionErr;
        return out;
    }
    CVPixelBufferLockBaseAddress(out->image, 0);
    const uint32_t w = (uint32_t)CVPixelBufferGetWidth(out->image), h = (uint32_t)CVPixelBufferGetHeight(out->image);
    const uint32_t width = w < picture->width ? w : picture->width, height = h < picture->height ? h : picture->height;
    block_linear_copy(CVPixelBufferGetBaseAddressOfPlane(out->image, 0),
                      CVPixelBufferGetBytesPerRowOfPlane(out->image, 0), picture->luma, picture->pitch, picture->crop_x,
                      picture->crop_y, width, height);
    block_linear_copy(CVPixelBufferGetBaseAddressOfPlane(out->image, 1),
                      CVPixelBufferGetBytesPerRowOfPlane(out->image, 1), picture->chroma, picture->pitch,
                      picture->crop_x & ~1u, picture->crop_y / 2, width & ~1u, (height + 1) / 2);
    CVPixelBufferUnlockBaseAddress(out->image, 0);
    if (d->matrix)
        CVBufferSetAttachment(out->image, kCVImageBufferYCbCrMatrixKey, d->matrix, kCVAttachmentMode_ShouldPropagate);
    if (d->primaries)
        CVBufferSetAttachment(out->image, kCVImageBufferColorPrimariesKey, d->primaries,
                              kCVAttachmentMode_ShouldPropagate);
    if (d->transfer)
        CVBufferSetAttachment(out->image, kCVImageBufferTransferFunctionKey, d->transfer,
                              kCVAttachmentMode_ShouldPropagate);
    return out;
}

static void emit(void *ctx, void *item)
{
    Decoder *d = ctx;
    Output *out = item;
    if (d->session)
        VTDecoderSessionEmitDecodedFrame(d->session, out->frame, out->status,
                                         out->image ? 0 : kVTDecodeInfo_FrameDropped, out->image);
    if (out->image) CVPixelBufferRelease(out->image);
    free(out);
}

static void discard(void *ctx, void *item)  // no_output_of_prior_pics: answered, without a picture
{
    Output *out = item;
    if (out->image) CVPixelBufferRelease(out->image);
    out->image = NULL;
    emit(ctx, out);
}

// ---- H264Backend: buffers and jobs, on this decoder's connection --------------------------------------------------

static int bufferAlloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    return nvdec_alloc(((Decoder *)ctx)->client, size, handle, va);
}
static void bufferFree(void *ctx, uint32_t handle)
{
    nvdec_free(((Decoder *)ctx)->client, handle);
}
static int jobSubmit(void *ctx, const TegraNVDECSubmit *submit, uint32_t *fence)
{
    return nvdec_submit(((Decoder *)ctx)->client, submit, fence);
}
static int jobWait(void *ctx, const TegraNVDECWait *wait)
{
    return nvdec_wait(((Decoder *)ctx)->client, wait);
}

// ---- Colour: the format description's own tags, else the SPS's VUI (H.273 code points) --------------------------

static CFTypeRef colourTag(CMFormatDescriptionRef format, CFStringRef key, int code, int kind, uint32_t height)
{
    CFTypeRef tag = CMFormatDescriptionGetExtension(format, key);
    if (tag) return CFRetain(tag);
    switch (kind) {
    case 0:  // matrix_coefficients
        if (code == 1) return CFRetain(kCVImageBufferYCbCrMatrix_ITU_R_709_2);
        if (code == 5 || code == 6) return CFRetain(kCVImageBufferYCbCrMatrix_ITU_R_601_4);
        if (code == 9) return CFRetain(kCVImageBufferYCbCrMatrix_ITU_R_2020);
        return CFRetain(height >= 720 ? kCVImageBufferYCbCrMatrix_ITU_R_709_2 : kCVImageBufferYCbCrMatrix_ITU_R_601_4);
    case 1:  // colour_primaries
        if (code == 1) return CFRetain(kCVImageBufferColorPrimaries_ITU_R_709_2);
        if (code == 5) return CFRetain(kCVImageBufferColorPrimaries_EBU_3213);
        if (code == 6) return CFRetain(kCVImageBufferColorPrimaries_SMPTE_C);
        if (code == 9) return CFRetain(kCVImageBufferColorPrimaries_ITU_R_2020);
        return NULL;
    default:  // transfer_characteristics
        if (code == 1 || code == 6 || code == 14 || code == 15)
            return CFRetain(kCVImageBufferTransferFunction_ITU_R_709_2);
        if (code == 16) return CFRetain(kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ);
        if (code == 18) return CFRetain(kCVImageBufferTransferFunction_ITU_R_2100_HLG);
        return NULL;
    }
}

// ---- VTVideoDecoderClass -------------------------------------------------------------------------------------------

static CFDataRef copyAvcC(CMVideoFormatDescriptionRef format)
{
    CFDictionaryRef atoms =
        CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    CFTypeRef avcC =
        atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(atoms, CFSTR("avcC")) : NULL;
    return avcC && CFGetTypeID(avcC) == CFDataGetTypeID() ? CFRetain(avcC) : NULL;
}

// ISO/IEC 14496-15 AVCDecoderConfigurationRecord: lengthSizeMinusOne, then the SPS and PPS lists.
static int loadAvcC(Decoder *d, CFDataRef avcC)
{
    const uint8_t *p = CFDataGetBytePtr(avcC);
    const size_t size = (size_t)CFDataGetLength(avcC);
    if (size < 7 || p[0] != 1) return -1;
    d->lengthSize = (p[4] & 3) + 1;
    size_t pos = 5;
    for (unsigned list = 0; list < 2; list++) {
        if (pos >= size) return -1;
        unsigned count = list ? p[pos] : p[pos] & 0x1f;
        pos++;
        while (count--) {
            if (pos + 2 > size) return -1;
            const size_t n = (size_t)p[pos] << 8 | p[pos + 1];
            pos += 2;
            if (pos + n > size || h264_decoder_parameter_set(d->h264, p + pos, n)) return -1;
            pos += n;
        }
    }
    return 0;
}

static void setNumber(CFMutableDictionaryRef dict, CFStringRef key, int32_t value)
{
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, n);
    CFRelease(n);
}

static OSStatus startSession(VTVideoDecoderRef ref, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format)
{
    Decoder *d = storage(ref);
    pthread_mutex_lock(&d->lock);
    d->session = session;
    d->avcC = copyAvcC(format);
    OSStatus status = kVTVideoDecoderUnsupportedDataFormatErr;
    const H264SPS *sps = NULL;
    const int configured = d->avcC && !loadAvcC(d, d->avcC) ? h264_decoder_configure(d->h264) : -1;
    if (!configured) sps = h264_decoder_sps(d->h264);
    if (sps) {
        const int32_t width = sps->width_mbs * 16 - (int32_t)(sps->crop_left + sps->crop_right);
        const int32_t height = sps->height_map_units * 16 - (int32_t)(sps->crop_top + sps->crop_bottom);
        const bool vui = sps->colour_description;
        d->pixelFormat = sps->full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                         : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        d->matrix =
            colourTag(format, kCMFormatDescriptionExtension_YCbCrMatrix, vui ? sps->matrix : 2, 0, (uint32_t)height);
        d->primaries =
            colourTag(format, kCMFormatDescriptionExtension_ColorPrimaries, vui ? sps->colour_primaries : 2, 1, 0);
        d->transfer = colourTag(format, kCMFormatDescriptionExtension_TransferFunction, vui ? sps->transfer : 2, 2, 0);
        CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
        setNumber(attributes, kCVPixelBufferPixelFormatTypeKey, (int32_t)d->pixelFormat);
        setNumber(attributes, kCVPixelBufferWidthKey, width);
        setNumber(attributes, kCVPixelBufferHeightKey, height);
        setNumber(attributes, kCVPixelBufferBytesPerRowAlignmentKey, 64);
        CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
        status = VTDecoderSessionSetPixelBufferAttributes(session, attributes);
        CFRelease(surface);
        CFRelease(attributes);
    } else if (configured == -3) {
        status = kVTVideoDecoderMalfunctionErr;
        os_log_error(gLog, "AVD: no NVDEC buffers for this stream (see the AVD alloc/map line)");
    } else {
        os_log_error(gLog, "AVD: stream not decodable on NVDEC (needs progressive 4:2:0 8-bit H.264 up to 4096x4096)");
    }
    pthread_mutex_unlock(&d->lock);
    return status;
}

static OSStatus decodeFrame(VTVideoDecoderRef ref, VTVideoDecoderFrame frame, CMSampleBufferRef sample,
                            VTDecodeFrameFlags flags, VTDecodeInfoFlags *info)
{
    Decoder *d = storage(ref);
    if (info) *info = 0;
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    const size_t size = block ? CMBlockBufferGetDataLength(block) : 0;
    if (!size) return kVTVideoDecoderBadDataErr;
    char *data = NULL, *copy = NULL;
    if (!CMBlockBufferIsRangeContiguous(block, 0, size) ||
        CMBlockBufferGetDataPointer(block, 0, NULL, NULL, &data) != kCMBlockBufferNoErr) {
        if (!(copy = data = malloc(size)) || CMBlockBufferCopyDataBytes(block, 0, size, data) != kCMBlockBufferNoErr) {
            free(copy);
            return kVTVideoDecoderMalfunctionErr;
        }
    }
    pthread_mutex_lock(&d->lock);
    const int ret = h264_decoder_decode(d->h264, (const uint8_t *)data, size, d->lengthSize, frame,
                                        kReorder && (flags & kVTDecodeFrame_EnableTemporalProcessing));
    if (!ret && d->session)  // parameter sets only: answered at once, without a picture
        VTDecoderSessionEmitDecodedFrame(d->session, frame, noErr, kVTDecodeInfo_FrameDropped, NULL);
    pthread_mutex_unlock(&d->lock);
    free(copy);
    if (ret <= -2) {
        static int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            os_log_error(gLog, "AVD: %{public}s",
                         ret == -2 ? "NVDEC job failed" : "no NVDEC buffers for a new geometry");
        return kVTVideoDecoderMalfunctionErr;
    }
    return ret < 0 ? kVTVideoDecoderBadDataErr : noErr;
}

static OSStatus copySupportedPropertyDictionary(VTVideoDecoderRef ref, CFDictionaryRef *out)
{
    (void)ref;
    *out = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
    return *out ? noErr : kVTAllocationFailedErr;
}

// A new format keeps this instance only when its parameter sets are the same bytes; otherwise VideoToolbox makes a
// fresh decoder for it.
static Boolean canAcceptFormatDescription(VTVideoDecoderRef ref, CMVideoFormatDescriptionRef format)
{
    Decoder *d = storage(ref);
    CFDataRef avcC = copyAvcC(format);
    const Boolean same = avcC && d->avcC && CFEqual(avcC, d->avcC);
    if (avcC) CFRelease(avcC);
    return same;
}

static OSStatus finishDelayedFrames(VTVideoDecoderRef ref)
{
    Decoder *d = storage(ref);
    pthread_mutex_lock(&d->lock);
    h264_decoder_flush(d->h264);
    pthread_mutex_unlock(&d->lock);
    return noErr;
}

// ---- CMBaseClass ---------------------------------------------------------------------------------------------------

static OSStatus invalidate(CMBaseObjectRef ref)
{
    Decoder *d = storage(ref);
    pthread_mutex_lock(&d->lock);
    d->session = NULL;  // pictures still waiting are released without being emitted
    pthread_mutex_unlock(&d->lock);
    return noErr;
}

static void finalize(CMBaseObjectRef ref)
{
    Decoder *d = storage(ref);
    d->session = NULL;
    h264_decoder_destroy(d->h264);  // frees its buffers through the client, so before closing it
    nvdec_close(d->client);
    if (d->avcC) CFRelease(d->avcC);
    if (d->matrix) CFRelease(d->matrix);
    if (d->primaries) CFRelease(d->primaries);
    if (d->transfer) CFRelease(d->transfer);
    pthread_mutex_destroy(&d->lock);
}

static CFStringRef copyDebugDescription(CMBaseObjectRef ref)
{
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("<AVD NVDEC H.264 decoder %p>"), ref);
}

static OSStatus copyProperty(CMBaseObjectRef ref, CFStringRef key, CFAllocatorRef allocator, void *value)
{
    (void)ref, (void)allocator;
    if (CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) {
        *(CFTypeRef *)value = CFRetain(kCFBooleanTrue);
        return noErr;
    }
    return -12783;  // kCMBaseObjectError_ValueNotAvailable
}

static OSStatus setProperty(CMBaseObjectRef ref, CFStringRef key, CFTypeRef value)
{
    (void)ref, (void)key, (void)value;  // real-time, thread count, ...: nothing to tune on a fixed-function engine
    return noErr;
}

static const VTPluginBaseClass kBaseClass = {
    .version = 1,
    .derivedStorageSize = sizeof(Decoder),
    .invalidate = invalidate,
    .finalize = finalize,
    .copyDebugDescription = copyDebugDescription,
    .copyProperty = copyProperty,
    .setProperty = setProperty,
};

static const VTPluginDecoderClass kDecoderClass = {
    .version = 1,
    .startSession = startSession,
    .decodeFrame = decodeFrame,
    .copySupportedPropertyDictionary = copySupportedPropertyDictionary,
    .canAcceptFormatDescription = canAcceptFormatDescription,
    .finishDelayedFrames = finishDelayedFrames,
};

static const VTPluginDecoderVTable kVTable = {NULL, &kBaseClass, &kDecoderClass};

static OSStatus createInstance(FourCharCode codec, CFAllocatorRef allocator, VTVideoDecoderRef *out)
{
    (void)codec;
    int error = 0;
    NVDECClient *client = nvdec_open(&error);
    if (!client) {  // VideoToolbox goes on to H264SW
        static int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            os_log_error(gLog, "AVD: TegraNVDEC refused this process (%#x); H.264 stays in software here", error);
        return kVTCouldNotFindVideoDecoderErr;
    }
    CMBaseObjectRef object = NULL;
    OSStatus status = CMDerivedObjectCreate(allocator, &kVTable, VTVideoDecoderGetClassID(), &object);
    if (status) {
        nvdec_close(client);
        return status;
    }
    Decoder *d = storage(object);
    memset(d, 0, sizeof(*d));
    pthread_mutex_init(&d->lock, NULL);
    d->client = client;
    const H264Backend backend = {d, bufferAlloc, bufferFree, jobSubmit, jobWait, decoded, emit, discard};
    if (!(d->h264 = h264_decoder_create(&backend))) {
        CFRelease(object);  // finalize closes the client
        return kVTAllocationFailedErr;
    }
    *out = object;
    return noErr;
}

__attribute__((visibility("default"))) void AVDRegister(void);

void AVDRegister(void)
{
    gLog = os_log_create("com.ioswitch.avd", "decoder");
    if (!nvdec_service_present()) return;  // switch-nvdec=3 not set, or NVDEC did not come up: nothing changes
    const OSStatus status = VTRegisterVideoDecoder(kCMVideoCodecType_H264, createInstance);
    if (status) os_log_error(gLog, "AVD: VTRegisterVideoDecoder('avc1') failed: %d", (int)status);
}
