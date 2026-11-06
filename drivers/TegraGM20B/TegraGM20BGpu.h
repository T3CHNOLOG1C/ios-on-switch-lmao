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
/* System sleep (gpu.c "system sleep", TegraGM20BSleep.h): the GPU off and back, for whatever stage the finished bring-up
 * reached; each step is one tgm_journal() line, and a step that cannot be done panics with one line. */
void tgm_sleep(void);
void tgm_wake(void);
/* One power-journal line (TegraGM20B.cpp: drivers/SwitchSuspend switch_power_journal). Thread context. */
void tgm_journal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int tgm_stage(void);
void tgm_get_info(struct tgm_info *info);
uint64_t tgm_timestamp_ns(void);
/* Waiting for a channel's fence (interrupt-driven from stage 1 on; no polled mode): with the channel valid, take
 * waiter = tgm_chan_waiter(chan) and seq = tgm_waiter_seq(waiter), then test the fence; tgm_waiter_idle() sleeps until
 * that channel's fence moves, it is killed or destroyed (1), or deadline_ns (the absolutetime_to_nanoseconds(
 * mach_absolute_time()) clock) passes (0). The waiter stays valid after the channel is gone. A fence found complete
 * after tgm_waiter_idle() returned 0 was not announced by its interrupt: tgm_waiter_unwoken() allows 50 ms for the
 * wake, then panics. */
uint32_t tgm_chan_waiter(struct tgm_chan *chan);
uint32_t tgm_waiter_seq(uint32_t waiter);
int tgm_waiter_idle(uint32_t waiter, uint32_t seq, uint64_t deadline_ns);
void tgm_waiter_unwoken(uint32_t waiter, uint32_t seq, uint32_t seqno);

/* GPU memory pool (contiguous, write-combined in the kernel, below 4 GiB) */
void *tgm_pool_alloc(uint64_t bytes, uint64_t *pa);
void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa);

/* Address spaces: one per user-client connection, which claims it (tgm_vm_claim): only channels made for that owner
 * may address it, so BoFree's wait for the connection's channels covers every channel that can reach its pages. */
int tgm_vm_new(struct tgm_vm **pvm);
void tgm_vm_claim(struct tgm_vm *vm, const void *owner);
void tgm_vm_del(struct tgm_vm **pvm);
/* Wrap physical pages as GPU memory. `pas` (one entry per 4 KiB page) must stay valid until tgm_mem_del. */
int tgm_mem_new(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages, struct tgm_mem **pmem);
void tgm_mem_del(struct tgm_mem **pmem);
/* User range [TGM_USER_VA_START, TGM_KERNEL_VA): raw binds. read_only: the GPU may only read the range (a write is an
 * MMU fault). tgm_vm_map and tgm_vm_unmap invalidate the L2 and the TLB before they return; tgm_vm_map_ordered (a
 * claimed space's) leaves both to the next submission of each of its channels (TegraGM20BTlb.h). */
int tgm_vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem, uint64_t offset,
	       uint8_t kind, int read_only);
int tgm_vm_map_ordered(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		       uint64_t offset, uint8_t kind, int read_only);
int tgm_vm_unmap(struct tgm_vm *vm, uint64_t va, uint64_t range);
/* Kernel range: map `mem` somewhere at or above TGM_KERNEL_VA and return the address. */
int tgm_vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, uint64_t *va);

/* Channels: GPFIFO on the GR runlist, subchannels 0..4 = 3D, compute, M2MF, 2D, copy. The channel releases
 * its seqno (4 bytes) to fence_gpu_va; fence_cpu is the kernel's write-combined view of the same word. owner (the
 * creating process) names it in nvkm's reports and our panic lines. A GPU error nvkm pins on a channel that is not
 * critical kills only that channel (tgm_chan_error then non-zero, its fence released) instead of
 * panicking. */
int tgm_chan_new(struct tgm_vm *vm, const void *vm_owner, volatile uint32_t *fence_cpu,
		 uint64_t fence_gpu_va, const char *owner, int critical, struct tgm_chan **pchan);
