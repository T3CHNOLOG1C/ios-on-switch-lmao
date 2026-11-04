/*
 * IOSwitch kernel sampling profiler: where the CPU goes, kernel_task included, in the unified log every 10 s.
 *
 * Sampling. Each CPU takes one sample about every 4 ms from its own running timer (RUNNING_TIMER_IOSWITCH_KPROF).
 * Running timers are armed only while a non-idle thread runs, so an idle CPU is never woken for a sample and the cost
 * scales with busy time (one extra timer interrupt per 4 ms of work). A sample is the interrupted context
 * (cpu_int_state): a user-mode sample counts against its process; a kernel-mode sample also goes into a per-CPU table
 * keyed by function, from the kernel's LC_FUNCTION_STARTS (copied at boot, before __LINKEDIT is jettisoned), or by
 * 128-byte bucket outside the kernel image (Apple's prelinked kexts). Each function also keeps a majority-vote caller
 * and owner (the process, or the kernel_task thread). A user-mode sample goes into a per-CPU table keyed by process and
 * 64-byte bucket: the unslid dyld shared cache address, or the offset from the main executable's Mach-O header, or one
 * "other" bucket (app frameworks, JIT). A timer sample cannot see interrupt handlers or code that runs with interrupts
 * masked: those land where interrupts come back on, so the exact per-CPU interrupt time is logged too.
 *
 * Reporting. A thread call swaps every CPU's table with one cross-call, merges them and writes os_log lines (the log
 * only, never the console):
 *   IOSwitch kprof: the window, per-CPU busy and interrupt time (exact, from recount), the sample split
 *   IOSwitch kprof tasks: processes by samples, user+kernel
 *   IOSwitch kprof kernel 1/2, 2/2: kernel functions by samples: unslid address (kext@address in a kext), samples in
 *       kernel_task threads, majority caller, majority owner
 *   IOSwitch kprof kernel_task: kernel_task threads by exact CPU time, with name and their top sampled function
 *   IOSwitch kprof user 1/2, 2/2: the busiest processes' user-mode hot spots: dsc <unslid address>, main+<offset>, other
 *   IOSwitch kprof masked: where the samples that land on ml_set_interrupts_enabled come from. A timer sample taken
 *       while interrupts are masked is delivered at the unmask, so these samples are masked time; three return
 *       addresses name the code that masked (the scheduler's context switch, a driver's spinlock, ...)
 *   The first line also carries each CPU's interrupts, IPIs and synchronous exceptions (faults and traps) per second.
 * User trap sites: a process's first brk (os_crash, __builtin_trap, Swift traps) prints one console line with the trap
 * site as main+<offset> or dsc <unslid address>, so a crash that takes the boot down (launchd _PanicOnCrash) still names
 * its os_crash call site.
 * Kernel addresses are unslid: `xcrun atos -o kernel.macho -arch arm64 <addr>...` with the build's kernel.macho names
 * them. dsc addresses: `ipsw dyld a2s <21H461 dyld_shared_cache_arm64> <addr>`.
 */

#include <kern/cpu_data.h>
#include <kern/kalloc.h>
#include <kern/processor.h>
#include <kern/recount.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/timer_call.h>
#include <libkern/OSKextLibPrivate.h>
#include <libkern/kernel_mach_header.h>
#include <mach-o/loader.h>
#include <machine/machine_routines.h>
#include <os/log.h>
#include <vm/vm_shared_region.h>
#include <arm/cpu_data_internal.h>
#include <arm64/proc_reg.h>

extern void qsort(void *a, size_t n, size_t es, int (*cmp)(const void *, const void *));
extern int  vsnprintf(char *, size_t, const char *, va_list);

#define KPROF_PERIOD_NS   4000000ull        /* one sample per 4 ms of CPU work */
#define KPROF_WINDOW_NS   10000000000ull    /* one summary every 10 s */
#define KPROF_MAX_CPUS    8
#define KPROF_CPU_FNS     1024              /* per CPU and window: at most 2500 samples, most of them repeats */
#define KPROF_CPU_TASKS   128
#define KPROF_FNS         4096              /* merged */
#define KPROF_TASKS       256
#define KPROF_THREADS     1024              /* kernel_task threads tracked for exact time */
#define KPROF_PROBES      16
#define KPROF_TOP_FNS     20
#define KPROF_TOP_TASKS   14
#define KPROF_TOP_THREADS 8
#define KPROF_CPU_USER    2048              /* per CPU and window: (process, 64-byte bucket) pairs */
#define KPROF_USER        8192              /* merged */
#define KPROF_TOP_USER    4                 /* processes whose user hot spots are logged */
#define KPROF_TOP_USER_PC 8
#define KPROF_CPU_MASKED  256               /* per CPU and window: unmask-point call chains */
#define KPROF_MASKED      1024              /* merged */
#define KPROF_TOP_MASKED  10
#define KPROF_TRAP_NAMES  32                /* processes whose first trap was printed */
#define KPROF_LINE        900               /* under the 1 KiB os_log record */
#define KPROF_TID         (1ull << 63)      /* owner is a kernel_task thread id, not a pid */
#define KPROF_BUCKET      1ul               /* key is a 128-byte bucket, not a function start */
#define KPROF_U_DSC       (1ull << 62)      /* user key: unslid dyld shared cache address */
#define KPROF_U_MAIN      (2ull << 62)      /* user key: offset from the main executable's Mach-O header */
#define KPROF_U_OTHER     (3ull << 62)      /* user key: anywhere else (app frameworks, JIT), one bucket */
#define KPROF_U_ADDR      ((1ull << 62) - 1)

