// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inside TegraNVDEC: the engine (TegraNVDEC.cpp), the buffer arena (TegraNVDECMemory.cpp), the user client
// (TegraNVDECUserClient.cpp) and the boot self-test (TegraNVDECSelfTest.cpp).
//
// On by default (stage 3). Boot-arg switch-nvdec=<stage> overrides: 0 off; 1 brings NVDEC up (N0 host1x, N1 power
// and falcon, N2 syncpoint proof of life); 2 also reserves the arena and decodes an embedded H.264 picture (N3); 3 also
// publishes the TegraNVDEC service and its user client (N4). switch-nvdec-mib=<MiB> sizes the arena (default 48).
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
void tegra_nvdec_arena_release(void);  // a failed bring-up gives the whole arena back (engine in reset first)
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
// Sleeps until the fence is reached (syncpoint interrupt), bounded by the engine's own GP timer per queued job. Past
// that bound: a one-line panic once published (N4); during the N3 self-test, -60 and the engine refuses work.
int tegra_nvdec_wait(uint32_t fence);
bool tegra_nvdec_ready(void);
// The userland clip check's verdict (TegraNVDECABI.h): claim (returns the state; *claimed for the one owner), the
// owner's report (a failure clears that codec's bit: VP9, HEVC, HEVC Main 10), a bounded wait while it runs, and the owner's release.
uint32_t tegra_nvdec_check_claim(const void *owner, bool *claimed);
int tegra_nvdec_check_report(const void *owner, bool vp9Ok, bool hevcOk, bool hevc10Ok);
uint32_t tegra_nvdec_check_wait(void);
void tegra_nvdec_check_release(const void *owner);

// The bring-up has ended: 1 when NVDEC decodes (N4 published), 0 when it does not or `timeoutMs` passed first.
// TegraNVENC's E3 decodes its picture here.
int tegra_nvdec_await(uint32_t timeoutMs);

int tegra_nvdec_self_test(void);            // N3 H.264: NVDEC itself
int tegra_nvdec_self_test_vp9(void);        // N3 VP9: decides the VP9 bit of tegra_nvdec_codecs()
int tegra_nvdec_self_test_hevc(void);       // N3 HEVC: decides the HEVC (and Main 10) bits
uint32_t tegra_nvdec_codecs(void);          // bit n: TEGRA_NVDEC_CODEC_* n proven at N3 and accepted by Submit
int tegra_nvdec_publish(uint64_t arenaUs);  // N4
}

#endif
