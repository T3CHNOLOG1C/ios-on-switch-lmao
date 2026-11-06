/*
 * Switch UI stall reports: one os_log line when SpringBoard's or backboardd's main thread has been off core for
 * SWITCH_STALL_NS, blocked on anything but its idle run loop or runnable and not run, and one when it runs again.
 *
 * Detection rides the scheduler's maintenance tick (sched_timeshare_maintenance_continue, woken from context switches
 * and quantum expiry at most every sched_tick_interval): no timer of its own, no wakeup of an idle system. The tick
 * samples the two threads under their thread locks and, when a stall begins, takes the snapshot there; the line is
 * written on a thread call because it reads the stalled thread's user stack (copyinmap, which can fault) and names
 * processes (proc_name, which takes proc_list_lock).
 *
 * The line names the wait: block hint, wait queue type and object, wait event, the kernel continuation or the first
 * kernel return addresses (unslid: `atos -o kernel.macho`), the user PC, LR and frame chain (dsc <unslid> for the
 * shared cache, main+<offset>), and who it waits for: the process that received the request a special reply port
 * waits on (or the receiver of the port it is still queued at), the receiver of the port a send is blocked on, or the
 * thread or workqueue at the end of its turnstile's inheritor chain (lock holders, sync IPC servers).
 *
 * Locking: a waiter is woken under its wait queue's lock and then its thread lock, so with the thread lock held every
 * wait queue, port and turnstile lock is only try-locked; a lock that is held is reported as such. The objects
 * followed are alive while the thread lock is held: the thread stays enqueued on its wait queue (its wakeup needs the
 * thread lock), so its own turnstile and the port it belongs to cannot be torn down; inheritors and a special reply
 * port's sync inheritor port carry references while their holder is locked. Ports further up a chain carry none and
 * are never dereferenced.
 */

#include <kern/switch_stall.h>

#include <ipc/ipc_port.h>
#include <ipc/ipc_space.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/simple_lock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <kern/turnstile.h>
#include <kern/waitq.h>
#include <mach/vm_param.h>
#include <os/log.h>
#include <ptrauth.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_shared_region.h>

void switch_stall_exec(thread_t thread);
void switch_stall_tick(uint64_t now);
void switch_stall_report(thread_call_param_t p0, thread_call_param_t p1);

extern turnstile_type_t switch_turnstile_type(struct turnstile *turnstile);
extern int switch_workq_pid(void *workq);
extern void proc_name(int pid, char *buf, int size);
extern int vsnprintf(char *, size_t, const char *, va_list);

#define STALL_CHECK_NS    250000000ull /* the most often the tick samples */
#define STALL_HOPS        8
#define STALL_KFRAMES     6
#define STALL_UFRAMES     8
#define STALL_LINE        900          /* under the 1 KiB os_log record */

struct stall_owner {
	int         pid;
	uint64_t    tid;           /* 0: a process, not a thread */
	const char *via;
};

struct stall_snapshot {
	uint64_t           taken;
	uint64_t           tid;
	int                state;
	int                sched_pri;
	unsigned int       hint;
	int                wq_type;
	uintptr_t          wq_object;
	uint64_t           event;
	uintptr_t          continuation;
	uintptr_t          kframes[STALL_KFRAMES];
	unsigned int       nkframes;
	uint64_t           pc, lr, fp;
	bool               special_reply;
	bool               locked;   /* a lock on the way was held: the owner may be missing */
	struct stall_owner owner;
	task_t             task;     /* +1 */
};

struct stall_slot {
	const char                   *name;
	thread_t                      thread;    /* +1: the main thread of the last exec of `name` */
	struct switch_stall_episode episode;
	struct stall_snapshot         snap;      /* taken when the episode began; snap.task +1 while report is set */
	bool                          report;    /* a begin line is due */
	bool                          ended;     /* an end line is due */
	uint64_t                      ended_ns;  /* that episode's length, to the sample that saw it end */
};

static struct stall_slot stall_slots[] = {
	{ .name = "SpringBoard" },
	{ .name = "backboardd" },
};

static SIMPLE_LOCK_DECLARE(stall_lock, 0);
static thread_call_t stall_call;
static uint64_t      stall_threshold, stall_check, stall_last;

static void stall_snapshot(thread_t t, struct stall_snapshot *ss);

