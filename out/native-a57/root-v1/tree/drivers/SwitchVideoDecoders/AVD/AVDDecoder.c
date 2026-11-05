// SPDX-License-Identifier: BSD-2-Clause-Patent
// AVD.videodecoder: H.264 and VP9 (profile 0) on the Tegra X1's NVDEC as VideoToolbox's hardware 'avc1' and 'vp09'
// decoders.
//
// How VideoToolbox finds it (21H461 VTLoadVideoDecoders, disassembled): it dlopens a fixed list of paths and calls each
// one's register function: H264SW.videocodec first (Apple's NEON H.264, registered with IsHardwareAccelerated false),
// ..., HEVCH8, H8, VCPHEVC, then /System/Library/VideoDecoders/AVD.videodecoder (AVDRegister), the slot of Apple's AVD
// decoders on later SoCs. iPad7,1 ships no such file; the userland overlay puts this one there.
//
// Registration is per process and tells the truth. AVDRegister registers a codec only when the kernel published
// TegraNVDEC (it proved itself at boot, N4), proved that codec on the engine (N3: TegraNVDECInfo.codecs) and this
// process may open it (a probe connection, closed at once).
// VTIsHardwareDecodeSupported only asks the registry (VTSelectAndCreateVideoDecoderInstance with no out-pointer
// returns before any factory runs), so registering in a process the sandbox refuses would be a false YES there.
//
// Selection: VTRegisterVideoDecoder leaves IsHardwareAccelerated true and rating 0; _vtSortRegistryItemsByRating ranks
// hardware first, so this decoder is tried before H264SW, and a factory that fails moves VideoToolbox to the next one.
// StartSession is not retried with another decoder, so a stream NVDEC cannot take (interlaced, 4:2:2 or 4:4:4, high
// bit depth, slice groups, beyond 4096x4096) or one the arena has no room for runs on Apple's software decoder inside
// this instance: VTSelectAndCreateVideoDecoderInstance with hardware disabled picks it, every class call is forwarded,
// and UsingHardwareAcceleratedVideoDecoder answers false. Nothing that decoded before decodes worse now. VP9 has no
// software decoder in 21H461's VideoToolbox to fall back on (and needs kVTVideoDecoderSpecification_AllowVP9Decode,
// which VideoToolbox gives Fig servers and entitled tasks such as WebKit's), so a VP9 session this decoder cannot take
// (profile 1-3, beyond 4096x4096, below NVDEC's 144x144) fails in StartSession, which is also what clients see on
// iPads without VP9 hardware.
//
// Per picture (the scheduling): H264NVDEC parses and fills NVDEC's setup on the caller's decode thread; the kernel
// queues the job on host1x channel 0 and the thread sleeps on the syncpoint interrupt (no polling, no GPU, nothing on
// the compositor's threads); then the picture is copied (de-swizzled) on the same thread into a pixel buffer from the
// session's pool, NV12 '420v'/'420f', the format H264SW gives the same clients. VIC is not used: with the SMMU
// bypassed it can only write physically contiguous memory, which pool IOSurfaces are not, so its output would need
// this same CPU pass. Frames go out as soon as they are decoded, in decode order, as Apple's H.264 decoders do even
// under kVTDecodeFrame_EnableTemporalProcessing (observed on macOS 27 with test/vdectest): clients order by
// presentation time. H264NVDEC's display-order bumping stays available (kReorder).
//
// Log (subsystem com.ioswitch.avd, category decoder): per process, a refusal by TegraNVDEC; per session, one line when
// it starts (on NVDEC, or on the software decoder and why) and one when it ends (frames, mean decode and copy time);
// once per boot, the clip check (ClipCheck.c) in the first process that may open NVDEC, category selftest.
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "BlockLinear.h"
#include "ClipCheck.h"
#include "H264NVDEC.h"
#include "NVDECClient.h"
#include "VP9NVDEC.h"
#include "VTDecoderSPI.h"

