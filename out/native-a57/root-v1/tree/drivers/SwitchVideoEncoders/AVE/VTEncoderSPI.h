// SPDX-License-Identifier: BSD-2-Clause-Patent
// VideoToolbox's encoder plug-in interface (exported by VideoToolbox and CoreMedia, not in the SDK headers). The CMBase
// part is VTDecoderSPI.h's. The layout and signatures are 21H461's own: H264H9.videoencoder (Apple's AVE encoder, in
// the dyld cache) keeps C++ names, so its H3H264VideoEncoderVTable at 0x22ab2a4c8 reads {0, CMBaseClass (version 1,
// invalidate/finalize/copyProperty/setProperty at +0x18/+0x20/+0x30/+0x38), VTVideoEncoderClass at 0x22ab2a548
// (version 3): StartSession(VTVideoEncoder*, VTVideoEncoderSession*, CMVideoDimensions, CFTypeRef) +8,
// EncodeFrame(VTVideoEncoder*, VTVideoEncoderFrame*, CVBuffer*, CMTime, CMTime, CFDictionary const*, unsigned*) +0x10,
// CompleteFrames(VTVideoEncoder*, CMTime) +0x18, CopySupportedPropertyDictionary +0x20, SetProperties +0x28,
// PrepareToEncodeFrames +0x38, BeginPass/EndPass +0x40/+0x48}. VideoToolbox's own MuxedAlpha encoder declares
// version 1 with only the first four slots. Factories take (codec type, allocator, out) (_TestIPBVideoEncoder_
// CreateInstance); VTEncoderSessionCreateVideoFormatDescription takes (session, codec, width, height, extensions,
// out) and VTEncoderSessionEmitEncodedFrame (session, frame, status, info flags, sample buffer) (both disassembled).
#ifndef VT_ENCODER_SPI_H
#define VT_ENCODER_SPI_H
#include "../../SwitchVideoDecoders/AVD/VTDecoderSPI.h"

typedef struct OpaqueVTVideoEncoderSession *VTVideoEncoderSession;
typedef struct OpaqueVTVideoEncoderFrame *VTVideoEncoderFrame;
typedef CMBaseObjectRef VTVideoEncoderRef;

typedef struct {
    uint32_t version;
    OSStatus (*startSession)(VTVideoEncoderRef, VTVideoEncoderSession, CMVideoDimensions, CFTypeRef);
    OSStatus (*encodeFrame)(VTVideoEncoderRef, VTVideoEncoderFrame, CVImageBufferRef, CMTime, CMTime, CFDictionaryRef,
                            VTEncodeInfoFlags *);
    OSStatus (*completeFrames)(VTVideoEncoderRef, CMTime);
    OSStatus (*copySupportedPropertyDictionary)(VTVideoEncoderRef, CFDictionaryRef *);
    OSStatus (*setProperties)(VTVideoEncoderRef, CFDictionaryRef);
    OSStatus (*copySerializableProperties)(VTVideoEncoderRef, CFAllocatorRef, CFDictionaryRef *);
    OSStatus (*prepareToEncodeFrames)(VTVideoEncoderRef);
    void *reserved[16];
} VTPluginEncoderClass;

typedef struct {
    const void *reserved;
    const VTPluginBaseClass *baseClass;
    const VTPluginEncoderClass *encoderClass;
} VTPluginEncoderVTable;

_Static_assert(offsetof(VTPluginEncoderClass, encodeFrame) == 0x10, "VTVideoEncoderClass");
_Static_assert(offsetof(VTPluginEncoderClass, copySupportedPropertyDictionary) == 0x20, "VTVideoEncoderClass");
_Static_assert(offsetof(VTPluginEncoderClass, prepareToEncodeFrames) == 0x38, "VTVideoEncoderClass");

typedef OSStatus (*VTEncoderCreateInstance)(FourCharCode codecType, CFAllocatorRef allocator, VTVideoEncoderRef *out);

extern OSStatus VTRegisterVideoEncoderWithInfo(FourCharCode codecType, CFDictionaryRef info,
                                               VTEncoderCreateInstance create);
extern CMBaseClassID VTVideoEncoderGetClassID(void);
extern CMBaseObjectRef VTVideoEncoderGetCMBaseObject(VTVideoEncoderRef encoder);
extern OSStatus VTEncoderSessionSetPixelBufferAttributes(VTVideoEncoderSession session, CFDictionaryRef attributes);
extern OSStatus VTEncoderSessionCreateVideoFormatDescription(VTVideoEncoderSession session, CMVideoCodecType codec,
                                                             int32_t width, int32_t height, CFDictionaryRef extensions,
                                                             CMVideoFormatDescriptionRef *out);
extern OSStatus VTEncoderSessionEmitEncodedFrame(VTVideoEncoderSession session, VTVideoEncoderFrame frame,
                                                 OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample);

extern const CFStringRef kVTVideoEncoder_IsHardwareAccelerated;
extern const CFStringRef kVTVideoEncoder_EncoderNameKey;
extern const CFStringRef kVTVideoEncoderList_EncoderID;

#endif