/* kern_exec.c, after a successful posix_spawn or execve: the new main thread, if its process is a UI one. */
void
switch_stall_exec(thread_t thread)
{
	char        name[32];
	thread_t    old = THREAD_NULL;
	task_t      stale = TASK_NULL;
	task_t      task = get_threadtask(thread);

	task_best_name(task, name, sizeof(name));
	for (unsigned int i = 0; i < sizeof(stall_slots) / sizeof(stall_slots[0]); i++) {
		struct stall_slot *slot = &stall_slots[i];

		if (strcmp(name, slot->name) != 0) {
			continue;
		}
		if (os_atomic_load(&stall_call, acquire) == NULL) {
			thread_call_t call = thread_call_allocate_with_options(switch_stall_report, NULL,
			    THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);

			nanoseconds_to_absolutetime(SWITCH_STALL_NS, &stall_threshold);
			nanoseconds_to_absolutetime(STALL_CHECK_NS, &stall_check);
			if (call != NULL && !os_atomic_cmpxchg(&stall_call, NULL, call, release)) {
				thread_call_free(call);
			}
		}
		thread_reference(thread);
		simple_lock(&stall_lock, LCK_GRP_NULL);
		old = slot->thread;
		if (slot->report) {
			stale = slot->snap.task;
		}
		slot->thread = thread;
		slot->episode = (struct switch_stall_episode){ 0 };
		slot->report = slot->ended = false;
		simple_unlock(&stall_lock);
		break;
	}
	if (stale != TASK_NULL) {
		task_deallocate(stale);
	}
	if (old != THREAD_NULL) {
		thread_deallocate(old);
	}
}

static void
stall_sample(thread_t t, uint64_t now, struct switch_stall_sample *s)
{
	const int state = t->state;
	const bool queued = thread_get_runq(t) != PROCESSOR_NULL;

	s->waiting = (state & (TH_WAIT | TH_RUN)) == TH_WAIT;
	s->runnable = (state & (TH_WAIT | TH_RUN)) == TH_RUN && queued;
	s->running = (state & TH_RUN) && !queued;
	s->run_loop_idle = t->block_hint == kThreadWaitPortSetReceive;
	s->now = now;
	s->last_run = t->last_run_time;
	s->made_runnable = t->last_made_runnable_time;
	s->threshold = stall_threshold;
}

/* sched_prim.c sched_timeshare_maintenance_continue, on the scheduler's maintenance thread. */
void
switch_stall_tick(uint64_t now)
{
	bool due = false;

	if (os_atomic_load(&stall_call, acquire) == NULL || now - stall_last < stall_check) {
		return;
	}
	stall_last = now;
	simple_lock(&stall_lock, LCK_GRP_NULL);
	for (unsigned int i = 0; i < sizeof(stall_slots) / sizeof(stall_slots[0]); i++) {
		struct stall_slot           *slot = &stall_slots[i];
		struct switch_stall_sample sample;
		thread_t                     t = slot->thread;
		spl_t                        s;

		if (t == THREAD_NULL) {
			continue;
		}
		s = splsched();
		thread_lock(t);
		if (t->state & TH_TERMINATE) {
			thread_unlock(t);
			splx(s);
			continue;
		}
		stall_sample(t, now, &sample);
		switch (switch_stall_step(&slot->episode, &sample)) {
		case SWITCH_STALL_BEGAN:
			if (!slot->report) {
				slot->snap = (struct stall_snapshot){ 0 };
				stall_snapshot(t, &slot->snap);
				slot->snap.task = get_threadtask(t);
				task_reference(slot->snap.task);
				slot->report = due = true;
			}
			break;
		case SWITCH_STALL_ENDED:
			absolutetime_to_nanoseconds(now - slot->episode.since, &slot->ended_ns);
			slot->ended = due = true;
			break;
		case SWITCH_STALL_QUIET:
		default:
			break;
		}
		thread_unlock(t);
		splx(s);
	}
	simple_unlock(&stall_lock);
	if (due) {
		thread_call_enter(stall_call);
	}
}

static void
stall_port_receiver(ipc_port_t port, const char *via, struct stall_snapshot *ss)
{
	ipc_space_t space;

	if (!ip_mq_lock_try(port)) {
		ss->locked = true;
		return;
	}
	if (ip_active(port) && (space = ip_get_receiver(port)) != IS_NULL && space->is_task != TASK_NULL) {
		ss->owner.pid = task_pid(space->is_task);
		ss->owner.via = via;
	}
	ip_mq_unlock(port);
}

