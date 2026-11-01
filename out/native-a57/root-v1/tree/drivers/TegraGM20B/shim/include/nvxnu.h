/* SPDX-License-Identifier: MIT */
/*
 * Boundary between the Linux-flavoured C world (vendored nouveau nvkm + shim/linux.c) and XNU.
 * Everything here is plain C with fixed-width types only, so it can be included from both sides
 * without dragging Linux names into XNU translation units or XNU names into nvkm ones.
 * Implemented in shim/xnu_glue.cpp.
 */
#ifndef NVXNU_H
#define NVXNU_H
#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long nvxnu_u64;
typedef unsigned int nvxnu_u32;

/* logging (always goes to the kernel log; callers filter) */
void nvxnu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Bring-up trace: nvxnu_log, then 400 ms so the console draws the line before the next (possibly hanging) step. */
void nvxnu_step(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* boot-arg tegra-gpu-trace=1: bring-up trace lines wait for the console (nvxnu_step 400 ms, 2 s before M1, 3 s per stage) */
int nvxnu_trace_paced(void);
int nvxnu_vsnprintf(char *buf, unsigned long size, const char *fmt, __builtin_va_list ap);
/* nvkm's error reports (dev_err and worse; nvkm_error/nvkm_fatal): logged like nvxnu_log. Those made by nvkm's interrupt
 * handler (the GPU interrupt work loop) are also kept for that pass, and once nvxnu_errors_fatal() was called the pass
 * ends in one panic quoting them: every GPU failure nvkm reports (MMU fault, PBDMA, GR trap/exception, FECS, LTC, PRI,
 * BIND/SCHED/CHSW errors) stops the machine with its own words on screen. A non-NULL `contained` sees the pass's reports
 * first (on the work loop, nvkm's handlers done) and returns 1 to go on instead: they stay with one GPU client. */
void nvxnu_error(const char *dev, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void nvxnu_errors_fatal(int (*contained)(const char *errors));
/* what the current interrupt pass has reported so far ("" if nothing) */
const char *nvxnu_gpu_errors(void);
void nvxnu_panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* heap (sizes are tracked by the glue; free needs no size) */
void *nvxnu_malloc(unsigned long size, int zero);
void nvxnu_free(void *p);
unsigned long nvxnu_malloc_size(void *p);

/* locks: opaque handles allocated on first use */
void *nvxnu_mutex_alloc(void);
void nvxnu_mutex_free(void *m);
void nvxnu_mutex_lock(void *m);
void nvxnu_mutex_unlock(void *m);
int nvxnu_mutex_trylock(void *m);
void *nvxnu_spin_alloc(void);
void nvxnu_spin_free(void *s);
void nvxnu_spin_lock(void *s);
void nvxnu_spin_unlock(void *s);
unsigned long nvxnu_irq_save(void);
void nvxnu_irq_restore(unsigned long flags);
void *nvxnu_rw_alloc(void);
void nvxnu_rw_lock_shared(void *rw);
void nvxnu_rw_unlock_shared(void *rw);
void nvxnu_rw_lock_exclusive(void *rw);
void nvxnu_rw_unlock_exclusive(void *rw);

/* time */
nvxnu_u64 nvxnu_nanotime(void);
void nvxnu_udelay(nvxnu_u32 us); /* busy wait */
void nvxnu_usleep(nvxnu_u32 us); /* may block */
void nvxnu_yield(void);

/* deferred work: run fn(arg) on a kernel thread call */
void *nvxnu_work_alloc(void (*fn)(void *), void *arg);
void nvxnu_work_schedule(void *w);
void nvxnu_work_cancel_wait(void *w);
/* once, ns from now (a deadline, not a loop); cancel: no wait, any context */
void nvxnu_work_schedule_after(void *w, nvxnu_u64 ns);
void nvxnu_work_cancel(void *w);

/* MMIO */
volatile void *nvxnu_ioremap(nvxnu_u64 pa, nvxnu_u64 size, int write_combined);
void nvxnu_iounmap(volatile void *va, nvxnu_u64 size);

/*
 * GPU memory pool: physically contiguous, below 4 GiB, mapped write-combined in the kernel.
 * Either a range the loader withholds from XNU (boot-args tegra-gpu-pool-pa / -mb) or, as a
 * fallback, an IOBufferMemoryDescriptor. Page granular (4 KiB).
 */
int nvxnu_pool_init(void);
nvxnu_u64 nvxnu_pool_base_pa(void);
nvxnu_u64 nvxnu_pool_size(void);
void *nvxnu_pool_base_va(void);
/* map arbitrary pool pages (by pa) into one virtually contiguous kernel range, write-combined */
void *nvxnu_vmap(const nvxnu_u64 *pas, nvxnu_u32 count);
void nvxnu_vunmap(void *va);
void nvxnu_dcache_clean(const void *va, nvxnu_u64 size);
void nvxnu_dcache_clean_inval(const void *va, nvxnu_u64 size);
/* clean+invalidate [pa, pa + size) (one page at most) through the physical aperture; no-op outside the kernel's RAM */
void nvxnu_dcache_clean_inval_pa(nvxnu_u64 pa, nvxnu_u64 size);
/* the same over `count` 4 KiB pages, with one barrier pair for the whole list instead of one per page */
void nvxnu_dcache_clean_inval_pages(const nvxnu_u64 *pas, nvxnu_u32 count);
/* the system's page faults so far (every vm_fault) and the zero-filled ones among them (XNU's vm_stat counters); with the
 * kernel's user-fault timing, the user faults handled so far, their total time and the longest since the last call (ns) */
struct nvxnu_faults {
	nvxnu_u64 faults, zero_fill, user_n, user_ns, user_max_ns;
};
void nvxnu_vm_faults(struct nvxnu_faults *f);

/* firmware embedded in the kernel image (shim/TegraGM20BFirmware.S); 0 on success */
int nvxnu_firmware_find(const char *name, const void **data, unsigned long *size);

/*
 * GPU interrupts, no polled mode. nvxnu_irq_start() claims the GPU's stall and nonstall GIC SPIs (157/158 = INTID
 * 189/190, Linux tegra210.dtsi:427-428) on a work loop whose action runs nvxnu_poll_irqs() (nvkm_intr reads every
 * MC leaf, so both lines share it) and then nvxnu_wake(); it panics if it cannot. Waiters take nvxnu_wake_seq(),
 * test their condition and call nvxnu_idle(), which sleeps until the sequence moves (1) or the deadline passes (0),
 * and panics when a GPU interrupt stays pending at PMC without being taken, or when called before nvxnu_irq_start().
 * nvxnu_wake() is safe with spinlocks held. nvxnu_wake_due(): `what` (a fence, say) is done although nothing woke
 * its waiter since `seq`; after 50 ms for a wake under way it panics with the PMC and GIC state.
 */
void nvxnu_irq_start(void);
nvxnu_u32 nvxnu_wake_seq(void);
void nvxnu_wake(void);
int nvxnu_idle(nvxnu_u32 seq, nvxnu_u64 deadline_ns);
void nvxnu_wake_due(nvxnu_u32 seq, const char *what, nvxnu_u32 value);

/* boot-args */
int nvxnu_bootarg_int(const char *name, long long *value);
int nvxnu_bootarg_str(const char *name, char *buf, int len);

/* Tegra210 platform pieces (platform.c), used by the clk/regulator/reset/pmc shims */
int tegra_gm20b_platform_map(void);
int tegra_gm20b_rail_enable(int on);
int tegra_gm20b_rail_get_uv(int *uv);
int tegra_gm20b_rail_set_uv(int uv);
int tegra_gm20b_rail_max_uv(void);
nvxnu_u32 tegra_gm20b_dvfs_khz(int row); /* L4T B01 GPU rate of table row, 0 past the end */
int tegra_gm20b_dvfs_na_uv(
	nvxnu_u32 khz, int speedo); /* its NAPLL voltage, worst over temperature; -22 off-table */
int tegra_gm20b_clk_enable(int id, int on); /* ids below */
unsigned long tegra_gm20b_clk_get_rate(int id);
int tegra_gm20b_clk_set_rate(int id, unsigned long rate);
void tegra_gm20b_reset(int assert);
int tegra_gm20b_unclamp(void);
int tegra_gm20b_mc_flush(int flush);
nvxnu_u32 tegra_gm20b_fuse_read(nvxnu_u32 offset); /* Linux tegra_fuse_readl() offsets */
/* MC_INTSTATUS (cleared after the read), MC_ERR_STATUS, MC_ERR_ADR (Linux drivers/memory/tegra/mc.h): what the memory
 * controller refused since the previous call */
void tegra_gm20b_mc_errors(nvxnu_u32 *intstatus, nvxnu_u32 *status, nvxnu_u32 *addr);
enum { TEGRA_GM20B_CLK_GPU = 0, TEGRA_GM20B_CLK_REF = 1, TEGRA_GM20B_CLK_PWR = 2 };

#ifdef __cplusplus
}
#endif
#endif