struct kprof_fn {
	uintptr_t key;          /* function start (slid) or 128-byte bucket | KPROF_BUCKET; 0: empty */
	uint64_t  caller;       /* majority-vote caller key */
	uint64_t  owner;        /* majority-vote owner: pid, or KPROF_TID | thread id */
	uint32_t  samples;
	uint32_t  kernel_task;  /* of which in kernel_task threads */
	uint32_t  caller_votes;
	uint32_t  owner_votes;
};

struct kprof_task {
	int32_t  pid;
	uint16_t user;
	uint16_t kernel;
	char     name[28];
	bool     used;
};

struct kprof_user {
	uint64_t key;           /* KPROF_U_* | bucket; 0: empty */
	int32_t  pid;
	uint32_t samples;
};

struct kprof_masked {
	uintptr_t chain[3];     /* the masking code's return addresses as function keys; chain[0] 0: empty */
	uint32_t  samples;
	uint32_t  kernel_task;
};

struct kprof_buf {
	uint32_t            samples, user, kernel, kernel_task, lost;
	struct kprof_fn     fn[KPROF_CPU_FNS];
	struct kprof_task   task[KPROF_CPU_TASKS];
	struct kprof_user   user_pc[KPROF_CPU_USER];
	struct kprof_masked masked[KPROF_CPU_MASKED];
};

struct kprof_cpu {
	struct kprof_buf *buf[2];
	unsigned int      active;   /* the buffer the sampler fills; the summary's cross-call flips it */
	bool              armed;
	uint64_t          deadline;
	uint32_t          rng;
	uint64_t          last_idle, last_irq;
	uint64_t          last_irqs, last_ipis, last_exc;  /* cpu_stat counts at the last summary */
	bool              primed;     /* last_idle/last_irq hold a previous summary's values */
};

struct kprof_thread {
	uint64_t tid;
	uint64_t time;
};

static struct kprof_cpu    kprof_cpus[KPROF_MAX_CPUS];
static unsigned int        kprof_ncpus;
static bool                kprof_ready;
static uint64_t            kprof_period, kprof_window;
static uintptr_t           kprof_image, kprof_text_start, kprof_text_end;
static uint32_t           *kprof_starts;        /* function starts, offsets from kprof_image, ascending */
static uint32_t            kprof_nstarts;
static struct kprof_fn    *kprof_fns;           /* merged window */
static struct kprof_task  *kprof_tasks;
static struct kprof_user  *kprof_users;
static struct kprof_masked *kprof_masks;        /* merged window */
static uintptr_t           kprof_unmask[2];     /* ml_set_interrupts_enabled(_with_debug) as function keys */
static struct kprof_thread *kprof_threads[2];   /* kernel_task threads: this window, last window (sorted by tid) */
static uint64_t           *kprof_vals;          /* top-N scratch, KPROF_USER entries */
static uint32_t            kprof_nthreads[2];
static unsigned int        kprof_thread_cur;
static uint64_t            kprof_last_summary;
static thread_call_t       kprof_call;
static char                kprof_line[KPROF_LINE];
static size_t              kprof_len;

/* --- sampling (timer interrupt context, interrupts off) ------------------------------------------------------------ */

static uintptr_t
kprof_fn_key(uintptr_t pc)
{
	if (pc >= kprof_text_start && pc < kprof_text_end && kprof_nstarts != 0) {
		uint32_t off = (uint32_t)(pc - kprof_image);
		uint32_t lo = 0, hi = kprof_nstarts;

		if (kprof_starts[0] <= off) {
			while (hi - lo > 1) {
				uint32_t mid = lo + (hi - lo) / 2;
				if (kprof_starts[mid] <= off) {
					lo = mid;
				} else {
					hi = mid;
				}
			}
			return kprof_image + kprof_starts[lo];
		}
	}
	return (pc & ~(uintptr_t)127) | KPROF_BUCKET;
}

static inline uint32_t
kprof_hash(uint64_t key, unsigned int bits)
{
	return (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> (64 - bits));
}

static void
kprof_vote(uint64_t *cand, uint32_t *votes, uint64_t value)
{
	if (*cand == value) {
		(*votes)++;
	} else if (*votes == 0) {
		*cand = value;
		*votes = 1;
	} else {
		(*votes)--;
	}
}

static void
kprof_vote_merge(uint64_t *cand, uint32_t *votes, uint64_t other, uint32_t other_votes)
{
	if (*cand == other) {
		*votes += other_votes;
	} else if (other_votes > *votes) {
		*cand = other;
		*votes = other_votes - *votes;
	} else {
		*votes -= other_votes;
	}
}

static struct kprof_fn *
kprof_fn_slot(struct kprof_fn *table, unsigned int bits, uintptr_t key)
{
	uint32_t mask = (1u << bits) - 1;
	uint32_t i = kprof_hash(key, bits);

	for (unsigned int probe = 0; probe < KPROF_PROBES; probe++, i = (i + 1) & mask) {
		if (table[i].key == key) {
			return &table[i];
		}
		if (table[i].key == 0) {
			table[i].key = key;
			return &table[i];
		}
	}
	return NULL;
}

static struct kprof_task *
kprof_task_slot(struct kprof_task *table, unsigned int bits, int32_t pid)
{
	uint32_t mask = (1u << bits) - 1;
	uint32_t i = kprof_hash((uint64_t)(uint32_t)pid + 1, bits);

	for (unsigned int probe = 0; probe < KPROF_PROBES; probe++, i = (i + 1) & mask) {
		if (table[i].used && table[i].pid == pid) {
			return &table[i];
		}
		if (!table[i].used) {
			table[i].used = true;
			table[i].pid = pid;
			return &table[i];
		}
	}
	return NULL;
}