/* The special reply port a sync request waits on: who received the request, else where it is queued. */
static void
stall_special_reply(ipc_port_t port, struct stall_snapshot *ss)
{
	ipc_port_t dest = IP_NULL;

	if (!ip_mq_lock_try(port)) {
		ss->locked = true;
		return;
	}
	if (ip_active(port) && port->ip_specialreply) {
		ss->special_reply = true;
		if (ipc_special_reply_get_pid_locked(port) != 0) {
			ss->owner.pid = ipc_special_reply_get_pid_locked(port);
			ss->owner.via = "received the request";
		} else if (port->ip_sync_link_state == PORT_SYNC_LINK_PORT) {
			dest = port->ip_sync_inheritor_port;
		}
	}
	if (dest != IP_NULL) {
		stall_port_receiver(dest, "receives the port the request is queued at", ss);
	}
	ip_mq_unlock(port);
}

/* The end of the turnstile's inheritor chain: the thread or workqueue that must run for the waiter to wake. */
static void
stall_inheritor(struct turnstile *ts, struct stall_snapshot *ss)
{
	struct turnstile *held = TURNSTILE_NULL;

	for (unsigned int hop = 0; ts != TURNSTILE_NULL && hop < STALL_HOPS; hop++) {
		struct turnstile        *next = TURNSTILE_NULL;
		turnstile_update_flags_t flags;
		void                    *inheritor;

		if (!waitq_lock_try(&ts->ts_waitq)) {
			ss->locked = true;
			break;
		}
		if (held != TURNSTILE_NULL) {
			waitq_unlock(&held->ts_waitq);
		}
		held = ts;
		flags = ts->ts_inheritor_flags;
		inheritor = ts->ts_inheritor;
		if (inheritor == NULL) {
			break;
		}
		if (flags & TURNSTILE_INHERITOR_THREAD) {
			thread_t owner = (thread_t)inheritor;

			ss->owner.pid = task_pid(get_threadtask(owner));
			ss->owner.tid = thread_tid(owner);
			ss->owner.via = "turnstile";
			break;
		}
		if (flags & TURNSTILE_INHERITOR_WORKQ) {
			ss->owner.pid = switch_workq_pid(inheritor);
			ss->owner.tid = 0;
			ss->owner.via = "workqueue";
			break;
		}
		if (flags & TURNSTILE_INHERITOR_TURNSTILE) {
			next = (struct turnstile *)inheritor;
		}
		ts = next;
	}
	if (held != TURNSTILE_NULL) {
		waitq_unlock(&held->ts_waitq);
	}
}

static void
stall_owner(thread_t t, struct stall_snapshot *ss)
{
	waitq_t           wq = t->waitq;
	struct turnstile *ts;

	if (waitq_is_null(wq) || waitq_type(wq) == WQT_PORT_SET) {
		return;
	}
	ts = thread_get_waiting_turnstile(t);
	stall_inheritor(ts, ss);
	if (ss->owner.pid != 0 && ss->owner.tid != 0) {
		return;
	}
	if (waitq_type(wq) == WQT_PORT) {
		stall_special_reply(ip_from_waitq(wq.wq_q), ss);
	} else if (ts != TURNSTILE_NULL && switch_turnstile_type(ts) == TURNSTILE_SYNC_IPC && ts->ts_proprietor != 0) {
		ipc_port_t port = (ipc_port_t)ts->ts_proprietor;

		if (t->block_hint == kThreadWaitPortSend) {
			stall_port_receiver(port, "receives the port the send is blocked on", ss);
		} else {
			stall_special_reply(port, ss);
		}
	}
}

