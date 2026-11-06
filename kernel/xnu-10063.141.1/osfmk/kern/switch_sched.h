/*
 * Switch scheduling stats (switch_sched_stats.c): the events Apple's files report, one call each. Debug kernels only; a
 * release kernel compiles every call to nothing.
 */
#ifndef _KERN_SWITCH_SCHED_H_
#define _KERN_SWITCH_SCHED_H_

#include <stdint.h>
#include <sys/cdefs.h>
#include <kern/kern_types.h>
#include <os/atomic_private.h>

__BEGIN_DECLS

#if DEVELOPMENT || DEBUG

extern uint32_t switch_sched_watching;

void switch_sched_dispatched(thread_t thread, uint64_t now);
void switch_sched_named(thread_t thread, const char *name);
void switch_sched_role(task_t task, int role);
void switch_sched_limit_block(thread_t thread, uint64_t until);
void switch_sched_tick(uint64_t now);
void switch_sched_boot_complete(void);

/* thread_invoke, under the incoming thread's lock: the scheduler's hot path, so one load while nothing is watched. */
static inline void
switch_sched_on_core(thread_t thread, uint64_t now)
{
	if (__improbable(os_atomic_load(&switch_sched_watching, relaxed) != 0)) {
		switch_sched_dispatched(thread, now);
	}
}

#else /* DEVELOPMENT || DEBUG */

static inline void
switch_sched_on_core(__unused thread_t thread, __unused uint64_t now)
{
}

static inline void
switch_sched_named(__unused thread_t thread, __unused const char *name)
{
}

static inline void
switch_sched_role(__unused task_t task, __unused int role)
{
}

static inline void
switch_sched_limit_block(__unused thread_t thread, __unused uint64_t until)
{
}

static inline void
switch_sched_tick(__unused uint64_t now)
{
}

static inline void
switch_sched_boot_complete(void)
{
}

#endif /* DEVELOPMENT || DEBUG */

__END_DECLS

#endif /* _KERN_SWITCH_SCHED_H_ */
