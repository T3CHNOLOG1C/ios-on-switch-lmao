#include "xnu_sched_stats_harness.h"

#include <stdio.h>

#include "switch_sched_stats.h"

static struct switch_sched_window window;

unsigned xnu_sched_buckets(void)
{
    return SWITCH_SCHED_BUCKETS;
}

unsigned xnu_sched_bucket(uint64_t us)
{
    return switch_sched_bucket(us);
}

void xnu_sched_reset(void)
{
    window = (struct switch_sched_window){0};
}

void xnu_sched_note(uint64_t us)
{
    switch_sched_note(&window, us);
}

void xnu_sched_set_counts(uint32_t failsafe_runs, uint32_t limit_blocks, uint64_t limit_us)
{
    window.failsafe_runs = failsafe_runs;
    window.limit_blocks = limit_blocks;
    window.limit_us = limit_us;
}

uint64_t xnu_sched_percentile(unsigned pct)
{
    return switch_sched_percentile(&window, pct);
}

uint64_t xnu_sched_max(void)
{
    return window.max_us;
}

uint32_t xnu_sched_runs(void)
{
    return window.runs;
}

size_t xnu_sched_format(char *out, size_t size, const char *label, int pid, uint8_t base_pri, uint8_t sched_pri,
                        uint8_t mode, uint8_t qos, uint8_t bucket)
{
    const struct switch_sched_class c = {
        .base_pri = base_pri,
        .sched_pri = sched_pri,
        .mode = mode,
        .qos = qos,
        .bucket = bucket,
    };
    return switch_sched_format(out, size, label, pid, &c, &window);
}
