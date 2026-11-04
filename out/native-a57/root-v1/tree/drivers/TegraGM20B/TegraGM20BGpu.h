/* SPDX-License-Identifier: MIT */
/*
 * C interface between the IOKit layer (TegraGM20B.cpp, TegraGM20BUserClient.cpp) and the nvkm/nvif layer
 * (gpu.c). Plain C types only; every object is opaque. All calls may block; call from thread context.
 */
#ifndef TEGRA_GM20B_GPU_H
#define TEGRA_GM20B_GPU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Kernel-owned part of every GPU address space (CONTRACT.md §4.3): [TGM_KERNEL_VA, TGM_VA_LIMIT). */
#define TGM_USER_VA_START 0x1000ULL
#define TGM_KERNEL_VA (1ULL << 39)
#define TGM_VA_LIMIT (1ULL << 40)

struct tgm_vm;
struct tgm_mem;
struct tgm_chan;

struct tgm_info {
	uint32_t boot0;
	uint16_t chipset;
	uint8_t gpc_count;
	uint16_t tpc_count;
};

/* Bring the GPU up to `stage` (1 = M1 power/ring/GPCPLL, 2 = + ACR/PMU/FECS/GPCCS, 3 = + channel/CE/fence
 * self-test). Writes one human-readable result line into `line`. Returns 0 or a negative errno. */
int tgm_bringup(int stage, char *line, unsigned int len);
int tgm_stage(void);
void tgm_get_info(struct tgm_info *info);
uint64_t tgm_timestamp_ns(void);
/* Waiting (interrupt-driven from stage 1 on; no polled mode): take tgm_wake_seq(), test the condition, then
 * tgm_idle() sleeps until a GPU interrupt or other wake moves the sequence (1) or deadline_ns (the
 * absolutetime_to_nanoseconds(mach_absolute_time()) clock) passes (0). A fence found complete after tgm_idle()
 * returned 0 was not announced by its interrupt: tgm_fence_unwoken() allows 50 ms for the wake, then panics. */
uint32_t tgm_wake_seq(void);
int tgm_idle(uint32_t seq, uint64_t deadline_ns);
void tgm_fence_unwoken(uint32_t seq, uint32_t seqno);

/* GPU memory pool (contiguous, write-combined in the kernel, below 4 GiB) */
void *tgm_pool_alloc(uint64_t bytes, uint64_t *pa);
void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa);

/* Address spaces: one per user-client connection. */
int tgm_vm_new(struct tgm_vm **pvm);
void tgm_vm_del(struct tgm_vm **pvm);
/* Wrap physical pages as GPU memory. `pas` (one entry per 4 KiB page) must stay valid until tgm_mem_del. */
int tgm_mem_new(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages, struct tgm_mem **pmem);
void tgm_mem_del(struct tgm_mem **pmem);
/* User range [TGM_USER_VA_START, TGM_KERNEL_VA): raw binds, TLB invalidated before return. read_only: the GPU may only
 * read the range (a write is an MMU fault). */
int tgm_vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem, uint64_t offset,
	       uint8_t kind, int read_only);
int tgm_vm_unmap(struct tgm_vm *vm, uint64_t va, uint64_t range);
/* Kernel range: map `mem` somewhere at or above TGM_KERNEL_VA and return the address. */
int tgm_vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, uint64_t *va);

/* Channels: GPFIFO on the GR runlist, subchannels 0..4 = 3D, compute, M2MF, 2D, copy. The channel releases
 * its seqno (4 bytes) to fence_gpu_va; fence_cpu is the kernel's write-combined view of the same word. owner (the
 * creating process) names it in nvkm's reports and our panic lines. With tegra-gpu-all-clients=1 a GPU error nvkm pins
 * on a channel that is not critical kills only that channel (tgm_chan_error then non-zero, its fence released) instead of
 * panicking. */
int tgm_chan_new(struct tgm_vm *vm, volatile uint32_t *fence_cpu, uint64_t fence_gpu_va,
		 const char *owner, int critical, struct tgm_chan **pchan);
/* boot-arg tegra-gpu-all-clients=1: GPU for every process (client fault isolation, 64 connections, client pool quota) */
int tgm_all_clients(void);
/* pool bytes not allocated */
uint64_t tgm_pool_avail(void);
void tgm_chan_del(struct tgm_chan **pchan);
struct tgm_push {
	uint64_t va;
	uint32_t dwords;
	uint32_t no_prefetch;
};
/* Append the pushes, then the fence (WFI semaphore release of the next seqno), then kick. With tegra-gpu-l2=1 user
 * mappings are L2-cacheable: the L2's clean lines are invalidated before the kick and its dirty lines written back
 * before the seqno is released (gpu.c l2_on). With tegra-gpu-present-order (on unless =0) the pushes run only after
 * the last GPU present queued before this call has finished reading its surface (a GPU-side wait). -EBUSY when the
 * GPFIFO stays full for 1 s (a panic from the self-test on), -EIO on a dead channel. */
int tgm_chan_exec(struct tgm_chan *chan, const struct tgm_push *push, uint32_t count,
		  uint32_t *seqno);
uint32_t tgm_chan_submitted(struct tgm_chan *chan);
uint32_t tgm_chan_completed(struct tgm_chan *chan);
/* 0 healthy; else the CONTRACT.md error code (1 MMU fault, 2 GR exception, 3 host error, 4 timeout). Only before the
 * self-test passed, or with tegra-gpu-all-clients=1 for a channel that is not critical: otherwise every GPU error panics
 * (nvkm's report in the panic string) and no channel dies quietly. */