typedef struct {
    pthread_mutex_t lock;
    VTVideoDecoderSession session;
    NVDECClient *client;
    FourCharCode codec;  // 'avc1' or 'vp09'
    H264Decoder *h264;
    VP9Decoder *vp9;
    VTVideoDecoderRef software;  // the session runs on Apple's software decoder (see above)
    CFDataRef avcC;              // or vpcC
    unsigned lengthSize;
    OSType pixelFormat;
    CFTypeRef matrix, primaries, transfer;
    uint32_t width, height, frames, errors;
    uint64_t decodeTicks, copyTicks;
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

static double ticksToMs(uint64_t ticks)
{
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    return (double)ticks * base.numer / base.denom / 1e6;
}

// ---- The software decoder this instance forwards to ----------------------------------------------------------------

static const VTPluginDecoderVTable *softwareVTable(const Decoder *d)
{
    return CMBaseObjectGetVTable(VTVideoDecoderGetCMBaseObject(d->software));
}

static OSStatus startSoftware(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format,
                              const char *why)
{
    const void *keys[] = {kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder};
    const void *values[] = {kCFBooleanFalse};
    CFDictionaryRef spec =
        CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTVideoDecoderRef software = NULL;
    OSStatus status =
        VTSelectAndCreateVideoDecoderInstance(kCMVideoCodecType_H264, kCFAllocatorDefault, spec, &software);
    CFRelease(spec);
    const VTPluginDecoderVTable *vtable =
        software ? CMBaseObjectGetVTable(VTVideoDecoderGetCMBaseObject(software)) : NULL;
    if (!status && (!vtable || !vtable->decoderClass || !vtable->decoderClass->startSession))
        status = kVTCouldNotFindVideoDecoderErr;
    if (!status) status = vtable->decoderClass->startSession(software, session, format);
    if (status) {
        if (software) CFRelease(software);
        os_log_error(gLog, "AVD: %{public}s: no software H.264 decoder either (%d)", why, (int)status);
        return status;
    }
    d->software = software;
    const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
    os_log(gLog, "AVD: session %dx%d in %{public}s on Apple's software H.264 decoder: %{public}s", size.width,
           size.height, getprogname(), why);
    return noErr;
}

// ---- H264Backend: pictures out ---------------------------------------------------------------------------------

static void *decoded(void *ctx, const H264Picture *picture)
{
    Decoder *d = ctx;
    Output *out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    out->frame = picture->token;
    out->status = picture->status ? kVTVideoDecoderBadDataErr : noErr;
    if (picture->status) d->errors++;
    CVPixelBufferPoolRef pool = d->session ? VTDecoderSessionGetPixelBufferPool(d->session) : NULL;
    if (!pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &out->image) != kCVReturnSuccess) {
        out->status = kVTVideoDecoderMalfunctionErr;
        return out;
    }
    const uint64_t began = mach_absolute_time();
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
    d->copyTicks += mach_absolute_time() - began;
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

static CFDataRef copyAtom(CMVideoFormatDescriptionRef format, CFStringRef name)
{
    CFDictionaryRef atoms =
        CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
    CFTypeRef atom = atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(atoms, name) : NULL;
    return atom && CFGetTypeID(atom) == CFDataGetTypeID() ? CFRetain(atom) : NULL;
}

static void setAttributes(Decoder *d, VTVideoDecoderSession session, bool fullRange, OSStatus *status)
{
    d->pixelFormat =
        fullRange ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    setNumber(attributes, kCVPixelBufferPixelFormatTypeKey, (int32_t)d->pixelFormat);
    setNumber(attributes, kCVPixelBufferWidthKey, (int32_t)d->width);
    setNumber(attributes, kCVPixelBufferHeightKey, (int32_t)d->height);
    setNumber(attributes, kCVPixelBufferBytesPerRowAlignmentKey, 64);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
    *status = VTDecoderSessionSetPixelBufferAttributes(session, attributes);
    CFRelease(surface);
    CFRelease(attributes);
}

// VP9 (WebM's or ISO BMFF's 'vpcC', VP Codec ISO Media File Format Binding 2.3): version 1, flags, profile, level,
// bitDepth(4) chromaSubsampling(3) videoFullRangeFlag(1), colourPrimaries, transferCharacteristics,
// matrixCoefficients. Profile 0 is 8-bit 4:2:0; a format without vpcC is taken as profile 0 and checked per frame.
static OSStatus startVP9(Decoder *d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format)
{
    const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
    d->avcC = copyAtom(format, CFSTR("vpcC"));
    const uint8_t *v = d->avcC && CFDataGetLength(d->avcC) >= 12 ? CFDataGetBytePtr(d->avcC) : NULL;
    const int profile = v ? v[4] : 0, depth = v ? v[6] >> 4 : 8, chroma = v ? (v[6] >> 1) & 7 : 1;
    const bool fullRange = v && (v[6] & 1);
    if (profile != 0 || depth != 8 || chroma > 1 || size.width < 144 || size.height < 144 || size.width > 4096 ||
        size.height > 4096) {
        os_log_error(gLog,
                     "AVD: VP9 session %dx%d in %{public}s refused: profile %d, %d-bit, chroma %d (NVDEC takes "
                     "profile 0, 4:2:0 8-bit, 144x144 to 4096x4096; VideoToolbox has no software VP9 here)",
                     size.width, size.height, getprogname(), profile, depth, chroma);
        return kVTVideoDecoderUnsupportedDataFormatErr;
    }
    if (vp9_decoder_configure(d->vp9, (uint32_t)size.width, (uint32_t)size.height)) {
        os_log_error(gLog, "AVD: VP9 session %dx%d in %{public}s: no room in the NVDEC arena (see the AVD alloc line)",
                     size.width, size.height, getprogname());
        return kVTVideoDecoderMalfunctionErr;
    }
    d->width = (uint32_t)size.width;
    d->height = (uint32_t)size.height;
    d->matrix = colourTag(format, kCMFormatDescriptionExtension_YCbCrMatrix, v ? v[9] : 2, 0, d->height);
    d->primaries = colourTag(format, kCMFormatDescriptionExtension_ColorPrimaries, v ? v[7] : 2, 1, 0);
    d->transfer = colourTag(format, kCMFormatDescriptionExtension_TransferFunction, v ? v[8] : 2, 2, 0);
    OSStatus status;
    setAttributes(d, session, fullRange, &status);
    if (!status)
        os_log(gLog, "AVD: session %ux%u in %{public}s on NVDEC (VP9 profile 0, level %d)", d->width, d->height,
               getprogname(), v ? v[5] : 0);
    return status;
}

static OSStatus startSession(VTVideoDecoderRef ref, VTVideoDecoderSession session, CMVideoFormatDescriptionRef format)
{
    Decoder *d = storage(ref);
    pthread_mutex_lock(&d->lock);
    d->session = session;
    if (d->vp9) {
        const OSStatus status = startVP9(d, session, format);
        pthread_mutex_unlock(&d->lock);
        return status;
    }
    d->avcC = copyAvcC(format);
    const int configured = d->avcC && !loadAvcC(d, d->avcC) ? h264_decoder_configure(d->h264) : -1;
    const H264SPS *sps = configured ? NULL : h264_decoder_sps(d->h264);
    OSStatus status;
    if (!sps) {
        // NVDEC's buffers go back to the arena for other sessions before the software decoder takes over.
        h264_decoder_destroy(d->h264);
        nvdec_close(d->client);
        d->h264 = NULL;
        d->client = NULL;
        status =
            startSoftware(d, session, format,
                          configured == -3 ? "no room in the NVDEC arena (see the AVD alloc/map line)"
                                           : "not decodable on NVDEC (progressive 4:2:0 8-bit up to 4096x4096 is)");
        pthread_mutex_unlock(&d->lock);
        return status;
    }
    d->width = (uint32_t)(sps->width_mbs * 16 - (int32_t)(sps->crop_left + sps->crop_right));
    d->height = (uint32_t)(sps->height_map_units * 16 - (int32_t)(sps->crop_top + sps->crop_bottom));
    const bool vui = sps->colour_description;
    d->pixelFormat = sps->full_range ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                                     : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    d->matrix = colourTag(format, kCMFormatDescriptionExtension_YCbCrMatrix, vui ? sps->matrix : 2, 0, d->height);
    d->primaries =
        colourTag(format, kCMFormatDescriptionExtension_ColorPrimaries, vui ? sps->colour_primaries : 2, 1, 0);
    d->transfer = colourTag(format, kCMFormatDescriptionExtension_TransferFunction, vui ? sps->transfer : 2, 2, 0);
    CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    setNumber(attributes, kCVPixelBufferPixelFormatTypeKey, (int32_t)d->pixelFormat);
    setNumber(attributes, kCVPixelBufferWidthKey, (int32_t)d->width);
    setNumber(attributes, kCVPixelBufferHeightKey, (int32_t)d->height);
    setNumber(attributes, kCVPixelBufferBytesPerRowAlignmentKey, 64);
    CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
    status = VTDecoderSessionSetPixelBufferAttributes(session, attributes);
    CFRelease(surface);
    CFRelease(attributes);
    if (!status)
        os_log(gLog, "AVD: session %ux%u in %{public}s on NVDEC (H.264 profile %u level %u, %u reference frames)",
               d->width, d->height, getprogname(), sps->profile_idc, sps->level_idc, sps->max_num_ref_frames);
    pthread_mutex_unlock(&d->lock);
    return status;
}

static OSStatus decodeFrame(VTVideoDecoderRef ref, VTVideoDecoderFrame frame, CMSampleBufferRef sample,
                            VTDecodeFrameFlags flags, VTDecodeInfoFlags *info)
{
    Decoder *d = storage(ref);
    if (d->software) return softwareVTable(d)->decoderClass->decodeFrame(d->software, frame, sample, flags, info);
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
    const uint64_t began = mach_absolute_time(), copied = d->copyTicks;
    // VP9: a superframe's hidden frames and its shown one; 0 when the sample showed none (answered like H.264's
    // parameter-set-only units).
    const int ret = d->vp9 ? vp9_decoder_decode(d->vp9, (const uint8_t *)data, size, frame)
                           : h264_decoder_decode(d->h264, (const uint8_t *)data, size, d->lengthSize, frame,
                                                 kReorder && (flags & kVTDecodeFrame_EnableTemporalProcessing));
    if (ret == 1) {
        d->frames++;
        d->decodeTicks += mach_absolute_time() - began - (d->copyTicks - copied);
    }
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

// VTDecompressionSessionCopyProperty hands a key it does not answer itself to the decoder only when the decoder lists
// it here (21H461, 0x1978a2438); UsingHardwareAcceleratedVideoDecoder is such a key, so it is listed, read-only, next
// to whatever the software decoder lists when this instance runs on it.
static OSStatus copySupportedPropertyDictionary(VTVideoDecoderRef ref, CFDictionaryRef *out)
{
    Decoder *d = storage(ref);
    CFDictionaryRef software = NULL;
    if (d->software && softwareVTable(d)->decoderClass->copySupportedPropertyDictionary)
        softwareVTable(d)->decoderClass->copySupportedPropertyDictionary(d->software, &software);
    CFMutableDictionaryRef supported =
        software ? CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, software)
                 : CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
    if (software) CFRelease(software);
    if (!supported) return kVTAllocationFailedErr;
    const void *keys[] = {kVTPropertyTypeKey, kVTPropertyReadWriteStatusKey};
    const void *values[] = {kVTPropertyType_Boolean, kVTPropertyReadWriteStatus_ReadOnly};
    CFDictionaryRef description = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 2,
                                                     &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(supported, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, description);
    CFRelease(description);
    *out = supported;
    return noErr;
}

// A new format keeps this instance only when its parameter sets are the same bytes; otherwise VideoToolbox makes a
// fresh decoder for it.
static Boolean canAcceptFormatDescription(VTVideoDecoderRef ref, CMVideoFormatDescriptionRef format)
{
    Decoder *d = storage(ref);
    if (d->software) {
        const VTPluginDecoderClass *software = softwareVTable(d)->decoderClass;
        return software->canAcceptFormatDescription && software->canAcceptFormatDescription(d->software, format);
    }
    if (d->vp9) {  // a new size or profile gets a fresh decoder; frames within one session may change size
        const CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
        CFDataRef vpcC = copyAtom(format, CFSTR("vpcC"));
        const Boolean same = (uint32_t)size.width == d->width && (uint32_t)size.height == d->height &&
                             (vpcC == d->avcC || (vpcC && d->avcC && CFEqual(vpcC, d->avcC)));
        if (vpcC) CFRelease(vpcC);
        return same;
    }
    CFDataRef avcC = copyAvcC(format);
    const Boolean same = avcC && d->avcC && CFEqual(avcC, d->avcC);
    if (avcC) CFRelease(avcC);
    return same;
}

static OSStatus finishDelayedFrames(VTVideoDecoderRef ref)
{
    Decoder *d = storage(ref);
    if (d->software) {
        const VTPluginDecoderClass *software = softwareVTable(d)->decoderClass;
        return software->finishDelayedFrames ? software->finishDelayedFrames(d->software) : noErr;
    }
    pthread_mutex_lock(&d->lock);
    if (d->h264) h264_decoder_flush(d->h264);  // VP9 frames go out as they are decoded
    pthread_mutex_unlock(&d->lock);
    return noErr;
}

// ---- CMBaseClass ---------------------------------------------------------------------------------------------------

static OSStatus invalidate(CMBaseObjectRef ref)
{
    Decoder *d = storage(ref);
    if (d->software) {
        const VTPluginBaseClass *software = softwareVTable(d)->baseClass;
        return software->invalidate ? software->invalidate(VTVideoDecoderGetCMBaseObject(d->software)) : noErr;
    }
    pthread_mutex_lock(&d->lock);
    d->session = NULL;  // pictures still waiting are released without being emitted
    pthread_mutex_unlock(&d->lock);
    return noErr;
}

static void finalize(CMBaseObjectRef ref)
{
    Decoder *d = storage(ref);
    d->session = NULL;
    if (d->frames)
        os_log(gLog,
               "AVD: session %ux%u in %{public}s ended: %u frames on NVDEC, %u with errors, mean %.2f ms decode + "
               "%.2f ms copy",
               d->width, d->height, getprogname(), d->frames, d->errors, ticksToMs(d->decodeTicks) / d->frames,
               ticksToMs(d->copyTicks) / d->frames);
    if (d->software) CFRelease(d->software);
    h264_decoder_destroy(d->h264);  // frees its buffers through the client, so before closing it
    vp9_decoder_destroy(d->vp9);
    nvdec_close(d->client);
    if (d->avcC) CFRelease(d->avcC);
    if (d->matrix) CFRelease(d->matrix);
    if (d->primaries) CFRelease(d->primaries);
    if (d->transfer) CFRelease(d->transfer);
    pthread_mutex_destroy(&d->lock);
}

static CFStringRef copyDebugDescription(CMBaseObjectRef ref)
{
    Decoder *d = storage(ref);
    return CFStringCreateWithFormat(kCFAllocatorDefault, NULL, CFSTR("<AVD %s decoder %p on %s>"),
                                    d->vp9 ? "VP9" : "H.264", ref, d->software ? "Apple's software decoder" : "NVDEC");
}

static OSStatus copyProperty(CMBaseObjectRef ref, CFStringRef key, CFAllocatorRef allocator, void *value)
{
    Decoder *d = storage(ref);
    if (CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) {
        *(CFTypeRef *)value = CFRetain(d->software ? kCFBooleanFalse : kCFBooleanTrue);
        return noErr;
    }
    if (d->software && softwareVTable(d)->baseClass->copyProperty)
        return softwareVTable(d)->baseClass->copyProperty(VTVideoDecoderGetCMBaseObject(d->software), key, allocator,
                                                          value);
    return -12783;  // kCMBaseObjectError_ValueNotAvailable
}

static OSStatus setProperty(CMBaseObjectRef ref, CFStringRef key, CFTypeRef value)
{
    Decoder *d = storage(ref);
    if (d->software && softwareVTable(d)->baseClass->setProperty)
        return softwareVTable(d)->baseClass->setProperty(VTVideoDecoderGetCMBaseObject(d->software), key, value);
    return noErr;  // real-time, thread count, ...: nothing to tune on a fixed-function engine
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

// Version 1: the slots VideoToolbox reads from every decoder. H264SW declares version 3 and leaves setProperties,
// copySerializableProperties and finishDelayedFrames empty, so VideoToolbox checks the optional slots before calling.
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
    int error = 0;
    NVDECClient *client = nvdec_open(&error);
    if (client && codec == kCMVideoCodecType_VP9 && !(nvdec_codecs(client) & (1u << TEGRA_NVDEC_CODEC_VP9))) {
        nvdec_close(client);  // VP9 was turned off after this process registered it (its clip check failed)
        return kVTCouldNotFindVideoDecoderErr;
    }
    if (!client) {  // VideoToolbox goes on to H264SW
        static int logged;
        if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
            os_log_error(gLog, "AVD: TegraNVDEC refused %{public}s (%#x) after registration; H.264 goes to software",
                         getprogname(), error);
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
    d->codec = codec;
    const H264Backend backend = {d, bufferAlloc, bufferFree, jobSubmit, jobWait, decoded, emit, discard};
    if (codec == kCMVideoCodecType_VP9 ? !(d->vp9 = vp9_decoder_create(&backend))
                                       : !(d->h264 = h264_decoder_create(&backend))) {
        CFRelease(object);  // finalize closes the client
        return kVTAllocationFailedErr;
    }
    *out = object;
    return noErr;
}

// The clip checks, once per boot, in the process that claimed them from the kernel. VP9 runs on a private registration
// of this decoder (not flagged hardware, so no VTIsHardwareDecodeSupported('vp09') answers YES from it; named, so only
// the check selects it); the verdict goes to the kernel, and 'vp09' is offered (here and in every other process) only
// when the clip matched libvpx. A failure turns VP9 off: TegraNVDECInfo.codecs loses it and Submit refuses it.
#define kCheckImplementation CFSTR("com.ioswitch.avd.check")
#define kCheckDecoderID      CFSTR("com.ioswitch.avd.check.vp09")

typedef struct {
    NVDECClient *probe;  // holds the claim until the verdict is reported
    bool vp9;
} Check;

static void logCheck(bool ok, const char *line)
{
    os_log_t log = os_log_create("com.ioswitch.avd", "selftest");
    if (ok)
        os_log(log, "vdectest: %{public}s (in %{public}s)", line, getprogname());
    else
        os_log_error(log, "vdectest: %{public}s (in %{public}s)", line, getprogname());
}

static void selfTest(void *context)
{
    Check *check = context;
    char line[512];
    logCheck(clip_check(ClipCheckHardware, NULL, line, sizeof(line)), line);
    bool vp9Ok = false;
    if (check->vp9) {
        const void *keys[] = {kVTVideoDecoderSpecification_DecoderID}, *values[] = {kCheckDecoderID};
        CFDictionaryRef spec =
            CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        vp9Ok = clip_check(ClipCheckVP9, spec, line, sizeof(line));
        CFRelease(spec);
        logCheck(vp9Ok, line);
    }
    nvdec_check_report(check->probe, vp9Ok);
    if (vp9Ok) {
        const OSStatus status = VTRegisterVideoDecoder(kCMVideoCodecType_VP9, createInstance);
        if (status) os_log_error(gLog, "AVD: VTRegisterVideoDecoder('vp09') failed: %d", (int)status);
    } else if (check->vp9) {
        os_log_error(gLog, "AVD: VP9 off until reboot (the clip check failed); sites fall back to H.264");
    }
    nvdec_close(check->probe);
    free(check);
}

static OSStatus registerCheckDecoder(void)
{
    const void *keys[] = {CFSTR("CMClassImplementationID"), kVTVideoDecoder_IsHardwareAccelerated};
    const void *values[] = {kCheckImplementation, kCFBooleanFalse};
    CFDictionaryRef info =
        CFDictionaryCreate(NULL, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus status = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_VP9, info, createInstance);
    CFRelease(info);
    return status;
}

__attribute__((visibility("default"))) void AVDRegister(void);

void AVDRegister(void)
{
    gLog = os_log_create("com.ioswitch.avd", "decoder");
    if (!nvdec_service_present()) return;  // switch-nvdec=0, or NVDEC did not prove itself at boot: nothing changes
    int error = 0;
    NVDECClient *probe = nvdec_open(&error);
    if (!probe) {
        os_log(gLog, "AVD: TegraNVDEC refuses %{public}s (%#x): H.264 stays on Apple's software decoder here",
               getprogname(), error);
        return;
    }
    const OSStatus status = VTRegisterVideoDecoder(kCMVideoCodecType_H264, createInstance);
    if (status) os_log_error(gLog, "AVD: VTRegisterVideoDecoder('avc1') failed: %d", (int)status);
    // VP9 needs the kernel's N3 VP9 picture (it matched libvpx this boot) and then this boot's clip check.
    const bool vp9Proven = nvdec_codecs(probe) & (1u << TEGRA_NVDEC_CODEC_VP9);
    bool claimed = false;
    uint32_t state = nvdec_check_claim(probe, &claimed);
    if (!claimed && state == TEGRA_NVDEC_CHECK_RUNNING) {
        state = nvdec_check_wait(probe);  // another process runs it (well under a second)
        if (state == TEGRA_NVDEC_CHECK_UNCLAIMED) state = nvdec_check_claim(probe, &claimed);  // it gave up the claim
    }
    if (claimed) {
        Check *check = calloc(1, sizeof(*check));
        if (check) {
            check->probe = probe;
            check->vp9 = vp9Proven && !registerCheckDecoder();
            // Off the registry's thread: the check's sessions look the registry up once this registration returns.
            dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), check, selfTest);
            return;
        }
        nvdec_check_report(probe, false);
    } else if (vp9Proven && state == TEGRA_NVDEC_CHECK_PASSED) {
        const OSStatus vp9Status = VTRegisterVideoDecoder(kCMVideoCodecType_VP9, createInstance);
        if (vp9Status) os_log_error(gLog, "AVD: VTRegisterVideoDecoder('vp09') failed: %d", (int)vp9Status);
    } else if (vp9Proven) {
        os_log(gLog, "AVD: VP9 not offered in %{public}s: the clip check %{public}s", getprogname(),
               state == TEGRA_NVDEC_CHECK_FAILED ? "failed" : "gave no verdict within 3 s");
    }
    nvdec_close(probe);
}
