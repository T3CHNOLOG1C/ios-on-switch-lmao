// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inside TegraNVDEC: the engine (TegraNVDEC.cpp), the buffer arena (TegraNVDECMemory.cpp), the user client
// (TegraNVDECUserClient.cpp) and the boot self-test (TegraNVDECSelfTest.cpp).
//
// Boot-arg switch-nvdec=<stage>: 1 brings NVDEC up (N0 host1x, N1 power and falcon, N2 syncpoint proof of life);
// 2 also reserves the arena and decodes an embedded H.264 picture (N3); 3 also publishes the TegraNVDEC service and
// its user client (N4). switch-nvdec-mib=<MiB> sizes the arena (default 48).
#ifndef TEGRA_NVDEC_H
#define TEGRA_NVDEC_H
#include <stddef.h>
#include <stdint.h>
#include "TegraNVDECABI.h"

class IOMemoryDescriptor;

// One arena allocation: physically contiguous, below 4 GiB, 64 KiB granular, zeroed and clean in the caches.
struct TegraNVDECBlock {
    uint64_t pa, size;
    uint8_t *va;
    uint32_t first, count;  // arena granules
};

// Resolves a submit's buffer handle; the self-test and every user client supply their own.
typedef bool (*TegraNVDECResolve)(void *context, uint32_t handle, TegraNVDECBlock *block);

extern "C" {
int tegra_nvdec_arena_init(uint64_t bytes);
int tegra_nvdec_arena_alloc(uint64_t size, TegraNVDECBlock *block);
void tegra_nvdec_arena_free(const TegraNVDECBlock *block);
IOMemoryDescriptor *tegra_nvdec_arena_descriptor(const TegraNVDECBlock *block);  // for mapping into a client
void tegra_nvdec_arena_usage(uint64_t *total, uint64_t *free);
void tegra_nvdec_cache_clean(const void *va, size_t size);  // dc cvac: the CPU's writes reach DRAM
void tegra_nvdec_cache_flush(const void *va, size_t size);  // dc civac: the next CPU read comes from DRAM

// Checks a submit (TegraNVDECCheck.h), cleans its CPU-written ranges, copies its picture setup into kernel memory
// and queues it on host1x channel 0. Returns 0 and the job's fence, or a negative errno-style value.
int tegra_nvdec_run(const TegraNVDECSubmit *submit, TegraNVDECResolve resolve, void *context, uint32_t *fence,
                    const char **why);
// Sleeps until the fence is reached (syncpoint interrupt). -60 when the engine did not finish in time: the engine is
// then wedged and refuses work until the next resume.
int tegra_nvdec_wait(uint32_t fence, uint32_t timeoutUs);
bool tegra_nvdec_ready(void);

int tegra_nvdec_self_test(void);  // N3
int tegra_nvdec_publish(void);    // N4
}

#endif