static struct kprof_user *
kprof_user_slot(struct kprof_user *table, unsigned int bits, int32_t pid, uint64_t key)
{
	uint32_t mask = (1u << bits) - 1;
	uint32_t i = kprof_hash(key ^ ((uint64_t)(uint32_t)pid << 32), bits);

	for (unsigned int probe = 0; probe < KPROF_PROBES; probe++, i = (i + 1) & mask) {
		if (table[i].key == key && table[i].pid == pid) {
			return &table[i];
		}
		if (table[i].key == 0) {
			table[i].key = key;
			table[i].pid = pid;
			return &table[i];
		}
	}
	return NULL;
}

/* A user PC as something the host can name: the unslid shared cache address, or main executable + offset. */
static uint64_t
kprof_user_location(task_t task, uint64_t pc)
{
	const struct vm_shared_region *sr = task->shared_region;
	uint64_t                       header = task->mach_header_vm_address;

	if (sr != NULL && pc >= sr->sr_base_address && pc - sr->sr_base_address < sr->sr_size) {
		return KPROF_U_DSC | (pc - sr->sr_slide);
	}
	if (header != 0 && pc >= header && pc - header < (256ull << 20)) {
		return KPROF_U_MAIN | (pc - header);
	}
	return KPROF_U_OTHER | (pc & KPROF_U_ADDR);
}

static uint64_t
kprof_user_key(task_t task, uint64_t pc)
{
	uint64_t where = kprof_user_location(task, pc);

	return (where & ~KPROF_U_ADDR) == KPROF_U_OTHER ? KPROF_U_OTHER : (where & ~63ull);
}

/* The caller of the sampled function: LR in a leaf, else the return address in the function's frame record. */
static uintptr_t
kprof_caller(arm_saved_state_t *state, thread_t thread, uintptr_t fn)
{
	uintptr_t caller = kprof_fn_key(get_saved_state_lr(state));
	uintptr_t fp, stack = thread->kernel_stack;

	if (caller != fn) {
		return caller;
	}
	fp = get_saved_state_fp(state);
	if (stack == 0 || (fp & 7) != 0 || fp < stack || fp > stack + kernel_stack_size - 16) {
		return 0;
	}
	return kprof_fn_key(((const uintptr_t *)fp)[1]);
}