/* Under the thread lock: the wait, the owner and the kernel half of the stack. */
static void
stall_snapshot(thread_t t, struct stall_snapshot *ss)
{
	const arm_saved_state_t *user = get_user_regs(t);

	ss->taken = mach_absolute_time();
	ss->tid = thread_tid(t);
	ss->state = t->state;
	ss->sched_pri = t->sched_pri;
	ss->hint = t->block_hint;
	ss->event = VM_KERNEL_UNSLIDE_OR_PERM(t->wait_event);
	if (!waitq_is_null(t->waitq)) {
		ss->wq_type = waitq_type(t->waitq);
		ss->wq_object = VM_KERNEL_UNSLIDE_OR_PERM(t->waitq.wq_q);
	}
	if (t->continuation != NULL) {
		ss->continuation = VM_KERNEL_UNSLIDE((uintptr_t)ptrauth_strip((void *)t->continuation,
		    ptrauth_key_function_pointer));
	} else if (t->kernel_stack != 0) {
		const vm_offset_t base = t->kernel_stack, top = base + kernel_stack_size;
		const arm_kernel_saved_state_t *k = &thread_get_kernel_state(t)->machine.ss;
		uintptr_t fp = k->fp;

		ss->kframes[ss->nkframes++] = VM_KERNEL_UNSLIDE((uintptr_t)ptrauth_strip((void *)k->lr,
		    ptrauth_key_return_address));
		while (ss->nkframes < STALL_KFRAMES && (fp & 7) == 0 && fp >= base && fp + 16 <= top) {
			const uintptr_t *frame = (const uintptr_t *)fp;

			ss->kframes[ss->nkframes++] = VM_KERNEL_UNSLIDE((uintptr_t)ptrauth_strip((void *)frame[1],
			    ptrauth_key_return_address));
			if (frame[0] <= fp) {
				break;
			}
			fp = frame[0];
		}
	}
	if (user != NULL) {
		ss->pc = get_saved_state_pc(user);
		ss->lr = get_saved_state_lr(user);
		ss->fp = get_saved_state_fp(user);
	}
	if ((ss->state & (TH_WAIT | TH_RUN)) == TH_WAIT) {
		stall_owner(t, ss);
	}
}

struct stall_line {
	char   text[STALL_LINE];
	size_t at;
};

static void
stall_put(struct stall_line *l, const char *fmt, ...) __printflike(2, 3);

static void
stall_put(struct stall_line *l, const char *fmt, ...)
{
	va_list ap;

	if (l->at >= sizeof(l->text)) {
		return;
	}
	va_start(ap, fmt);
	l->at += (size_t)vsnprintf(l->text + l->at, sizeof(l->text) - l->at, fmt, ap);
	va_end(ap);
}

static void
stall_put_user(struct stall_line *l, task_t task, const char *label, uint64_t pc)
{
	const struct vm_shared_region *sr = task->shared_region;
	uint64_t                       value;

	pc = (uint64_t)ptrauth_strip((void *)pc, ptrauth_key_return_address);
	switch (switch_stall_user_where(pc, sr ? sr->sr_base_address : 0, sr ? sr->sr_size : 0, sr ? sr->sr_slide : 0,
	    task->mach_header_vm_address, &value)) {
	case SWITCH_STALL_DSC:
		stall_put(l, "%sdsc 0x%llx", label, value);
		break;
	case SWITCH_STALL_MAIN:
		stall_put(l, "%smain+0x%llx", label, value);
		break;
	case SWITCH_STALL_RAW:
	default:
		stall_put(l, "%s0x%llx", label, value);
		break;
	}
}

static const char *
stall_wq_name(int type)
{
	switch (type) {
	case WQT_QUEUE:
		return "wait queue";
	case WQT_TURNSTILE:
		return "turnstile";
	case WQT_PORT:
		return "port";
	case WQT_SELECT:
		return "select";
	case WQT_PORT_SET:
		return "port set";
	case WQT_SELECT_SET:
		return "select set";
	default:
		return "no wait queue";
	}
}