/* GPU for every process (client fault isolation, 64 connections, client pool quota) */
int tgm_all_clients(void);
/* pool bytes not allocated */
uint64_t tgm_pool_avail(void);
/* Zero a pool BO's range (write-combined; off any client's thread) and give it back clean, so the next tgm_pool_alloc of
 * those pages does no memset. tgm_pool_free gives a range back as it is. */
void tgm_pool_free_zeroed(void *va, uint64_t bytes, uint64_t pa);
/* Give a freed pool range back zeroed by the GPU's copy engine (gpu.c "pool scrub"), without the CPU waiting: 0 if queued
 * (the range comes back clean once the copy engine's fence passes), else (no scrubber, its queue full, or not a pool
 * range) the caller zeroes it. */
int tgm_pool_scrub(uint64_t pa, uint64_t bytes);
void tgm_chan_del(struct tgm_chan **pchan);

struct tgm_push {
	uint64_t va;
	uint32_t dwords;
	uint32_t no_prefetch;
};
/* Append the pushes, then the fence (WFI semaphore release of the next seqno), then kick. User mappings are
 * L2-cacheable: the L2's clean lines are invalidated on the GPU before the pushes are fetched and its dirty lines written back before the seqno
 * is released (gpu.c l2_on). With present order the pushes run only after
 * the last GPU present queued before this call has finished reading its surface (a GPU-side wait), and after what the
 * acquires tgm_chan_order queued for the channel wait for. -EBUSY when the GPFIFO stays full for 1 s (a panic from the
 * self-test on), -EIO on a dead channel. */
struct tgm_prof;
int tgm_chan_exec(struct tgm_chan *chan, const struct tgm_push *push, uint32_t count,
		  uint32_t *seqno, const struct tgm_prof *prof);
/* The stats' push profile (gpu.c "push profile"): `prof` (or NULL) names each push of a sampled submission by what it
 * asks of the GPU (TegraGM20BUserClient.cpp scanPushes); the submission then stamps the GPU's time after each push whose
 * class is not TGM_PC_SKIP, and each window's "gpuprof" line gives every process's GPU time by class. Draws and clears
 * are split by their pass's first color target: its size (full >= 600k pixels, mid >= 100k, small), pitch-linear or
 * block-linear, multisampled or not. */
#define TGM_PROF_MAX 128

enum {
	TGM_PC_DRAW = 0,   /* + size * 4 + pitch * 2 + msaa */
	TGM_PC_CLEAR = 12, /* + the same */
	TGM_PC_PASS = 24,  /* begins a render pass, draws and clears nothing */
	TGM_PC_SYNC,       /* only waits, cache invalidates and host flushes */
	TGM_PC_COMPUTE,
	TGM_PC_COPY,
	TGM_PC_2D,
	TGM_PC_STATE, /* state and anything else */
	TGM_PC_FENCE, /* the kernel's fence push (WFI, L2 flush): the end stamp's share */
	TGM_PC_COUNT,
	TGM_PC_SKIP =
		0xff, /* no stamp after this push: a method may continue into the next one, or it was not read */
};

struct tgm_prof {
	uint8_t cls[TGM_PROF_MAX];
	/* each push's draws, and those shaded per sample (PushCounts::sampleDraws) */
	uint16_t draws[TGM_PROF_MAX], sampleDraws[TGM_PROF_MAX];
};

uint32_t tgm_chan_submitted(struct tgm_chan *chan);
uint32_t tgm_chan_completed(struct tgm_chan *chan);
/* The group a channel belongs to (its connection's task): tgm_chan_order relates the channels of one group. */
void tgm_chan_set_group(struct tgm_chan *chan, const void *group);
/* GPU-side order between the channels of chan's group (TegraGM20BABI.h method 14), no CPU wait: semaphore acquires the
 * channels' next tgm_chan_exec starts with. TGM_ORDER_AFTER: chan's next submission waits for the last submission of every
 * other channel of the group with work pending; TGM_ORDER_BEFORE: every other channel's next submission waits for chan to
 * complete `seqno` (0: its last submission; a later one is -EINVAL). *queued: the acquires queued. -ENODEV: an address
 * space has no view of the fence pages; -ENOSPC: a channel has TGM_ORDER_MAX acquires queued from other channels. */
