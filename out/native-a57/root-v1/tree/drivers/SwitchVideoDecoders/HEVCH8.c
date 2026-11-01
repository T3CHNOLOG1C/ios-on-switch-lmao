// SPDX-License-Identifier: BSD-2-Clause-Patent
// HEVCH8.videodecoder: Apple's own software HEVC decoder, registered in VideoToolbox's hardware slot.
//
// How 21H461 decodes HEIC/HEVC on iPad7,1 (disassembly of the dyld cache):
//   ImageIO HEIFReadPlugin -> CMPhoto (dlsym'd) -> VTDecompressionSession / VTTileDecompressionSession -> a VideoToolbox
//   decoder for 'hvc1'. VideoToolbox fills its decoder registry once per process (vtInitializeVideoDecoderRegistry ->
//   VTLoadVideoDecoders), which dlopens a fixed table of paths and calls each one's register function:
//     /System/Library/VideoDecoders/H264H8.videodecoder  H264H8Register: registers the A10X hardware H.264 and HEVC
//         decoders only when IOServiceMatching("AppleD5500") finds the VXD393 kext's service. There is none here.
//     /System/Library/VideoCodecs/VCPHEVC.videocodec   VCPHEVCRegisterDecoder: always registers the software decoder
//         HEVCVideoDecoder_CreateInstance for 'hvc1' with kVTVideoDecoder_IsHardwareAccelerated = false.
//     /System/Library/VideoDecoders/HEVCH8.videodecoder  HEVCH8Register: a slot this SoC does not ship (the path is
//         not in the dyld cache), so dlopen looks on disk. This file sits there through the userland overlay.
//   With no options, ImageIO asks CMPhoto for AccelerationMode 0 (auto) and VideoToolbox falls back to the software
//   decoder by itself. But kCGImageSourceUseHardwareAcceleration = true (QuickLook previews, PhotoLibraryServices
//   thumbnail rebuilds) becomes AccelerationMode 2, CMPhoto's RequireHardwareCodec, and
//   kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: those decodes fail without a hardware entry,
//   and VTIsHardwareDecodeSupported('hvc1') answers false (MediaToolbox asks it for video).
//
// So this registers the same software decoder again through VTRegisterVideoDecoder, which leaves
// kVTVideoDecoder_IsHardwareAccelerated at its default, true. Every request then finds a decoder, and all of them
// run Apple's NEON decoder on the CPU (tile decoding included). No Apple binary is patched. VTRegisterVideoDecoder and
// HEVCVideoDecoder_CreateInstance are exported but not in the SDK, so both come from dlsym. An NVDEC-backed decoder
// can take this slot later.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <os/log.h>

typedef OSStatus (*CreateInstance)(FourCharCode codecType, CFAllocatorRef allocator, void **decoderOut);
typedef OSStatus (*RegisterDecoder)(FourCharCode codecType, CreateInstance create);

__attribute__((visibility("default"))) void HEVCH8Register(void);

void HEVCH8Register(void)
{
    os_log_t log = os_log_create("com.ioswitch.hevc", "register");
    void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
    CreateInstance create = codec ? (CreateInstance)dlsym(codec, "HEVCVideoDecoder_CreateInstance") : NULL;
    RegisterDecoder registerDecoder = (RegisterDecoder)dlsym(RTLD_DEFAULT, "VTRegisterVideoDecoder");
    if (!create || !registerDecoder) {
        os_log_error(log, "HEVCH8: no %{public}s, HEVC stays software-only", create ? "VTRegisterVideoDecoder" :
                     "HEVCVideoDecoder_CreateInstance in VCPHEVC.videocodec");
        return;
    }
    OSStatus status = registerDecoder('hvc1', create);
    if (status)
        os_log_error(log, "HEVCH8: VTRegisterVideoDecoder('hvc1') failed: %d", (int)status);
    else
        os_log(log, "HEVCH8: software HEVC (VCPHEVC) registered as the hardware 'hvc1' decoder");
}