/* Up to three return addresses above the sampled function, as function keys: LR when it is a leaf, then frame records. */
static void
kprof_chain(arm_saved_state_t *state, thread_t thread, uintptr_t fn, uintptr_t chain[3])
{
	uintptr_t    fp = get_saved_state_fp(state), stack = thread->kernel_stack;
	unsigned int n = 0;

	chain[0] = chain[1] = chain[2] = 0;
	if (kprof_fn_key(get_saved_state_lr(state)) != fn) {
		chain[n++] = kprof_fn_key(get_saved_state_lr(state));
	}
	while (n < 3 && stack != 0 && (fp & 7) == 0 && fp >= stack && fp <= stack + kernel_stack_size - 16) {
		const uintptr_t *frame = (const uintptr_t *)fp;

		chain[n++] = kprof_fn_key(frame[1]);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
}

static struct kprof_masked *
kprof_masked_slot(struct kprof_masked *table, unsigned int bits, const uintptr_t chain[3])
{
	uint32_t mask = (1u << bits) - 1;
	uint32_t i = kprof_hash(chain[0] ^ (chain[1] << 1) ^ (chain[2] << 2), bits);

	for (unsigned int probe = 0; probe < KPROF_PROBES; probe++, i = (i + 1) & mask) {
		if (table[i].chain[0] == chain[0] && table[i].chain[1] == chain[1] && table[i].chain[2] == chain[2]) {
			return &table[i];
		}
		if (table[i].chain[0] == 0) {
			memcpy(table[i].chain, chain, sizeof(table[i].chain));
			return &table[i];
		}
	}
	return NULL;
}

static void
kprof_sample(struct kprof_cpu *kc)
{
	arm_saved_state_t *state = getCpuDatap()->cpu_int_state;
	struct kprof_buf  *b = kc->buf[kc->active];
	thread_t           thread = current_thread();
	task_t             task = get_threadtask(thread);
	struct kprof_task *ts;
	struct kprof_fn   *f;
	uintptr_t          fn;
	bool               kt;

	if (state == NULL || task == TASK_NULL) {
		return;
	}
	b->samples++;
	ts = kprof_task_slot(b->task, 7, task_pid(task));
	if (ts != NULL && ts->name[0] == '\0') {
		task_best_name(task, ts->name, sizeof(ts->name));
	}
	if (PSR64_IS_USER(get_saved_state_cpsr(state))) {
		struct kprof_user *u = kprof_user_slot(b->user_pc, 11, task_pid(task),
		    kprof_user_key(task, get_saved_state_pc(state)));

		b->user++;
		if (ts != NULL) {
			ts->user++;
		}
		if (u != NULL) {
			u->samples++;
		}
		return;
	}
	b->kernel++;
	if (ts != NULL) {
		ts->kernel++;
	}
	kt = (task == kernel_task);
	if (kt) {
		b->kernel_task++;
	}
	fn = kprof_fn_key(get_saved_state_pc(state));
	f = kprof_fn_slot(b->fn, 10, fn);
	if (f == NULL) {
		b->lost++;
		return;
	}
	f->samples++;
	if (kt) {
		f->kernel_task++;
	}
	kprof_vote(&f->caller, &f->caller_votes, kprof_caller(state, thread, fn));
	kprof_vote(&f->owner, &f->owner_votes, kt ? (KPROF_TID | thread_tid(thread)) : (uint64_t)(uint32_t)task_pid(task));
	if (fn == kprof_unmask[0] || fn == kprof_unmask[1]) {
		uintptr_t           chain[3];
		struct kprof_masked *m;

		kprof_chain(state, thread, fn, chain);
		if (chain[0] != 0 && (m = kprof_masked_slot(b->masked, 8, chain)) != NULL) {
			m->samples++;
			if (kt) {
				m->kernel_task++;
			}
		}
	}
}

static uint64_t
kprof_next(struct kprof_cpu *kc, uint64_t from)
{
	/* +-1/8 of a period of jitter, so a sample never locks onto a periodic kernel activity */
	kc->rng ^= kc->rng << 13;
	kc->rng ^= kc->rng >> 17;
	kc->rng ^= kc->rng << 5;
	return from + kprof_period - kprof_period / 8 + (kc->rng % (kprof_period / 4));
}

/* The next point of this CPU's sampling grid after now: busy bursts of any length are sampled in proportion. */
static uint64_t
kprof_catch_up(struct kprof_cpu *kc, uint64_t now)
{
	uint64_t deadline = kc->deadline;

	if (deadline <= now) {
		deadline = now + kprof_period - (now - deadline) % kprof_period;
	}
	return deadline;
}

void
ioswitch_kprof_expire(void *param0, __unused void *param1)
{
	processor_t       processor = param0;
	struct kprof_cpu *kc = &kprof_cpus[processor->cpu_id];
	uint64_t          now = mach_absolute_time();

	if (!kprof_ready || (unsigned int)processor->cpu_id >= kprof_ncpus) {
		running_timer_clear(processor, RUNNING_TIMER_IOSWITCH_KPROF);
		return;
	}
	kprof_sample(kc);
	kc->deadline = kprof_next(kc, kc->deadline);
	kc->deadline = kprof_catch_up(kc, now);
	running_timer_enter(processor, RUNNING_TIMER_IOSWITCH_KPROF, NULL, kc->deadline, now);
}

/* thread_dispatch, leaving idle: re-arm on this CPU's grid (the deadline went stale while idle). */
void
ioswitch_kprof_running_setup(processor_t processor, uint64_t now)
{
	struct kprof_cpu *kc = &kprof_cpus[processor->cpu_id];

	if (!kprof_ready || (unsigned int)processor->cpu_id >= kprof_ncpus) {
		return;
	}
	kc->armed = true;
	kc->deadline = kprof_catch_up(kc, now);
	running_timer_setup(processor, RUNNING_TIMER_IOSWITCH_KPROF, NULL, kc->deadline, now);
}

/* sleh.c handle_breakpoint, EL0: one console line for a process's first trap (os_crash and friends end in brk). */
void ioswitch_kprof_user_trap(arm_saved_state_t *state, uint32_t esr);
void
ioswitch_kprof_user_trap(arm_saved_state_t *state, uint32_t esr)
{
	static char         names[KPROF_TRAP_NAMES][32];
	static unsigned int used;
	task_t              task = current_task();
	char                name[32];
	uint64_t            pc, lr;
	unsigned int        slot;

	task_best_name(task, name, sizeof(name));
	for (unsigned int i = 0; i < MIN(os_atomic_load(&used, acquire), KPROF_TRAP_NAMES); i++) {
		if (strncmp(names[i], name, sizeof(name)) == 0) {
			return;
		}
	}
	slot = os_atomic_inc_orig(&used, relaxed);
	if (slot >= KPROF_TRAP_NAMES) {
		return;
	}
	strlcpy(names[slot], name, sizeof(names[slot]));
	pc = kprof_user_location(task, get_saved_state_pc(state));
	lr = kprof_user_location(task, get_saved_state_lr(state));
	printf("IOSwitch trap: %s[%d] brk #0x%x at %s%llx, lr %s%llx\n", name, task_pid(task), ISS_BRK_COMMENT(esr),
	    (pc & ~KPROF_U_ADDR) == KPROF_U_DSC ? "dsc 0x" : (pc & ~KPROF_U_ADDR) == KPROF_U_MAIN ? "main+0x" : "0x",
	    pc & KPROF_U_ADDR,
	    (lr & ~KPROF_U_ADDR) == KPROF_U_DSC ? "dsc 0x" : (lr & ~KPROF_U_ADDR) == KPROF_U_MAIN ? "main+0x" : "0x",
	    lr & KPROF_U_ADDR);
}

/* --- summary (thread call) ---------------------------------------------------------------------------------------- */

/* Cross-call on every CPU: the sampler moves to the other buffer; arm CPUs that have not left idle since boot. */
static void
kprof_flip_cpu(__unused void *arg)
{
	processor_t       processor = current_processor();
	struct kprof_cpu *kc = &kprof_cpus[processor->cpu_id];
	uint64_t          now = mach_absolute_time();

	if ((unsigned int)processor->cpu_id >= kprof_ncpus) {
		return;
	}
	kc->active ^= 1;
	if (!kc->armed) {
		kc->armed = true;
		kc->deadline = now + kprof_period;
		running_timer_enter(processor, RUNNING_TIMER_IOSWITCH_KPROF, NULL, kc->deadline, now);
	}
}

static void
kprof_put(const char *fmt, ...) __printflike(1, 2);

static void
kprof_put(const char *fmt, ...)
{
	va_list args;
	int     n;

	if (kprof_len >= sizeof(kprof_line) - 1) {
		return;
	}
	va_start(args, fmt);
	n = vsnprintf(kprof_line + kprof_len, sizeof(kprof_line) - kprof_len, fmt, args);
	va_end(args);
	if (n > 0) {
		kprof_len = MIN(kprof_len + (size_t)n, sizeof(kprof_line) - 1);
	}
}

static void
kprof_flush(void)
{
	os_log(OS_LOG_DEFAULT, "%s", kprof_line);
	kprof_len = 0;
	kprof_line[0] = '\0';
}

static const char *
kprof_task_name(uint64_t owner, char *buf, size_t size)
{
	if (owner & KPROF_TID) {
		snprintf(buf, size, "kt:%llx", owner & ~KPROF_TID);
		return buf;
	}
	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		if (kprof_tasks[i].used && (uint64_t)(uint32_t)kprof_tasks[i].pid == owner) {
			return kprof_tasks[i].name;
		}
	}
	snprintf(buf, size, "pid %llu", owner);
	return buf;
}