#define TGM_ORDER_AFTER 1u
#define TGM_ORDER_BEFORE 2u
#define TGM_ORDER_MAX 16
int tgm_chan_order(struct tgm_chan *chan, uint32_t flags, uint32_t seqno, uint32_t *queued);
/* 0 healthy; else the CONTRACT.md error code (1 MMU fault, 2 GR exception, 3 host error, 4 timeout). Only before the
 * self-test passed, or under client fault isolation for a channel that is not critical: otherwise every GPU error panics
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
 * queues one copy engine copy, pitch source (stride; src_block: block-linear, width x 4 bytes across, blocks of
 * 2^TEGRA_SCANOUT_TILED_BLOCK_HEIGHT_LOG2 GOBs) to a block-linear destination (dst_pitch bytes across, blocks one
 * GOB wide and 2^block_height_log2 GOBs high, 8-line GOBs), after the work submitted so far by the channels whose address
 * spaces import the source (every user channel's when none does), and
 * returns at once: 0, or -EBUSY (one in flight), -EINVAL, -ENODEV. done(arg) runs exactly once for an accepted copy,
 * from the GPU interrupt pass, when its bytes are in DRAM; one not done in 1 s panics. */
int tgm_present_open(struct tgm_vm **pvm);
int tgm_present_map(uint64_t va, struct tgm_mem *mem, uint64_t bytes, uint8_t kind);
int tgm_present_unmap(uint64_t va, uint64_t bytes);
int tgm_present_copy(uint64_t src_va, uint64_t src_pa, uint64_t stride, int src_block,
		     uint32_t width, uint32_t height, uint64_t dst_va, uint32_t dst_pitch,
		     uint32_t block_height_log2, void (*done)(void *), void *arg);
/* Host import bookkeeping for the present: `pa` is the import's first page; add 1 when made, 0 when released. The
 * present waits only for address spaces that import its surface (src_pa, the surface's first page), when any does. */
void tgm_vm_import_note(struct tgm_vm *vm, uint64_t pa, int add);

/* Clean+invalidate the CPU caches over pages of ordinary RAM (by physical address), so the GPU, which does not snoop
 * them, reads what the CPU wrote and no dirty line is later written back over what the GPU wrote. */
void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages);

/* Host-import coherency registry (TegraGM20BCoherency.cpp), kept coherent at every process's IOSurfaceLock/Unlock of
 * an imported surface (tegra_gm20b_iosurface_trap). add at import, with the first page, the IOSurface ID (0: not an
 * IOSurface) and the import's page list; it returns the entry the import holds (NULL without memory: no lazy
 * coherency). The importer cleans its pages itself, then says so (cleaned), unless *covered: a live import of the
 * same surface that finished its clean, kept clean since by the traps, already spans them. gpu_writes: the import does
 * not say the GPU only reads it (TegraGM20BImportGpuReads). put at release; wrote from Exec, for a writable import a
 * submission may have written: it marks only a surface the GPU may write. take_stats returns and zeroes the traps' lock
 * invalidations and unlock cleans (count, bytes each). */
struct tgm_coh;
int tgm_coherency_init(void);
struct tgm_coh *tgm_coherency_add(uint64_t pa, uint32_t surface_id, const uint64_t *pas,
				  uint32_t npages, int writable, int gpu_writes, int *covered);
/* SurfaceWrites (method 15): every entry of the surface counts submissions as GPU writes from now on. */
void tgm_coherency_gpu_writes(uint32_t surface_id);
void tgm_coherency_cleaned(struct tgm_coh *c);
void tgm_coherency_put(struct tgm_coh *c);
void tgm_coherency_wrote(struct tgm_coh *c);
void tgm_coherency_take_stats(uint64_t out[4]);
/* XNU's iokit_user_client_trap, after an IOSurfaceRootUserClient trap ran: index 2 lock, 3 unlock (IOConnectTrap3 p1 the
 * surface ID, p2 the lock options). */
