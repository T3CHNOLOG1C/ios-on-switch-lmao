#include "xnu_stall_harness.h"

#include "ioswitch_stall.h"

static struct ioswitch_stall_episode episode;

static struct ioswitch_stall_sample convert(const struct xnu_stall_sample *s, uint64_t threshold)
{
    struct ioswitch_stall_sample out = {
        .waiting = s->waiting,
        .runnable = s->runnable,
        .running = s->running,
        .run_loop_idle = s->run_loop_idle,
        .now = s->now,
        .last_run = s->last_run,
        .made_runnable = s->made_runnable,
        .threshold = threshold,
    };
    return out;
}

uint64_t xnu_stall_threshold_ns(void)
{
    return IOSWITCH_STALL_NS;
}

uint64_t xnu_stall_not_runnable(void)
{
    return IOSWITCH_STALL_NOT_RUNNABLE;
}

int xnu_stall_kind(const struct xnu_stall_sample *s, uint64_t threshold)
{
    const struct ioswitch_stall_sample sample = convert(s, threshold);
    return (int)ioswitch_stall_kind(&sample);
}

void xnu_stall_reset(void)
{
    const struct ioswitch_stall_episode none = {0};
    episode = none;
}

int xnu_stall_step(const struct xnu_stall_sample *s, uint64_t threshold)
{
    const struct ioswitch_stall_sample sample = convert(s, threshold);
    return (int)ioswitch_stall_step(&episode, &sample);
}

uint64_t xnu_stall_since(void)
{
    return episode.since;
}

int xnu_stall_episode_kind(void)
{
    return (int)episode.kind;
}

const char *xnu_stall_wait_name(unsigned hint)
{
    return ioswitch_stall_wait_name(hint);
}

unsigned xnu_stall_wait_names(void)
{
    return IOSWITCH_STALL_WAIT_NAMES;
}

int xnu_stall_user_where(uint64_t pc, uint64_t sr_base, uint64_t sr_size, uint64_t sr_slide, uint64_t header,
                         uint64_t *value)
{
    return (int)ioswitch_stall_user_where(pc, sr_base, sr_size, sr_slide, header, value);
}