/* An address as the build's kernel.macho has it (the prelinked kexts are in it too), with the kext's name outside the
 * kernel image. */
static void
kprof_put_addr(const char *prefix, uintptr_t key)
{
	OSKextLoadedKextSummary kext;
	uintptr_t               addr = key & ~KPROF_BUCKET;
	const char             *name;

	if (key == 0) {
		kprof_put("%s?", prefix);
		return;
	}
	if ((addr < kprof_text_start || addr >= kprof_text_end) &&
	    OSKextGetLoadedKextSummaryForAddress((const void *)addr, &kext) == KERN_SUCCESS) {
		name = kext.name;
		if (strncmp(name, "com.apple.", 10) == 0) {
			name += 10;
		}
		kprof_put("%s%s@%lx", prefix, name, (unsigned long)VM_KERNEL_UNSLIDE(addr));
		return;
	}
	kprof_put("%s%lx", prefix, (unsigned long)VM_KERNEL_UNSLIDE(addr));
}

/* Indices of the n largest non-zero values, largest first. */
static unsigned int
kprof_top(const uint64_t *values, unsigned int count, unsigned int *top, unsigned int n)
{
	unsigned int have = 0;

	for (unsigned int i = 0; i < count; i++) {
		uint64_t     v = values[i];
		unsigned int at;

		if (v == 0 || (have == n && v <= values[top[n - 1]])) {
			continue;
		}
		at = (have < n) ? have++ : n - 1;
		while (at > 0 && values[top[at - 1]] < v) {
			top[at] = top[at - 1];
			at--;
		}
		top[at] = i;
	}
	return have;
}

static int
kprof_thread_cmp(const void *a, const void *b)
{
	uint64_t x = ((const struct kprof_thread *)a)->tid, y = ((const struct kprof_thread *)b)->tid;

	return (x > y) - (x < y);
}

static uint64_t
kprof_thread_prev(uint64_t tid)
{
	const struct kprof_thread *prev = kprof_threads[kprof_thread_cur ^ 1];
	uint32_t lo = 0, hi = kprof_nthreads[kprof_thread_cur ^ 1];

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (prev[mid].tid == tid) {
			return prev[mid].time;
		}
		if (prev[mid].tid < tid) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	return 0;
}

static void
kprof_kernel_task_threads(uint64_t window)
{
	struct kprof_thread *cur = kprof_threads[kprof_thread_cur];
	unsigned int         top[KPROF_TOP_THREADS], ntop;
	uint64_t             delta[KPROF_TOP_THREADS];
	char                 names[KPROF_TOP_THREADS][MAXTHREADNAMESIZE];
	uint32_t             n = 0;
	thread_t             thread;

	task_lock(kernel_task);
	queue_iterate(&kernel_task->threads, thread, thread_t, task_threads) {
		struct recount_usage usage = {};

		if (n == KPROF_THREADS) {
			break;
		}
		if (thread->options & TH_OPT_IDLE_THREAD) {
			continue;
		}
		recount_thread_usage(thread, &usage);
		cur[n].tid = thread_tid(thread);
		cur[n].time = recount_usage_time_mach(&usage);
		n++;
	}
	task_unlock(kernel_task);
	qsort(cur, n, sizeof(cur[0]), kprof_thread_cmp);
	kprof_nthreads[kprof_thread_cur] = n;

	for (uint32_t i = 0; i < n; i++) {
		kprof_vals[i] = cur[i].time - MIN(cur[i].time, kprof_thread_prev(cur[i].tid));
	}
	ntop = kprof_top(kprof_vals, n, top, KPROF_TOP_THREADS);
	for (unsigned int i = 0; i < ntop; i++) {
		delta[i] = kprof_vals[top[i]];
		names[i][0] = '\0';
	}
	task_lock(kernel_task);
	queue_iterate(&kernel_task->threads, thread, thread_t, task_threads) {
		for (unsigned int i = 0; i < ntop; i++) {
			if (cur[top[i]].tid == thread_tid(thread)) {
				thread_get_thread_name(thread, names[i]);
			}
		}
	}
	task_unlock(kernel_task);

	kprof_put("IOSwitch kprof kernel_task: %u threads; ms in %llu ms, name, top sampled fn:", n,
	    window / (NSEC_PER_MSEC));
	for (unsigned int i = 0; i < ntop; i++) {
		uint64_t ns, best_samples = 0;
		uintptr_t best = 0;

		absolutetime_to_nanoseconds(delta[i], &ns);
		for (unsigned int j = 0; j < KPROF_FNS; j++) {
			if (kprof_fns[j].key != 0 && kprof_fns[j].owner == (KPROF_TID | cur[top[i]].tid) &&
			    kprof_fns[j].samples > best_samples) {
				best_samples = kprof_fns[j].samples;
				best = kprof_fns[j].key;
			}
		}
		kprof_put(" %s%llx '%s' %llu", i ? "; " : "", cur[top[i]].tid, names[i], ns / NSEC_PER_MSEC);
		if (best != 0) {
			kprof_put_addr(" ", best);
		}
	}
	kprof_flush();
	kprof_thread_cur ^= 1;
}

