// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for AVD.videodecoder's VideoToolbox side, on a Mac: the plug-in itself (AVDDecoder.c, included for its
// factory) over a fake TegraNVDEC client, registered in this process and driven by the clip check (ClipCheck.c)
// through VideoToolbox, which checks the plug-in's class layout and session calls against a real VideoToolbox.
//   engine: the fake client hands out buffers and checks every submit with the kernel's checker; its engine writes
//           nothing, so pictures come out black: the test wants all 30 out of a session that says hardware.
//   full:   the fake arena refuses every buffer, so the instance runs Apple's software decoder: the test wants 30/30
//           frames matching x264 from a session that says software.
//   vp9:    VP9Clip.h as 'vp09' (kVTVideoDecoderSpecification_AllowVP9Decode) on AVD's private check registration
//           (registerCheckDecoder: not flagged hardware, named com.brunostjohn.switch.avd.check.vp09, as the device's boot check
//           uses it) and the fake engine: every superframe and frame through VP9NVDEC and the kernel's checker, all 16
//           shown frames out of a session that says hardware.
//   present: the engine session again with TegraNVDECPresent on: every picture handed to the fake VIC, whose pool
//           buffer must be one VIC addresses (rows and the chroma plane on 256 bytes) and which copies it as VIC would;
//           no wait invalidates the surface, and the CPU path's pictures come out.
//   gate:   the private registration outside an open check (gCheckOpen false): no instance, so no session by its name.
//   hevc:   HEVCClip.h's Main clips A and V and Main 10 clip T as 'hvc1' on the private check registration (named
//           com.brunostjohn.switch.avd.check.hvc1) and the fake engine: every picture through HEVCNVDEC and the kernel's checker,
//           all out of a hardware session; then with the arena full, Apple's software HEVC decoder in the instance,
//           matching FFmpeg.
// Prints "PASS avd ..." when all hold (avd_host_test).
#include "../../AVD/AVDDecoder.c"
#include <stdio.h>
#include "../../../TegraNVDEC/TegraNVDECCheck.h"

struct NVDECClient {
    uint8_t *va[64];
    uint64_t size[64];
};

static bool gFull, gPresentOn;
static int gOpens, gSubmits, gRefused, gPresents, gPresentBad, gWaitRanges;

bool nvdec_service_present(void)
{
    return true;
}

NVDECClient *nvdec_open(int *error)
{
    (void)error;
    gOpens++;
    return calloc(1, sizeof(NVDECClient));
}

void nvdec_close(NVDECClient *c)
{
    if (!c)
        return;
    for (int i = 0; i < 64; i++)
        free(c->va[i]);
    free(c);
}

uint32_t nvdec_codecs(const NVDECClient *c)
{
    (void)c;
    return 1u << TEGRA_NVDEC_CODEC_H264 | 1u << TEGRA_NVDEC_CODEC_VP9 | 1u << TEGRA_NVDEC_CODEC_HEVC |
           TEGRA_NVDEC_HEVC_MAIN10;
}

uint32_t nvdec_codecs_now(NVDECClient *c)
{
    return nvdec_codecs(c);
}

bool nvdec_can_present(const NVDECClient *c)
{
    (void)c;
    return gPresentOn;
}

// The fake VIC: TegraNVDECUserClient's geometry checks and TegraVICConfig.h's address rules for the destination, then
// the copy (BlockLinear.c, as VIC's pass-through would leave it).
int nvdec_present(NVDECClient *c, const TegraNVDECPresent *p)
{
    gPresents++;
    const uint64_t luma = (uint64_t)p->pitch * p->rows;
    IOSurfaceRef s = IOSurfaceLookup(p->surface_id);
    bool ok = p->handle < 64 && c->va[p->handle] && p->pitch && !(p->pitch % 64) && p->rows && !(p->rows % 32) &&
              p->block_height_log2 == 1 && !(p->chroma_offset % 256) && p->chroma_offset >= luma &&
              p->chroma_offset + luma / 2 <= c->size[p->handle] && s;
    if (ok) {
        const uintptr_t base = (uintptr_t)IOSurfaceGetBaseAddress(s);
        ok = !(IOSurfaceGetBytesPerRowOfPlane(s, 0) % 256) && !(IOSurfaceGetBytesPerRowOfPlane(s, 1) % 256) &&
             !(((uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, 0) - base) % 256) &&
             !(((uintptr_t)IOSurfaceGetBaseAddressOfPlane(s, 1) - base) % 256) && p->width <= IOSurfaceGetWidth(s) &&
             p->height <= IOSurfaceGetHeight(s) && !((p->width | p->height) & 1);
    }
    if (ok) {
        IOSurfaceLock(s, 0, NULL);
        const uint8_t *picture = c->va[p->handle];
        block_linear_copy(IOSurfaceGetBaseAddressOfPlane(s, 0), IOSurfaceGetBytesPerRowOfPlane(s, 0), picture, p->pitch,
                          p->crop_x, p->crop_y, p->width, p->height);
        block_linear_copy(IOSurfaceGetBaseAddressOfPlane(s, 1), IOSurfaceGetBytesPerRowOfPlane(s, 1),
                          picture + p->chroma_offset, p->pitch, p->crop_x, p->crop_y / 2, p->width, p->height / 2);
        IOSurfaceUnlock(s, 0, NULL);
    } else {
        gPresentBad++;
    }
    if (s)
        CFRelease(s);
    return ok ? 0 : kIOReturnUnsupported;
}

