// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inside TegraNVENC: the engine (TegraNVENC.cpp), the buffer arena (TegraNVENCMemory.cpp) and the user client
// (TegraNVENCUserClient.cpp).
//
// On by default; the stages (TegraNVENC.cpp) are E0 host1x and the arena's boot chunk, E1 power and falcon, E2 syncpoint
// proof of life, E3 the TegraNVENC service and its user client. No boot-arg.
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

// Resolves a submit's buffer handle; every user client supplies its own.
typedef bool (*TegraNVENCResolve)(void *context, uint32_t handle, TegraNVENCBlock *block);

// How NVENC reads its input and addresses chroma (TegraNVENCInfo's input_layout, chroma_mode, p_frames).
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

// Checks a submit (TegraNVENCCheck.h), cleans its CPU-written ranges, copies its setup into kernel memory, writes 0
// in the address methods of features it leaves off and queues it on host1x channel 0. Returns 0 and the job's fence,
// or a negative errno-style value.
int tegra_nvenc_run(const TegraNVENCSubmit *submit, TegraNVENCResolve resolve, void *context, uint32_t *fence,
                    const char **why);
const char *tegra_nvenc_falcon_dump(void);  // the falcon's state without its debugger (a job's own line has PC/SP)
// Sleeps until the fence is reached (syncpoint interrupt), bounded per queued job. Past that bound: a one-line panic
// once published (E3), else -60 and the engine refuses work.
int tegra_nvenc_wait(uint32_t fence);
bool tegra_nvenc_ready(void);
const TegraNVENCProven *tegra_nvenc_proven(void);

int tegra_nvenc_publish(uint64_t arenaUs);  // E3
}

#endif
