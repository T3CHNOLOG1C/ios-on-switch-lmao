// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test for AVD.videodecoder's VideoToolbox side, on a Mac: the plug-in itself (AVDDecoder.c, included for its
// factory) over a fake TegraNVDEC client, registered in this process and driven by the clip check (ClipCheck.c)
// through VideoToolbox, which checks the plug-in's class layout and session calls against a real VideoToolbox.
//   engine: the fake client hands out buffers and checks every submit with the kernel's checker; its engine writes
//           nothing, so pictures come out black: the test wants all 30 out of a session that says hardware.
//   full:   the fake arena refuses every buffer, so the instance runs Apple's software decoder: the test wants 30/30
//           frames matching x264 from a session that says software.
//   vp9:    VP9Clip.h as 'vp09' (kVTVideoDecoderSpecification_AllowVP9Decode) on the fake engine: every superframe and
//           frame through VP9NVDEC and the kernel's checker, all 16 shown frames out of a session that says hardware.
// Prints "PASS avd ..." when both hold (tests/test_switch_video.py).
#include "../AVD/AVDDecoder.c"
#include <stdio.h>
#include "../../TegraNVDEC/TegraNVDECCheck.h"

extern const CFStringRef kVTVideoDecoderSpecification_DecoderID;
extern OSStatus VTRegisterVideoDecoderWithInfo(FourCharCode codecType, CFDictionaryRef info,
                                               VTDecoderCreateInstance create);

struct NVDECClient {
    uint8_t *va[64];
    uint64_t size[64];
};

static bool gFull;
static int gOpens, gSubmits, gRefused;

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
    if (!c) return;
    for (int i = 0; i < 64; i++) free(c->va[i]);
    free(c);
}

uint32_t nvdec_codecs(const NVDECClient *c)
{
    (void)c;
    return 1u << TEGRA_NVDEC_CODEC_H264 | 1u << TEGRA_NVDEC_CODEC_VP9;
}

bool nvdec_claim_self_test(NVDECClient *c)
{
    (void)c;
    return false;
}

int nvdec_alloc(void *ctx, uint64_t size, uint32_t *handle, void **va)
{
    NVDECClient *c = ctx;
    if (gFull) return -1;
    for (uint32_t h = 1; h < 64; h++) {
        if (c->va[h]) continue;
        if (!(c->va[h] = calloc(1, size))) return -1;
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
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC) continue;
        if (e->value >= 64 || !c->va[e->value] || e->offset > c->size[e->value]) return -1;
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
    (void)ctx, (void)w;
    return 0;
}

int main(void)
{
    gLog = os_log_create("com.ioswitch.avd", "host-test");
    // A class ID of our own makes the registry entry "com.ioswitch.avd-host.avc1", which the sessions name below: the
    // Mac's own H.264 decoders would otherwise win the selection.
    const void *keys[] = {CFSTR("CMClassImplementationID")}, *values[] = {CFSTR("com.ioswitch.avd-host")};
    CFDictionaryRef info =
        CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const OSStatus registered = VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_H264, info, createInstance) ||
                                VTRegisterVideoDecoderWithInfo(kCMVideoCodecType_VP9, info, createInstance);
    CFRelease(info);
    if (registered) {
        printf("FAIL avd: VTRegisterVideoDecoderWithInfo %d\n", (int)registered);
        return 1;
    }
    const void *specKeys[] = {kVTVideoDecoderSpecification_DecoderID},
               *specValues[] = {CFSTR("com.ioswitch.avd-host.avc1")};
    CFDictionaryRef spec = CFDictionaryCreate(NULL, specKeys, specValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    char engine[512], full[512];
    clip_check(ClipCheckHardware, spec, engine, sizeof(engine));
    const int engineOpens = gOpens, engineSubmits = gSubmits;
    gFull = true;
    clip_check(ClipCheckHardware, spec, full, sizeof(full));
    CFRelease(spec);
    gFull = false;
    char vp9[512];
    const int vp9Submits = gSubmits;
    const void *vp9Keys[] = {kVTVideoDecoderSpecification_DecoderID},
               *vp9Values[] = {CFSTR("com.ioswitch.avd-host.vp09")};
    CFDictionaryRef vp9Spec = CFDictionaryCreate(NULL, vp9Keys, vp9Values, 1, &kCFTypeDictionaryKeyCallBacks,
                                                 &kCFTypeDictionaryValueCallBacks);
    clip_check(ClipCheckVP9, vp9Spec, vp9, sizeof(vp9));
    CFRelease(vp9Spec);
    printf("vp9: %s (submits %d)\n", vp9, gSubmits - vp9Submits);
    printf("engine: %s (opens %d, submits %d, refused %d)\nfull: %s (opens %d)\n", engine, engineOpens, engineSubmits,
           gRefused, full, gOpens - engineOpens);
    const bool ok = engineOpens >= 1 && engineSubmits == 30 && !gRefused &&
                    strstr(engine, "hardware: hardware decoder") && strstr(engine, " 30 out (") &&
                    gOpens > engineOpens && strstr(full, "hardware: software decoder, 30/30 frames match x264") &&
                    strstr(vp9, "vp9: hardware decoder") && strstr(vp9, " 16 out (") && gSubmits - vp9Submits == 18;
    printf("%s avd: plug-in on VideoToolbox (H.264 and VP9 on a fake NVDEC, then Apple's software H.264 in the "
           "instance)\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