/* The busiest processes' user-mode hot spots, two processes per line. */
static void
kprof_user_hot_spots(void)
{
	unsigned int procs[KPROF_TOP_USER], nprocs, top[KPROF_TOP_USER_PC], ntop;

	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		kprof_vals[i] = kprof_tasks[i].used ? kprof_tasks[i].user : 0;
	}
	nprocs = kprof_top(kprof_vals, KPROF_TASKS, procs, KPROF_TOP_USER);
	for (unsigned int p = 0; p < nprocs; p++) {
		const struct kprof_task *t = &kprof_tasks[procs[p]];
		uint32_t other = 0;

		for (unsigned int i = 0; i < KPROF_USER; i++) {
			bool mine = kprof_users[i].key != 0 && kprof_users[i].pid == t->pid;

			kprof_vals[i] = (mine && kprof_users[i].key != KPROF_U_OTHER) ? kprof_users[i].samples : 0;
			if (mine && kprof_users[i].key == KPROF_U_OTHER) {
				other = kprof_users[i].samples;
			}
		}
		ntop = kprof_top(kprof_vals, KPROF_USER, top, KPROF_TOP_USER_PC);
		if (p % 2 == 0) {
			kprof_put("IOSwitch kprof user %u/%u:", p / 2 + 1, (nprocs + 1) / 2);
		}
		kprof_put(" %s[%d] %u:", t->name, t->pid, t->user);
		for (unsigned int i = 0; i < ntop; i++) {
			const struct kprof_user *u = &kprof_users[top[i]];

			kprof_put(" %s %llx %u,", (u->key & ~KPROF_U_ADDR) == KPROF_U_DSC ? "dsc" : "main+",
			    u->key & KPROF_U_ADDR, u->samples);
		}
		kprof_put(" other %u;", other);
		if (p % 2 == 1 || p + 1 == nprocs) {
			kprof_flush();
		}
	}
}