uint32_t tgm_chan_error(struct tgm_chan *chan);
/* Panic naming `who` and the channel's state (fence, GPFIFO GET/PUT, PGRAPH/engine/PBDMA status, MC error registers):
 * work submitted on it did not complete and nvkm reported no error. */
void tgm_chan_hung(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
	__attribute__((noreturn));
/* After a wait for `seqno` timed out: panics (tgm_chan_hung) once the channel has completed nothing for 2 s while work
 * is pending; a single submission that runs that long is taken for a hang. */
void tgm_chan_check_progress(struct tgm_chan *chan, const char *who, uint32_t seqno);

/* For a CPU reader of GPU-written memory outside the GPU client (NativeLegacyDisplay's copy of a swapped IOSurface,
 * which calls it weakly): wait for every GPU submission made so far (all channels), then clean+invalidate
 * [va, va + bytes) of the reader's mapping. Returns at once, touching no cache, when no user channel exists.
 * 0; a channel that has not completed its work after timeout_ns panics (tgm_chan_hung, from the self-test on;
 * -ETIMEDOUT before). Thread context. */
int tegra_gm20b_sync_for_cpu(const void *va, uint64_t bytes, uint64_t timeout_ns);

/* Fence pages: one per connection, from one pool range the GPU present maps whole (gpu.c). Zeroed; 0 or -ENOMEM. */
int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa);
void tgm_fence_page_put(volatile uint32_t *va);

/* GPU present (gpu.c "GPU present"): a kernel channel and address space, made at the first call (from the self-test on).
 * Mappings in it are volatile, at addresses the caller picks in [TGM_USER_VA_START, TGM_KERNEL_VA). tgm_present_copy
 * queues one copy engine copy, pitch source (stride) to a block-linear destination (dst_pitch bytes across, blocks one
 * GOB wide and 2^block_height_log2 GOBs high, 8-line GOBs), after the work submitted so far by the channels whose address
 * spaces import the source (every user channel's when none does, or with tegra-gpu-present-writer=0), and
 * returns at once: 0, or -EBUSY (one in flight), -EINVAL, -ENODEV. done(arg) runs exactly once for an accepted copy,
 * from the GPU interrupt pass, when its bytes are in DRAM; one not done in 1 s panics. */
int tgm_present_open(struct tgm_vm **pvm);
int tgm_present_map(uint64_t va, struct tgm_mem *mem, uint64_t bytes, uint8_t kind);
int tgm_present_unmap(uint64_t va, uint64_t bytes);
int tgm_present_copy(uint64_t src_va, uint64_t src_pa, uint64_t stride, uint32_t width,
		     uint32_t height, uint64_t dst_va, uint32_t dst_pitch,
		     uint32_t block_height_log2, void (*done)(void *), void *arg);
/* Host import bookkeeping for the present: `pa` is the import's first page; add 1 when made, 0 when released. The
 * present waits only for address spaces that import its surface (src_pa, the surface's first page), when any does. */
void tgm_vm_import_note(struct tgm_vm *vm, uint64_t pa, int add);

/* Clean+invalidate the CPU caches over pages of ordinary RAM (by physical address), so the GPU, which does not snoop
 * them, reads what the CPU wrote and no dirty line is later written back over what the GPU wrote. */
void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages);

/* Host-import coherency registry (gpu.c), keyed by a surface's first physical page: the lazy CPU<->GPU path the XNU
 * IOSurface lock/unlock hook drives (once that hook is on the trap path: see gpu.c). add at import (with the import's page list; returns the entry the import holds; the
 * import still cleans its pages itself, always) and put at release; wrote marks a writable import a submission may have
 * written (from exec: one store through the import's entry); read_lock invalidates a GPU-written import for a CPU read lock and clears the mark;
 * write_unlock cleans a GPU import after a CPU write unlock. The last two return the bytes touched (0 if nothing / not
 * our import), and do the cache maintenance themselves (by VA, inner-shareable broadcast). */
struct tgm_coh;
struct tgm_coh *tgm_coherency_add(uint64_t pa, const uint64_t *pas, uint32_t npages, int writable);
void tgm_coherency_put(struct tgm_coh *c);
void tgm_coherency_wrote(struct tgm_coh *c);
uint64_t tgm_coherency_read_lock(uint64_t pa);
uint64_t tgm_coherency_write_unlock(uint64_t pa);

/* boot-arg tegra-gpu-stats=1: one log line per 5 s of submissions (submits, fence latency, imports, pool, MC errors).
 * tgm_stats_import(+bytes, first page) for each host import made, (-bytes, first page) for each one released. */
int tgm_stats_on(void);
void tgm_stats_import(int64_t bytes, uint64_t pa);
/* With tegra-gpu-stats, what making and dropping GPU memory costs the CPU ("tegra-gm20b: mem", per 5 s window):
 * a BoAlloc of `bytes` (pool: write-combined pool memory; else cached pages, phase_ns = kernel allocation and wiring,
 * cache clean, page list, GPU memory object), a host import (phase_ns = wiring, page list, GPU memory object, cache
 * clean; skipped: always 0 now, the import always cleans), a VmBind map or unmap of `pages`, a BoFree, a cached BO's view
 * mapped with its pages entered at once (prefault). */
enum { TGM_PHASES = 4 };
void tgm_stats_bo_alloc(uint64_t bytes, int pool, uint64_t ns, const uint64_t phase_ns[TGM_PHASES]);
void tgm_stats_bo_import(uint64_t bytes, int read_only, int skipped, uint64_t ns,
			 const uint64_t phase_ns[TGM_PHASES]);
void tgm_stats_bind(int map, uint64_t pages, uint64_t ns);
void tgm_stats_bo_free(uint64_t ns);
void tgm_stats_prefault(uint64_t bytes);

#ifdef __cplusplus
}
#endif
#endif
