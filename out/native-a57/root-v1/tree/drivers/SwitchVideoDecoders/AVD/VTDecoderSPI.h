// SPDX-License-Identifier: BSD-2-Clause-Patent
// VideoToolbox's decoder plug-in interface (exported by VideoToolbox and CoreMedia, not in the SDK headers).
// The shapes are those WebKit's VP9 decoder and others declare; the layout is checked against 21H461 itself:
// H264SW.videocodec's vtable at 0x231e9c248 = {0, CMBaseClass at 0x231e9c260 (version 1, derived storage 0x480 at
// +8, finalize/copyDebugDescription/copyProperty/setProperty at +0x20..+0x38), VTVideoDecoderClass at 0x231e9c2c8
// (version 3, startSession +8, decodeFrame +0x10, copySupportedPropertyDictionary +0x18, canAccept +0x30)}. Naturally
// aligned on arm64 (not the pack(4) of the x86 headers), so every slot is 8 bytes.
#ifndef VT_DECODER_SPI_H
#define VT_DECODER_SPI_H
#include <stddef.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef struct OpaqueCMBaseClass *CMBaseClassID;
typedef struct OpaqueVTVideoDecoderSession *VTVideoDecoderSession;
typedef struct OpaqueVTVideoDecoderFrame *VTVideoDecoderFrame;
typedef CMBaseObjectRef VTVideoDecoderRef;

typedef struct {
    uint32_t version;
    size_t derivedStorageSize;
    Boolean (*equal)(CMBaseObjectRef, CMBaseObjectRef);
    OSStatus (*invalidate)(CMBaseObjectRef);
    void (*finalize)(CMBaseObjectRef);
    CFStringRef (*copyDebugDescription)(CMBaseObjectRef);
    OSStatus (*copyProperty)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
    OSStatus (*setProperty)(CMBaseObjectRef, CFStringRef, CFTypeRef);
    OSStatus (*notificationBarrier)(CMBaseObjectRef);
    const void *protocolTable;
} VTPluginBaseClass;

typedef struct {
    uint32_t version;
    OSStatus (*startSession)(VTVideoDecoderRef, VTVideoDecoderSession, CMVideoFormatDescriptionRef);
    OSStatus (*decodeFrame)(VTVideoDecoderRef, VTVideoDecoderFrame, CMSampleBufferRef, VTDecodeFrameFlags,
                            VTDecodeInfoFlags *);
    OSStatus (*copySupportedPropertyDictionary)(VTVideoDecoderRef, CFDictionaryRef *);
    OSStatus (*setProperties)(VTVideoDecoderRef, CFDictionaryRef);
    OSStatus (*copySerializableProperties)(VTVideoDecoderRef, CFAllocatorRef, CFDictionaryRef *);
    Boolean (*canAcceptFormatDescription)(VTVideoDecoderRef, CMVideoFormatDescriptionRef);
    OSStatus (*finishDelayedFrames)(VTVideoDecoderRef);
    void *reserved[3];
} VTPluginDecoderClass;

typedef struct {
    const void *reserved;
    const VTPluginBaseClass *baseClass;
    const VTPluginDecoderClass *decoderClass;
} VTPluginDecoderVTable;

_Static_assert(offsetof(VTPluginBaseClass, derivedStorageSize) == 0x8, "CMBaseClass");
_Static_assert(offsetof(VTPluginBaseClass, finalize) == 0x20, "CMBaseClass");
_Static_assert(offsetof(VTPluginBaseClass, setProperty) == 0x38, "CMBaseClass");
_Static_assert(offsetof(VTPluginDecoderClass, decodeFrame) == 0x10, "VTVideoDecoderClass");
_Static_assert(offsetof(VTPluginDecoderClass, canAcceptFormatDescription) == 0x30, "VTVideoDecoderClass");

typedef OSStatus (*VTDecoderCreateInstance)(FourCharCode codecType, CFAllocatorRef allocator, VTVideoDecoderRef *out);

extern OSStatus VTRegisterVideoDecoder(FourCharCode codecType, VTDecoderCreateInstance create);
extern CMBaseClassID VTVideoDecoderGetClassID(void);
extern OSStatus CMDerivedObjectCreate(CFAllocatorRef allocator, const void *vtable, CMBaseClassID classID,
                                      CMBaseObjectRef *out);
extern void *CMBaseObjectGetDerivedStorage(CMBaseObjectRef object);
extern const void *CMBaseObjectGetVTable(CMBaseObjectRef object);  // a VTPluginDecoderVTable for a decoder
extern CMBaseObjectRef VTVideoDecoderGetCMBaseObject(VTVideoDecoderRef decoder);
// The registry's choice for a codec under a decoder specification (21H461 VideoToolbox, exported): with an out-pointer
// it runs the candidates' factories in rating order until one succeeds; with none it only answers whether one exists.
extern OSStatus VTSelectAndCreateVideoDecoderInstance(FourCharCode codecType, CFAllocatorRef allocator,
                                                      CFDictionaryRef specification, VTVideoDecoderRef *out);
extern OSStatus VTDecoderSessionSetPixelBufferAttributes(VTVideoDecoderSession session, CFDictionaryRef attributes);
extern CVPixelBufferPoolRef VTDecoderSessionGetPixelBufferPool(VTVideoDecoderSession session);
extern OSStatus VTDecoderSessionEmitDecodedFrame(VTVideoDecoderSession session, VTVideoDecoderFrame frame,
                                                 OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image);

#endif