static void
kprof_summary(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t     now, window_abs, window, samples = 0, user = 0, kernel = 0, kernel_task_samples = 0, lost = 0;
	unsigned int top[KPROF_TOP_FNS], ntop;
	char         owner[24];

	cpu_broadcast_xcall_simple(TRUE, kprof_flip_cpu, NULL);
	now = mach_absolute_time();
	window_abs = now - kprof_last_summary;
	kprof_last_summary = now;
	absolutetime_to_nanoseconds(window_abs, &window);

	bzero(kprof_fns, KPROF_FNS * sizeof(kprof_fns[0]));
	bzero(kprof_tasks, KPROF_TASKS * sizeof(kprof_tasks[0]));
	bzero(kprof_users, KPROF_USER * sizeof(kprof_users[0]));
	bzero(kprof_masks, KPROF_MASKED * sizeof(kprof_masks[0]));
	for (unsigned int c = 0; c < kprof_ncpus; c++) {
		struct kprof_cpu *kc = &kprof_cpus[c];
		struct kprof_buf *b = kc->buf[kc->active ^ 1];

		samples += b->samples;
		user += b->user;
		kernel += b->kernel;
		kernel_task_samples += b->kernel_task;
		lost += b->lost;
		for (unsigned int i = 0; i < KPROF_CPU_FNS; i++) {
			const struct kprof_fn *src = &b->fn[i];
			struct kprof_fn       *dst;

			if (src->key == 0) {
				continue;
			}
			dst = kprof_fn_slot(kprof_fns, 12, src->key);
			if (dst == NULL) {
				lost += src->samples;
				continue;
			}
			dst->samples += src->samples;
			dst->kernel_task += src->kernel_task;
			kprof_vote_merge(&dst->caller, &dst->caller_votes, src->caller, src->caller_votes);
			kprof_vote_merge(&dst->owner, &dst->owner_votes, src->owner, src->owner_votes);
		}
		for (unsigned int i = 0; i < KPROF_CPU_MASKED; i++) {
			const struct kprof_masked *src = &b->masked[i];
			struct kprof_masked       *dst;

			if (src->chain[0] == 0) {
				continue;
			}
			dst = kprof_masked_slot(kprof_masks, 10, src->chain);
			if (dst != NULL) {
				dst->samples += src->samples;
				dst->kernel_task += src->kernel_task;
			}
		}
		for (unsigned int i = 0; i < KPROF_CPU_USER; i++) {
			const struct kprof_user *src = &b->user_pc[i];
			struct kprof_user       *dst;

			if (src->key == 0) {
				continue;
			}
			dst = kprof_user_slot(kprof_users, 13, src->pid, src->key);
			if (dst != NULL) {
				dst->samples += src->samples;
			}
		}
		for (unsigned int i = 0; i < KPROF_CPU_TASKS; i++) {
			const struct kprof_task *src = &b->task[i];
			struct kprof_task       *dst;

			if (!src->used) {
				continue;
			}
			dst = kprof_task_slot(kprof_tasks, 8, src->pid);
			if (dst == NULL) {
				continue;
			}
			dst->user += src->user;
			dst->kernel += src->kernel;
			if (dst->name[0] == '\0') {
				strlcpy(dst->name, src->name, sizeof(dst->name));
			}
		}
		bzero(b, sizeof(*b));
	}

	kprof_put("IOSwitch kprof: %llu ms, %u cpus busy/irq %%:", window / NSEC_PER_MSEC, kprof_ncpus);
	for (unsigned int c = 0; c < kprof_ncpus; c++) {
		processor_t          processor = processor_array[c];
		struct recount_usage usage = {};
		uint64_t             idle, irq, idle_ns, irq_ns;

		if (processor == PROCESSOR_NULL) {
			continue;
		}
		recount_processor_usage(&processor->pr_recount, &usage, &idle);
		irq = processor->pr_recount.rpr_interrupt_duration_mach;
		absolutetime_to_nanoseconds(idle - MIN(idle, kprof_cpus[c].last_idle), &idle_ns);
		absolutetime_to_nanoseconds(irq - MIN(irq, kprof_cpus[c].last_irq), &irq_ns);
		kprof_cpus[c].last_idle = idle;
		kprof_cpus[c].last_irq = irq;
		if (!kprof_cpus[c].primed) {
			kprof_cpus[c].primed = true;
			kprof_put(" -");
			continue;
		}
		kprof_put(" %llu/%llu.%llu", (window - MIN(window, idle_ns)) * 100 / window, irq_ns * 100 / window,
		    irq_ns * 1000 / window % 10);
	}
	kprof_put("; irq/ipi/exc per s:");
	for (unsigned int c = 0; c < kprof_ncpus; c++) {
		cpu_data_t *cdp = (cpu_data_t *)CpuDataEntries[c].cpu_data_vaddr;
		uint64_t    irqs, ipis, exc;

		if (cdp == NULL) {
			continue;
		}
		irqs = cdp->cpu_stat.irq_ex_cnt;
		ipis = cdp->cpu_stat.ipi_cnt;
		exc = cdp->cpu_stat.data_ex_cnt + cdp->cpu_stat.instr_ex_cnt;
		kprof_put(" %llu/%llu/%llu", (irqs - kprof_cpus[c].last_irqs) * NSEC_PER_SEC / window,
		    (ipis - kprof_cpus[c].last_ipis) * NSEC_PER_SEC / window, (exc - kprof_cpus[c].last_exc) * NSEC_PER_SEC / window);
		kprof_cpus[c].last_irqs = irqs;
		kprof_cpus[c].last_ipis = ipis;
		kprof_cpus[c].last_exc = exc;
	}
	kprof_put("; samples %llu (%llu ms each): user %llu, kernel %llu (kernel_task %llu), lost %llu", samples,
	    KPROF_PERIOD_NS / NSEC_PER_MSEC, user, kernel, kernel_task_samples, lost);
	kprof_flush();

	for (unsigned int i = 0; i < KPROF_TASKS; i++) {
		kprof_vals[i] = kprof_tasks[i].used ? (uint64_t)kprof_tasks[i].user + kprof_tasks[i].kernel : 0;
	}
	ntop = kprof_top(kprof_vals, KPROF_TASKS, top, KPROF_TOP_TASKS);
	kprof_put("IOSwitch kprof tasks (user+kernel samples):");
	for (unsigned int i = 0; i < ntop; i++) {
		const struct kprof_task *t = &kprof_tasks[top[i]];
		kprof_put("%s %s[%d] %u+%u", i ? "," : "", t->name, t->pid, t->user, t->kernel);
	}
	kprof_flush();

	for (unsigned int i = 0; i < KPROF_FNS; i++) {
		kprof_vals[i] = kprof_fns[i].samples;
	}
	ntop = kprof_top(kprof_vals, KPROF_FNS, top, KPROF_TOP_FNS);
	for (unsigned int half = 0; half < 2; half++) {
		kprof_put("IOSwitch kprof kernel %u/2 (fn samples kernel_task <caller @owner):", half + 1);
		for (unsigned int i = half * KPROF_TOP_FNS / 2; i < MIN(ntop, (half + 1) * KPROF_TOP_FNS / 2); i++) {
			const struct kprof_fn *f = &kprof_fns[top[i]];

			kprof_put_addr(" ", f->key);
			kprof_put(" %u %u", f->samples, f->kernel_task);
			kprof_put_addr(" <", f->caller);
			kprof_put(" @%s;", kprof_task_name(f->owner, owner, sizeof(owner)));
		}
		kprof_flush();
	}

	for (unsigned int i = 0; i < KPROF_MASKED; i++) {
		kprof_vals[i] = kprof_masks[i].samples;
	}
	ntop = kprof_top(kprof_vals, KPROF_MASKED, top, MIN(KPROF_TOP_MASKED, KPROF_TOP_FNS));
	if (ntop != 0) {
		kprof_put("IOSwitch kprof masked (samples kernel_task: masking code < its callers):");
		for (unsigned int i = 0; i < ntop; i++) {
			const struct kprof_masked *m = &kprof_masks[top[i]];

			kprof_put(" %u %u", m->samples, m->kernel_task);
			for (unsigned int j = 0; j < 3 && m->chain[j] != 0; j++) {
				kprof_put_addr(j ? " <" : " ", m->chain[j]);
			}
			kprof_put(";");
		}
		kprof_flush();
	}

	kprof_kernel_task_threads(window);
	kprof_user_hot_spots();
	thread_call_enter_delayed_with_leeway(kprof_call, NULL, now + kprof_window, kprof_window / 10,
	    THREAD_CALL_DELAY_LEEWAY);
}

/* --- boot ---------------------------------------------------------------------------------------------------------- */

