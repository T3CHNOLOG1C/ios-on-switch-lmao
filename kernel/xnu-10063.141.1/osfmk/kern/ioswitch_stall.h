/*
 * IOSwitch UI stall reports (ioswitch_stall.c): the decisions, free of kernel types so kernel/tests can drive them.
 */
#ifndef _KERN_IOSWITCH_STALL_H_
#define _KERN_IOSWITCH_STALL_H_

#include <stdbool.h>
#include <stdint.h>

/* A main thread off core this long, other than idle in its run loop, is stalled. */
#define IOSWITCH_STALL_NS 2000000000ull

/* thread.h THREAD_NOT_RUNNABLE */
#define IOSWITCH_STALL_NOT_RUNNABLE (~0ull)

enum ioswitch_stall_kind {
	IOSWITCH_STALL_NONE = 0,
	IOSWITCH_STALL_WAITING,  /* blocked */
	IOSWITCH_STALL_RUNNABLE, /* on a run queue, not run */
};

/* The scheduler's view of one thread, taken under its thread lock (mach absolute time). */
struct ioswitch_stall_sample {
	bool     waiting;       /* TH_WAIT without TH_RUN */
	bool     runnable;      /* TH_RUN without TH_WAIT, on a run queue */
	bool     running;       /* TH_RUN, on no run queue: on a core, or being dispatched to one */
	bool     run_loop_idle; /* waiting in a port set receive (block hint kThreadWaitPortSetReceive) */
	uint64_t now;
	uint64_t last_run;      /* thread->last_run_time: when it last left a core */
	uint64_t made_runnable; /* thread->last_made_runnable_time */
	uint64_t threshold;     /* IOSWITCH_STALL_NS in mach absolute time */
};

static inline enum ioswitch_stall_kind
ioswitch_stall_kind(const struct ioswitch_stall_sample *s)
{
	if (s->waiting && !s->run_loop_idle && s->now > s->last_run && s->now - s->last_run >= s->threshold) {
		return IOSWITCH_STALL_WAITING;
	}
	if (s->runnable && s->made_runnable != IOSWITCH_STALL_NOT_RUNNABLE && s->now > s->made_runnable &&
	    s->now - s->made_runnable >= s->threshold) {
		return IOSWITCH_STALL_RUNNABLE;
	}
	return IOSWITCH_STALL_NONE;
}

/*
 * One stall episode per time the thread left a core: last_run_time only changes when it runs again, so a stall is
 * reported once however long it lasts, and its end is the first sample that finds it running or with a new
 * last_run_time.
 */
struct ioswitch_stall_episode {
	bool     open;
	uint64_t key;      /* last_run_time of the episode reported */
	uint64_t since;    /* when it began: blocked (last_run_time) or made runnable */
	enum ioswitch_stall_kind kind;
};

enum ioswitch_stall_event {
	IOSWITCH_STALL_QUIET = 0,
	IOSWITCH_STALL_BEGAN,
	IOSWITCH_STALL_ENDED,
};

static inline enum ioswitch_stall_event
ioswitch_stall_step(struct ioswitch_stall_episode *e, const struct ioswitch_stall_sample *s)
{
	enum ioswitch_stall_kind kind;

	if (e->open) {
		if (s->running || s->last_run != e->key) {
			e->open = false;
			return IOSWITCH_STALL_ENDED;
		}
		return IOSWITCH_STALL_QUIET;
	}
	kind = ioswitch_stall_kind(s);
	if (kind == IOSWITCH_STALL_NONE || s->last_run == e->key) {
		return IOSWITCH_STALL_QUIET;
	}
	e->open = true;
	e->key = s->last_run;
	e->since = kind == IOSWITCH_STALL_RUNNABLE ? s->made_runnable : s->last_run;
	e->kind = kind;
	return IOSWITCH_STALL_BEGAN;
}

/* block_hint.h's thread_snapshot_wait_flags, by value. */
static inline const char *
ioswitch_stall_wait_name(unsigned int hint)
{
	static const char *const names[] = {
		"no hint", "kernel mutex", "mach_msg receive", "mach_msg receive on a port set", "mach_msg send",
		"mach_msg send, in transit", "semaphore", "kernel rwlock read", "kernel rwlock write",
		"kernel rwlock upgrade", "ulock", "pthread mutex", "pthread rwlock read", "pthread rwlock write",
		"pthread condvar", "parked workqueue", "workloop sync wait", "wait4", "sleep with inheritor", "eventlink",
		"compressor",
	};
	return hint < sizeof(names) / sizeof(names[0]) ? names[hint] : "unknown wait";
}
#define IOSWITCH_STALL_WAIT_NAMES 21u

/* A user address as the host can name it: dyld shared cache (unslid) or main executable + offset. */
enum ioswitch_stall_where {
	IOSWITCH_STALL_RAW = 0,
	IOSWITCH_STALL_DSC,
	IOSWITCH_STALL_MAIN,
};

static inline enum ioswitch_stall_where
ioswitch_stall_user_where(uint64_t pc, uint64_t sr_base, uint64_t sr_size, uint64_t sr_slide, uint64_t header,
    uint64_t *value)
{
	if (sr_size != 0 && pc >= sr_base && pc - sr_base < sr_size) {
		*value = pc - sr_slide;
		return IOSWITCH_STALL_DSC;
	}
	if (header != 0 && pc >= header && pc - header < (256ull << 20)) {
		*value = pc - header;
		return IOSWITCH_STALL_MAIN;
	}
	*value = pc;
	return IOSWITCH_STALL_RAW;
}

#endif /* _KERN_IOSWITCH_STALL_H_ */