uint32_t nvdec_check_claim(NVDECClient *c, bool *claimed)
{
    (void)c;
    *claimed = false;
    return TEGRA_NVDEC_CHECK_PASSED;
}

int nvdec_check_report(NVDECClient *c, bool vp9Ok, bool hevcOk, bool hevc10Ok)
{
    (void)c, (void)vp9Ok, (void)hevcOk, (void)hevc10Ok;
    return 0;
}

uint32_t nvdec_check_wait(NVDECClient *c)
{
    (void)c;
    return TEGRA_NVDEC_CHECK_PASSED;
}

int nvdec_alloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    NVDECClient *c = ctx;
    if (gFull)
        return -1;
    for (uint32_t h = 1; h < 64; h++) {
        if (c->va[h])
            continue;
        if (!(c->va[h] = calloc(1, size)))
            return -1;
        c->size[h] = size;
        *handle = h;
        *va = c->va[h];
        return 0;
    }
    return -1;
}

void nvdec_free(void *ctx, uint32_t handle)
{
    NVDECClient *c = ctx;
    free(c->va[handle]);
    c->va[handle] = NULL;
}

int nvdec_submit(void *ctx, const TegraNVDECSubmit *s, uint32_t *fence)
{
    NVDECClient *c = ctx;
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES] = {0};
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC)
            continue;
        if (e->value >= 64 || !c->va[e->value] || e->offset > c->size[e->value])
            return -1;
        avail[i] = c->size[e->value] - e->offset;
    }
    if (tegra_nvdec_check(s, avail)) {
        gRefused++;
        return -1;
    }
    *fence = (uint32_t)++gSubmits;
    return 0;
}

int nvdec_wait(void *ctx, const TegraNVDECWait *w)
{
    (void)ctx;
    gWaitRanges += (int)w->invalidate_count;
    return 0;
}