/* The kernel's LC_FUNCTION_STARTS, decoded while __LINKEDIT is still mapped (it is jettisoned at lockdown). */
static void
kprof_function_starts(void)
{
	kernel_mach_header_t           *mh = &_mh_execute_header;
	kernel_segment_command_t       *text = getsegbynamefromheader(mh, "__TEXT");
	kernel_segment_command_t       *exec = getsegbynamefromheader(mh, "__TEXT_EXEC");
	kernel_segment_command_t       *linkedit = getsegbynamefromheader(mh, "__LINKEDIT");
	const struct load_command      *lc = (const struct load_command *)(mh + 1);
	const struct linkedit_data_command *fs = NULL;
	const uint8_t                  *p, *end;
	uintptr_t                       slide;
	uint64_t                        off = 0;
	uint32_t                        n = 0;

	if (text == NULL || exec == NULL || linkedit == NULL) {
		return;
	}
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		if (lc->cmd == LC_FUNCTION_STARTS) {
			fs = (const struct linkedit_data_command *)lc;
		}
		lc = (const struct load_command *)((uintptr_t)lc + lc->cmdsize);
	}
	if (fs == NULL || fs->datasize == 0 || fs->dataoff < linkedit->fileoff ||
	    fs->dataoff + fs->datasize > linkedit->fileoff + linkedit->filesize) {
		return;
	}
	slide = (uintptr_t)mh - (uintptr_t)text->vmaddr;   /* 0 if the header is already slid */
	kprof_image = (uintptr_t)mh;
	kprof_text_start = (uintptr_t)exec->vmaddr + slide;
	kprof_text_end = kprof_text_start + (uintptr_t)exec->vmsize;
	p = (const uint8_t *)((uintptr_t)linkedit->vmaddr + slide + (fs->dataoff - linkedit->fileoff));
	end = p + fs->datasize;

	/* ULEB128 deltas from the image start; two passes: count, then fill */
	for (unsigned int pass = 0; pass < 2; pass++) {
		const uint8_t *q = p;

		off = 0;
		n = 0;
		while (q < end && *q != 0) {
			uint64_t delta = 0;
			unsigned int shift = 0;

			do {
				delta |= (uint64_t)(*q & 0x7f) << shift;
				shift += 7;
			} while ((*q++ & 0x80) && q < end && shift < 64);
			off += delta;
			if (off > UINT32_MAX) {
				break;
			}
			if (pass == 1) {
				kprof_starts[n] = (uint32_t)off;
			}
			n++;
		}
		if (pass == 0) {
			/* Large: kalloc_large refuses Z_NOFAIL (panics). Without the table, 128-byte buckets. */
			kprof_starts = kalloc_data(n * sizeof(uint32_t), Z_WAITOK);
			if (kprof_starts == NULL) {
				return;
			}
		}
	}
	kprof_nstarts = n;
}

static void
kprof_init(void)
{
	kprof_ncpus = MIN((unsigned int)ml_get_max_cpu_number() + 1, KPROF_MAX_CPUS);
	nanoseconds_to_absolutetime(KPROF_PERIOD_NS, &kprof_period);
	nanoseconds_to_absolutetime(KPROF_WINDOW_NS, &kprof_window);
	kprof_function_starts();
	kprof_unmask[0] = kprof_fn_key((uintptr_t)&ml_set_interrupts_enabled_with_debug);
	kprof_unmask[1] = kprof_fn_key((uintptr_t)&ml_set_interrupts_enabled);
	/*
	 * These tables are past kalloc's zone sizes, and kalloc_large panics on Z_NOFAIL ("trying to kalloc(Z_NOFAIL)
	 * with a large size"): a diagnostic must not take the boot down, so a refused table leaves kprof off.
	 */
	bool tables = true;
	for (unsigned int c = 0; c < kprof_ncpus; c++) {
		kprof_cpus[c].buf[0] = kalloc_data(sizeof(struct kprof_buf), Z_WAITOK | Z_ZERO);
		kprof_cpus[c].buf[1] = kalloc_data(sizeof(struct kprof_buf), Z_WAITOK | Z_ZERO);
		kprof_cpus[c].rng = 0x9e3779b9u * (c + 1);
		tables = tables && kprof_cpus[c].buf[0] != NULL && kprof_cpus[c].buf[1] != NULL;
	}
	kprof_fns = kalloc_data(KPROF_FNS * sizeof(kprof_fns[0]), Z_WAITOK | Z_ZERO);
	kprof_tasks = kalloc_data(KPROF_TASKS * sizeof(kprof_tasks[0]), Z_WAITOK | Z_ZERO);
	kprof_users = kalloc_data(KPROF_USER * sizeof(kprof_users[0]), Z_WAITOK | Z_ZERO);
	kprof_threads[0] = kalloc_data(KPROF_THREADS * sizeof(struct kprof_thread), Z_WAITOK | Z_ZERO);
	kprof_threads[1] = kalloc_data(KPROF_THREADS * sizeof(struct kprof_thread), Z_WAITOK | Z_ZERO);
	kprof_vals = kalloc_data(KPROF_USER * sizeof(uint64_t), Z_WAITOK | Z_ZERO);
	kprof_masks = kalloc_data(KPROF_MASKED * sizeof(kprof_masks[0]), Z_WAITOK | Z_ZERO);
	if (!tables || kprof_fns == NULL || kprof_tasks == NULL || kprof_users == NULL || kprof_threads[0] == NULL ||
	    kprof_threads[1] == NULL || kprof_vals == NULL || kprof_masks == NULL) {
		os_log(OS_LOG_DEFAULT, "IOSwitch kprof: off (a sample table could not be allocated)");
		return;
	}
	kprof_call = thread_call_allocate_with_options(kprof_summary, NULL, THREAD_CALL_PRIORITY_USER,
	    THREAD_CALL_OPTIONS_ONCE);
	kprof_last_summary = mach_absolute_time();
	os_atomic_store(&kprof_ready, true, release);
	thread_call_enter_delayed_with_leeway(kprof_call, NULL, kprof_last_summary + kprof_window, kprof_window / 10,
	    THREAD_CALL_DELAY_LEEWAY);
	os_log(OS_LOG_DEFAULT, "IOSwitch kprof: sampling every %llu ms of CPU work, %u kernel functions%s, summary every %llu s",
	    KPROF_PERIOD_NS / NSEC_PER_MSEC, kprof_nstarts, kprof_nstarts ? "" : " (no LC_FUNCTION_STARTS: 128-byte buckets)",
	    KPROF_WINDOW_NS / NSEC_PER_SEC);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, kprof_init);
