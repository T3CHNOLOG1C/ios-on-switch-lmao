// SPDX-License-Identifier: BSD-2-Clause-Patent
// The clip checks: test/VDECClip.h's H.264 clip (320x240, 30 frames, High profile with B-pyramid and 3 references) and
// test/VP9Clip.h's VP9 clip, decoded through VideoToolbox, every output frame's luma CRC compared with the reference
// encoder's reconstruction (x264) or decoder's output (libvpx).
//
// On the Switch, AVD.videodecoder runs it once per boot (the first process that may open TegraNVDEC claims it from the
// kernel), requiring the hardware decoder: that proves the whole path, VideoToolbox -> this plug-in -> TegraNVDEC ->
// NVDEC -> the de-swizzle, against an independent reference. launchd cannot start a job of ours for it: 21H461's
// launchd reads its signed cache (/System/Library/xpc/launchd.plist), not the LaunchDaemons the overlay could show.
// test/vdectest runs the same check on a Mac against Apple's decoders, and the host tests drive it through this
// plug-in's VideoToolbox plumbing.
#ifndef CLIP_CHECK_H
#define CLIP_CHECK_H
#include <CoreFoundation/CoreFoundation.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ClipCheckHardware,  // kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder
    ClipCheckSoftware,  // kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder = false
    ClipCheckVP9,       // test/VP9Clip.h (512x288, superframes, segmentation, 2 tile columns) on a hardware 'vp09'
                        // decoder, against libvpx's output
} ClipCheckMode;

// One result line ("hardware: hardware decoder, 30/30 frames match x264, ... OK"); true when every frame matched and,
// for ClipCheckHardware, the session reported a hardware decoder. extraSpec (NULL on the device) adds decoder
// specification entries: the host test names the plug-in under test among the Mac's own decoders.
bool clip_check(ClipCheckMode mode, CFDictionaryRef extraSpec, char *line, size_t size);

#endif