/* Consumes ss.task's reference. */
static void
stall_report_one(const char *name, struct stall_snapshot ss, struct switch_stall_episode episode)
{
	struct stall_line l = { .at = 0 };
	uint64_t          off_ns = 0, since_ns = 0;
	char              owner[32] = "";
	vm_map_t          map = VM_MAP_NULL;

	absolutetime_to_nanoseconds(ss.taken - episode.since, &off_ns);
	absolutetime_to_nanoseconds(episode.since, &since_ns);
	stall_put(&l, "%s[%d] main thread 0x%llx %s %llu.%llu s (since %llu.%llu s of uptime, pri %d):", name,
	    task_pid(ss.task), ss.tid, episode.kind == SWITCH_STALL_RUNNABLE ? "runnable, not run, for" : "blocked",
	    off_ns / 1000000000ull, off_ns / 100000000ull % 10, since_ns / 1000000000ull, since_ns / 100000000ull % 10,
	    ss.sched_pri);
	if ((ss.state & (TH_WAIT | TH_RUN)) == TH_WAIT) {
		stall_put(&l, " %s%s on %s 0x%lx, event 0x%llx;", switch_stall_wait_name(ss.hint),
		    ss.special_reply ? " (special reply port)" : "", stall_wq_name(ss.wq_type), ss.wq_object, ss.event);
		if (ss.owner.pid != 0) {
			proc_name(ss.owner.pid, owner, sizeof(owner));
			stall_put(&l, " waits for %s[%d]", owner, ss.owner.pid);
			if (ss.owner.tid != 0) {
				stall_put(&l, " thread 0x%llx", ss.owner.tid);
			}
			stall_put(&l, " (%s);", ss.owner.via);
		} else if (ss.locked) {
			stall_put(&l, " owner not read (a lock on the way was held);");
		}
	} else {
		stall_put(&l, " %s;", (ss.state & TH_SUSP) ? "suspended" : "on a run queue");
	}
	if (ss.continuation != 0) {
		stall_put(&l, " kernel continuation 0x%lx;", ss.continuation);
	} else if (ss.nkframes != 0) {
		stall_put(&l, " kernel");
		for (unsigned int i = 0; i < ss.nkframes; i++) {
			stall_put(&l, " 0x%lx", ss.kframes[i]);
		}
		stall_put(&l, ";");
	}
	stall_put_user(&l, ss.task, " user pc ", ss.pc);
	stall_put_user(&l, ss.task, " lr ", ss.lr);
	map = get_task_map_reference(ss.task);
	if (map != VM_MAP_NULL) {
		uint64_t fp = ss.fp;

		stall_put(&l, ", frames");
		for (unsigned int i = 0; i < STALL_UFRAMES && fp != 0 && (fp & 7) == 0; i++) {
			uint64_t frame[2];

			if (copyinmap(map, fp, frame, sizeof(frame)) != KERN_SUCCESS || frame[1] == 0) {
				break;
			}
			stall_put_user(&l, ss.task, " ", frame[1]);
			if (frame[0] <= fp) {
				break;
			}
			fp = frame[0];
		}
		vm_map_deallocate(map);
	}
	task_deallocate(ss.task);
	os_log(OS_LOG_DEFAULT, "SwitchStall: %s", l.text);
}

void
switch_stall_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	for (unsigned int i = 0; i < sizeof(stall_slots) / sizeof(stall_slots[0]); i++) {
		struct stall_slot            *slot = &stall_slots[i];
		struct switch_stall_episode episode;
		struct stall_snapshot         snap;
		thread_t                      t;
		bool                          report, ended;
		uint64_t                      ended_ns;

		simple_lock(&stall_lock, LCK_GRP_NULL);
		t = slot->thread;
		report = slot->report;
		ended = slot->ended;
		ended_ns = slot->ended_ns;
		episode = slot->episode;
		snap = slot->snap;
		slot->report = slot->ended = false;
		if (t != THREAD_NULL && (report || ended)) {
			thread_reference(t);
		}
		simple_unlock(&stall_lock);
		if (t == THREAD_NULL || !(report || ended)) {
			continue;
		}
		if (report) {
			stall_report_one(slot->name, snap, episode);
		}
		if (ended) {
			os_log(OS_LOG_DEFAULT, "SwitchStall: %s[%d] main thread 0x%llx ran again %llu.%llu s after it %s",
			    slot->name, task_pid(get_threadtask(t)), thread_tid(t), ended_ns / 1000000000ull,
			    ended_ns / 100000000ull % 10,
			    episode.kind == SWITCH_STALL_RUNNABLE ? "was made runnable" : "blocked");
		}
		thread_deallocate(t);
	}
}

#if DEVELOPMENT || DEBUG
/*
 * The frame timeline (NativeLegacyDisplay's "SwitchFrame" line) names the scheduling of the thread that submitted a
 * swap: base and current priority, mode (1 realtime, 2 fixed, 3 timeshare) and effective QoS. Unlocked reads: a
 * snapshot for a log line.
 */
void switch_thread_sched(thread_t t, int *base_pri, int *sched_pri, int *mode, int *qos);

void
switch_thread_sched(thread_t t, int *base_pri, int *sched_pri, int *mode, int *qos)
{
	*base_pri = t->base_pri;
	*sched_pri = t->sched_pri;
	*mode = t->sched_mode;
	*qos = t->effective_policy.thep_qos;
}
#endif