int main(void)
{
    gLog = os_log_create("com.brunostjohn.switch.avd", "host-test");
    setvbuf(stdout, NULL, _IONBF, 0);  // a hung session still shows how far it got
    // A class ID of our own makes the registry entry "com.brunostjohn.switch.avd-host.avc1", which the sessions name below: the
    // Mac's own H.264 decoders would otherwise win the selection.
    const void *keys[] = {CFSTR("CMClassImplementationID")}, *values[] = {CFSTR("com.brunostjohn.switch.avd-host")};
    CFDictionaryRef info =
        CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus registered = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_H264, info, createInstance) ||
                                registerCheckDecoder(kCMVideoCodecType_VP9) ||
                                registerCheckDecoder(kCMVideoCodecType_HEVC);
    CFRelease(info);
    // This process is the check (its private registration makes instances only while one is open).
    __atomic_store_n(&gClaimer, true, __ATOMIC_RELEASE);
    __atomic_store_n(&gCheckOpen, true, __ATOMIC_RELEASE);
    if (registered) {
        printf("FAIL avd: VTRegisterVideoDecoderWithInfo %d\n", (int)registered);
        return 1;
    }
    const void *specKeys[] = {kVTVideoDecoderSpecification_DecoderID},
               *specValues[] = {CFSTR("com.brunostjohn.switch.avd-host.avc1")};
    CFDictionaryRef spec = CFDictionaryCreate(NULL, specKeys, specValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    char engine[512], full[512], present[512];
    clip_check(ClipCheckHardware, spec, engine, sizeof(engine));
    const int engineOpens = gOpens, engineSubmits = gSubmits;
    // VIC's present: the same session with the feature on.
    gPresentOn = true;
    const int presentSubmits = gSubmits, presentWaits = gWaitRanges;
    clip_check(ClipCheckHardware, spec, present, sizeof(present));
    gPresentOn = false;
    const bool presentOk = strstr(present, "hardware: hardware decoder") && strstr(present, " 30 out (") &&
                           gSubmits - presentSubmits == 30 && gPresents == 30 && !gPresentBad &&
                           gWaitRanges - presentWaits == 30;  // the status alone, no surface
    printf("present: %s (presents %d, refused %d, wait ranges %d)\n", present, gPresents, gPresentBad,
           gWaitRanges - presentWaits);
    gFull = true;
    clip_check(ClipCheckHardware, spec, full, sizeof(full));
    CFRelease(spec);
    gFull = false;
    char vp9[512];
    const int vp9Submits = gSubmits;
    const void *vp9Keys[] = {kVTVideoDecoderSpecification_DecoderID}, *vp9Values[] = {kCheckDecoderID};
    CFDictionaryRef vp9Spec = CFDictionaryCreate(NULL, vp9Keys, vp9Values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    clip_check(ClipCheckVP9, vp9Spec, vp9, sizeof(vp9));
    printf("vp9: %s (submits %d)\n", vp9, gSubmits - vp9Submits);
    // Outside an open check the private registration makes no instance: VideoToolbox finds no decoder by that name.
    char closed[512];
    __atomic_store_n(&gCheckOpen, false, __ATOMIC_RELEASE);
    const int closedSubmits = gSubmits;
    clip_check(ClipCheckVP9, vp9Spec, closed, sizeof(closed));
    const bool gated = strstr(closed, "VTDecompressionSessionCreate failed") && gSubmits == closedSubmits;
    printf("vp9 outside the check: %s\n", closed);
    __atomic_store_n(&gCheckOpen, true, __ATOMIC_RELEASE);
    CFRelease(vp9Spec);
    // HEVC on the private check registration: clips A and V, then the Main 10 clip T, through HEVCNVDEC and the kernel
    // checker (the fake engine writes nothing, so no frame matches: all must come out, from a hardware session); then
    // with the arena full, Apple's software HEVC decoder inside the instance, which must match FFmpeg.
    char hevc[512], hevc10[512], hevcFull[512];
    const int hevcSubmits = gSubmits;
    const void *hevcKeys[] = {kVTVideoDecoderSpecification_DecoderID}, *hevcValues[] = {kCheckHEVCDecoderID};
    CFDictionaryRef hevcSpec = CFDictionaryCreate(NULL, hevcKeys, hevcValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                                  &kCFTypeDictionaryValueCallBacks);
    clip_check(ClipCheckHEVC, hevcSpec, hevc, sizeof(hevc));
    printf("hevc: %s\n", hevc);
    const int hevcMain = gSubmits - hevcSubmits;
    clip_check(ClipCheckHEVC10, hevcSpec, hevc10, sizeof(hevc10));
    printf("hevc10: %s\n", hevc10);
    const int hevcTen = gSubmits - hevcSubmits - hevcMain;
    gFull = true;
    clip_check(ClipCheckHEVC, hevcSpec, hevcFull, sizeof(hevcFull));
    gFull = false;
    CFRelease(hevcSpec);
    printf("hevc: %s (submits %d)\nhevc10: %s (submits %d)\nhevc full: %s\n", hevc, hevcMain, hevc10, hevcTen,
           hevcFull);
    const bool hevcOk = strstr(hevc, "hevc: hardware decoder") && strstr(hevc, "A 0/20") && strstr(hevc, "V 0/14") &&
                        !strstr(hevc, " out,") && hevcMain == 34 && !gRefused &&
                        strstr(hevc10, "hevc10: hardware decoder") && strstr(hevc10, "T 0/10") &&
                        !strstr(hevc10, " out,") && hevcTen == 10 && strstr(hevcFull, "software decoder") &&
                        strstr(hevcFull, "A 20/20, V 14/14");
    printf("engine: %s (opens %d, submits %d, refused %d)\nfull: %s (opens %d)\n", engine, engineOpens, engineSubmits,
           gRefused, full, gOpens - engineOpens);
    const bool ok = engineOpens >= 1 && engineSubmits == 30 && !gRefused &&
                    strstr(engine, "hardware: hardware decoder") && strstr(engine, " 30 out (") &&
                    gOpens > engineOpens && strstr(full, "hardware: software decoder, 30/30 frames match x264") &&
                    strstr(vp9, "vp9: hardware decoder") && strstr(vp9, " 16 out (") &&
                    hevcSubmits - vp9Submits == 18 && gated && presentOk && hevcOk;
    printf("%s avd: plug-in on VideoToolbox (H.264, VP9 and HEVC on a fake NVDEC, then Apple's software H.264 and "
           "HEVC in the instance)\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
