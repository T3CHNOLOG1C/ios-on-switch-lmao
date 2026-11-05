// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inside TegraNVENC: the engine (TegraNVENC.cpp), the buffer arena (TegraNVENCMemory.cpp), the user client
// (TegraNVENCUserClient.cpp) and the boot self-test (TegraNVENCSelfTest.cpp).
//
// On by default, like TegraNVDEC; the stages (TegraNVENC.cpp) are E0 host1x and the arena's boot chunk, E1 power and
// falcon, E2 syncpoint proof of life, E3 an H.264 picture encoded and decoded back through NVDEC, E4 the TegraNVENC
// service and its user client. switch-nvdec=0 turns NVENC off as well: E3 needs NVDEC, and so does a check of what
// NVENC writes. No boot-arg of its own.
#ifndef TEGRA_NVENC_H
#define TEGRA_NVENC_H
#include <stddef.h>
#include <stdint.h>
#include "TegraNVENCABI.h"

class IOMemoryDescriptor;

// One arena allocation: physically contiguous, below 4 GiB, 64 KiB granular, zeroed and clean in the caches.
struct TegraNVENCBlock {
    uint64_t pa, size;
    uint8_t *va;
    uint32_t chunk, first, count;  // arena chunk and its granules
};

// Resolves a submit's buffer handle; the self-test and every user client supply their own.
typedef bool (*TegraNVENCResolve)(void *context, uint32_t handle, TegraNVENCBlock *block);

// What E3 proved (TegraNVENCInfo's input_layout, chroma_mode, p_frames).
struct TegraNVENCProven {
    uint32_t inputLayout, chromaMode, pFrames;
};

extern "C" {
int tegra_nvenc_arena_init(uint64_t bytes);  // the boot chunk
void tegra_nvenc_arena_release(void);        // a failed bring-up gives every chunk back (engine in reset first)
int tegra_nvenc_arena_alloc(uint64_t size, TegraNVENCBlock *block);  // adds a chunk when none has room
void tegra_nvenc_arena_free(const TegraNVENCBlock *block);
IOMemoryDescriptor *tegra_nvenc_arena_descriptor(const TegraNVENCBlock *block);  // for mapping into a client
void tegra_nvenc_arena_usage(uint64_t *total, uint64_t *free);
void tegra_nvenc_cache_clean(const void *va, size_t size);  // dc cvac: the CPU's writes reach DRAM
void tegra_nvenc_cache_flush(const void *va, size_t size);  // dc civac: the next CPU read comes from DRAM

// Checks a submit (TegraNVENCCheck.h), cleans its CPU-written ranges, copies its setup into kernel memory, points
// the address methods it leaves out at the scratch buffer and queues it on host1x channel 0. Returns 0 and the
// job's fence, or a negative errno-style value.
int tegra_nvenc_run(const TegraNVENCSubmit *submit, TegraNVENCResolve resolve, void *context, uint32_t *fence,
                    const char **why);
// E3 only (refused once published): the same job with the setup's magic zeroed in the kernel's copy, which the
// firmware must refuse (nvenc_drv.h: "terminate with error code ...BadMagic").
int tegra_nvenc_run_magic_probe(const TegraNVENCSubmit *submit, TegraNVENCResolve resolve, void *context,
                                uint32_t *fence, const char **why);
const char *tegra_nvenc_falcon_dump(void);  // the falcon's state without its debugger (a job's own line has PC/SP)
// Sleeps until the fence is reached (syncpoint interrupt), bounded per queued job. Past that bound: a one-line panic
// once published (E4); during the E3 self-test, -60 and the engine refuses work.
int tegra_nvenc_wait(uint32_t fence);
bool tegra_nvenc_ready(void);
bool tegra_nvenc_claim_self_test(void);  // true for one caller per boot (the plug-in's clip check)
const TegraNVENCProven *tegra_nvenc_proven(void);

int tegra_nvenc_self_test(TegraNVENCProven *proven);  // E3
int tegra_nvenc_publish(uint64_t arenaUs);            // E4

// TegraNVDEC's bring-up has ended (published, refused, or off): 1 when NVDEC decodes, 0 otherwise. Sleeps on
// NVDEC's settle event, at most `timeoutMs`.
int tegra_nvdec_await(uint32_t timeoutMs);
}

#endif