void tegra_gm20b_iosurface_trap(uint32_t index, uint32_t surface_id, uint32_t options);
/* A GPU client made (add 1) or removed (add -1) a bind of the whole surface holding `c` with TEGRA_SCANOUT_TILED_KIND:
 * the surface is tiled while any such bind lives, whatever other binds of it say (tegra_gm20b_surface_tiled). */
void tgm_coherency_tiled_bind(struct tgm_coh *c, int add);

/* The stats (every periodic tegra-gm20b line, the submissions' GPU stamps, the push scan and profile): debug kernels
 * only; a release kernel has none of their code or strings. */
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define TGM_STATS 1
#else
#define TGM_STATS 0
#endif
/* The stats: one log line per 5 s of submissions (submits, fence latency, imports, pool, MC errors).
 * tgm_stats_import(+bytes, first page) for each host import made, (-bytes, first page) for each one released. */
void tgm_stats_import(int64_t bytes, uint64_t pa);

/* With the stats, what making and dropping GPU memory costs the CPU ("tegra-gm20b: mem", per 5 s window):
 * a BoAlloc of `bytes` (pool: write-combined pool memory; else cached pages, phase_ns = kernel allocation and wiring,
 * cache clean, page list, GPU memory object), a host import (phase_ns = wiring, page list, GPU memory object, cache
 * clean; skipped: always 0, the import always cleans), a VmBind map or unmap of `pages`, a BoFree, a cached BO's view
 * mapped with its pages entered at once (prefault). */
enum {
	TGM_PHASES = 4
};

/* kind: TGM_BO_CACHED (made on the caller's thread: allocated, zero-filled, cleaned), TGM_BO_POOL, TGM_BO_SPARE (a cached
 * BO taken from TegraGM20BUserClient.cpp's spares: zeroed and cleaned off every client's thread); owner: the process.
 * tgm_stats_compositor_pool: the compositor's pool BOs, +/-. tgm_spare_counts and tgm_owner_stats (each process's BoAllocs
 * this window, then cleared) are TegraGM20BUserClient.cpp's, for the "mem" and "mem3" lines. */
enum {
	TGM_BO_CACHED = 0,
	TGM_BO_POOL = 1,
	TGM_BO_SPARE = 2
};

void tgm_stats_bo_alloc(uint64_t bytes, int kind, uint64_t ns, const uint64_t phase_ns[TGM_PHASES],
			const char *owner);
void tgm_stats_compositor_pool(int64_t bytes);
void tgm_spare_counts(uint64_t *kept_bytes, uint64_t *ready, uint64_t *pool_zeroing_bytes);
/* Where the pool is (TegraGM20BUserClient.cpp, for the stats2 line): pool BOs of clients other than the compositor, pool
 * pages freed but still behind their view's release (orphans: a task still maps the view, or an import of it is live), and
 * coherent BOs served from system memory instead (tgm_pool::FromSystem) with their count. */
void tgm_pool_breakdown(uint64_t *client_bytes, uint64_t *orphan_bytes, uint64_t *orphans,
			uint64_t *system_bytes, uint64_t *systems);
unsigned long tgm_owner_stats(char *out, unsigned long size);
void tgm_stats_bo_import(uint64_t bytes, int read_only, int skipped, uint64_t ns,
			 const uint64_t phase_ns[TGM_PHASES]);
void tgm_stats_bind(int map, uint64_t pages, uint64_t ns);
/* A fence wait that slept on `waiter`, then woke (woken 1) or reached its deadline (0), and found its fence done or
 * not: the "waits" line's wakes, waits woken for another channel's fence, and wake-to-waiter latency. */
void tgm_stats_wait(uint32_t waiter, int woken, int done);
void tgm_stats_bo_free(uint64_t ns);
void tgm_stats_prefault(uint64_t bytes);

/* drivers/SwitchPower (gpu.c "idle clock"): switch_perf_hint's touch/missed-frame/launch hints (any context; back to
 * the run rate at once if idle), and the CPU governor's tick (scheduler context: loads, at most one work item). */
void tegra_gm20b_perf_hint(unsigned int hint);
void tegra_gm20b_idle_tick(void);

#ifdef __cplusplus
}
#endif
#endif
