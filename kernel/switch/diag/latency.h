// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch frame diagnostics (frames.c, debug kernels): how long a touch takes to reach the panel, stage by stage. Plain
 * data and text like frames.h, so kernel/tests runs it on the host; the including file declares snprintf.
 *
 * One sample is in flight at a time. It starts when backboardd delivers the oldest touch report not yet delivered
 * (frames.h's delivery) and follows that report through the receiving process's next Core Animation commit, the first
 * vsync after the commit (the render server draws once per vsync), and the swap drawn for that vsync to its scan-out.
 * A drag gives about one sample a frame, the age of the oldest input that frame shows.
 *   deliver  report -> backboardd's message to the app       app      delivery -> the app's commit
 *   wait     commit -> the render server's next vsync        compose  vsync -> the swap's submission
 *   show     submission -> scan-out start (GPU tail and the wait for the frame boundary)
 * A sample that nothing answers within SWITCH_LATENCY_STALE_US (a touch on still content) is dropped and counted.
 * Times are microseconds of one monotonic clock.
 */
#ifndef SWITCH_DIAG_LATENCY_H
#define SWITCH_DIAG_LATENCY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SWITCH_LATENCY_STALE_US 250000

/* The id of QuartzCore's commit message to the render server (21H461: a transaction sends one per context it changed;
 * its other messages to backboardd, an event acknowledgement among them, are not commits). */
#define SWITCH_LATENCY_COMMIT_ID 40002

/* Histogram bins: 0.5 ms wide to 32 ms, then 2 ms wide to 288 ms; the last bin also holds everything longer. */
#define SWITCH_LATENCY_FINE_US     500
#define SWITCH_LATENCY_FINE_BINS   64
#define SWITCH_LATENCY_COARSE_US   2000
#define SWITCH_LATENCY_COARSE_BINS 128
#define SWITCH_LATENCY_BINS        (SWITCH_LATENCY_FINE_BINS + SWITCH_LATENCY_COARSE_BINS)

/* What changed in a touch report: the bits switch_frames_touch takes. */
#define SWITCH_LATENCY_BEGAN_BIT 1u
#define SWITCH_LATENCY_ENDED_BIT 2u

enum {
    SWITCH_LATENCY_DELIVER,
    SWITCH_LATENCY_APP,
    SWITCH_LATENCY_WAIT,
    SWITCH_LATENCY_COMPOSE,
    SWITCH_LATENCY_SHOW,
    SWITCH_LATENCY_TOTAL,
    SWITCH_LATENCY_STAGES
};

/* A sample's kind: a contact went down in one of its reports, else one lifted, else contacts only moved. */
enum {
    SWITCH_LATENCY_BEGAN,
    SWITCH_LATENCY_ENDED,
    SWITCH_LATENCY_MOVED,
    SWITCH_LATENCY_KINDS
};

enum {
    SWITCH_LATENCY_IDLE,
    SWITCH_LATENCY_DELIVERED,
    SWITCH_LATENCY_COMMITTED
};

struct switch_latency_hist {
    uint32_t n;
    uint32_t max_us;
    uint16_t bin[SWITCH_LATENCY_BINS];
};

struct switch_latency {
    uint8_t changes; /* SWITCH_LATENCY_*_BIT of the reports since the last delivery */
    uint8_t stage;
    uint8_t kind;
    int32_t pid[2]; /* the processes the sample was delivered to; -1: none */
    uint64_t report_us, delivered_us, committed_us;
    uint32_t dropped[SWITCH_LATENCY_KINDS];
    struct switch_latency_hist hist[SWITCH_LATENCY_KINDS][SWITCH_LATENCY_STAGES];
};

static inline unsigned int switch_latency_bin(uint64_t us)
{
    const uint64_t fine = (uint64_t)SWITCH_LATENCY_FINE_US * SWITCH_LATENCY_FINE_BINS;

    if (us < fine) {
        return (unsigned int)(us / SWITCH_LATENCY_FINE_US);
    }
    const uint64_t coarse = (us - fine) / SWITCH_LATENCY_COARSE_US;
    return coarse < SWITCH_LATENCY_COARSE_BINS ? SWITCH_LATENCY_FINE_BINS + (unsigned int)coarse
                                               : SWITCH_LATENCY_BINS - 1;
}

static inline uint64_t switch_latency_bin_top(unsigned int b)
{
    if (b < SWITCH_LATENCY_FINE_BINS) {
        return (uint64_t)(b + 1) * SWITCH_LATENCY_FINE_US;
    }
    return (uint64_t)SWITCH_LATENCY_FINE_US * SWITCH_LATENCY_FINE_BINS +
           (uint64_t)(b - SWITCH_LATENCY_FINE_BINS + 1) * SWITCH_LATENCY_COARSE_US;
}

static inline void switch_latency_add(struct switch_latency_hist *h, uint64_t us)
{
    const unsigned int b = switch_latency_bin(us);

    if (h->bin[b] != UINT16_MAX) {
        h->bin[b]++;
    }
    h->n++;
    if (us > h->max_us) {
        h->max_us = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
    }
}

/* The top of the bin holding `pct` percent of the values, never above the largest. */
static inline uint64_t switch_latency_percentile(const struct switch_latency_hist *h, unsigned int pct)
{
    const uint64_t need = ((uint64_t)h->n * pct + 99) / 100;
    uint64_t seen = 0;

    if (h->n == 0) {
        return 0;
    }
    for (unsigned int b = 0; b < SWITCH_LATENCY_BINS; b++) {
        seen += h->bin[b];
        if (seen >= need) {
            const uint64_t top = switch_latency_bin_top(b);
            return top < h->max_us ? top : h->max_us;
        }
    }
    return h->max_us;
}

static inline void switch_latency_drop_stale(struct switch_latency *l, uint64_t now_us)
{
    const uint64_t since = l->stage == SWITCH_LATENCY_DELIVERED ? l->delivered_us : l->committed_us;

    if (l->stage != SWITCH_LATENCY_IDLE && now_us > since + SWITCH_LATENCY_STALE_US) {
        l->dropped[l->kind]++;
        l->stage = SWITCH_LATENCY_IDLE;
    }
}

static inline void switch_latency_report(struct switch_latency *l, unsigned int changes)
{
    l->changes |= (uint8_t)changes;
}

/* backboardd delivered the reports since the last delivery, the oldest from report_us (0: none pending), to pid. */
static inline void switch_latency_delivered(struct switch_latency *l, uint64_t report_us, int32_t pid, uint64_t now_us)
{
    const uint8_t changes = l->changes;

    l->changes = 0;
    switch_latency_drop_stale(l, now_us);
    if (l->stage == SWITCH_LATENCY_DELIVERED && pid != l->pid[0] && l->pid[1] < 0) {
        l->pid[1] = pid;
    }
    if (l->stage != SWITCH_LATENCY_IDLE || report_us == 0 || report_us > now_us) {
        return;
    }
    l->stage = SWITCH_LATENCY_DELIVERED;
    l->kind = (changes & SWITCH_LATENCY_BEGAN_BIT)   ? SWITCH_LATENCY_BEGAN
              : (changes & SWITCH_LATENCY_ENDED_BIT) ? SWITCH_LATENCY_ENDED
                                                     : SWITCH_LATENCY_MOVED;
    l->pid[0] = pid;
    l->pid[1] = -1;
    l->report_us = report_us;
    l->delivered_us = now_us;
}

/* A message with id `id` from pid to the render server: a commit answers a delivered sample. */
static inline void switch_latency_message(struct switch_latency *l, int32_t pid, int32_t id, uint64_t now_us)
{
    switch_latency_drop_stale(l, now_us);
    if (id == SWITCH_LATENCY_COMMIT_ID && l->stage == SWITCH_LATENCY_DELIVERED &&
        (pid == l->pid[0] || pid == l->pid[1]) && now_us >= l->delivered_us) {
        l->stage = SWITCH_LATENCY_COMMITTED;
        l->committed_us = now_us;
    }
}

/* A swap the render server submitted at submit_us after its vsync at vsync_us (0: not known) went to the panel at
 * latch_us. It answers a committed sample if it was drawn for a vsync after the commit. */
static inline void switch_latency_present(struct switch_latency *l, uint64_t vsync_us, uint64_t submit_us,
                                          uint64_t latch_us)
{
    switch_latency_drop_stale(l, latch_us);
    if (l->stage != SWITCH_LATENCY_COMMITTED) {
        return;
    }
    const uint64_t vsync = vsync_us != 0 && vsync_us <= submit_us ? vsync_us : submit_us;
    if (vsync < l->committed_us || latch_us < submit_us) {
        return;
    }
    const uint64_t at[SWITCH_LATENCY_STAGES] = {
        l->delivered_us - l->report_us, l->committed_us - l->delivered_us, vsync - l->committed_us, submit_us - vsync,
        latch_us - submit_us,           latch_us - l->report_us,
    };
    for (unsigned int s = 0; s < SWITCH_LATENCY_STAGES; s++) {
        switch_latency_add(&l->hist[l->kind][s], at[s]);
    }
    l->stage = SWITCH_LATENCY_IDLE;
}

/* A new window: the histograms start over, a sample in flight carries on. */
static inline void switch_latency_restart(struct switch_latency *l)
{
    for (unsigned int k = 0; k < SWITCH_LATENCY_KINDS; k++) {
        l->dropped[k] = 0;
    }
    memset(l->hist, 0, sizeof(l->hist));
}

/* Per kind with samples: "began 12 (dropped 1): total 41.5/58.0/63.5 ms = deliver 1.0/1.5/2.0 + app ... + show ...",
 * each stage p50/p95/max in ms. A summary of the histograms, so it is small enough to copy out from under a lock. */
struct switch_latency_summary {
    uint32_t n[SWITCH_LATENCY_KINDS], dropped[SWITCH_LATENCY_KINDS];
    uint32_t us[SWITCH_LATENCY_KINDS][SWITCH_LATENCY_STAGES][3];
};

static inline void switch_latency_summarize(const struct switch_latency *l, struct switch_latency_summary *out)
{
    for (unsigned int k = 0; k < SWITCH_LATENCY_KINDS; k++) {
        out->n[k] = l->hist[k][SWITCH_LATENCY_TOTAL].n;
        out->dropped[k] = l->dropped[k];
        for (unsigned int s = 0; s < SWITCH_LATENCY_STAGES; s++) {
            const struct switch_latency_hist *h = &l->hist[k][s];
            out->us[k][s][0] = (uint32_t)switch_latency_percentile(h, 50);
            out->us[k][s][1] = (uint32_t)switch_latency_percentile(h, 95);
            out->us[k][s][2] = h->max_us;
        }
    }
}

static inline size_t switch_latency_format(const struct switch_latency_summary *m, char *out, size_t size)
{
    static const char *const kinds[SWITCH_LATENCY_KINDS] = {"began", "ended", "moved"};
    static const char *const stages[SWITCH_LATENCY_STAGES] = {"deliver", "app", "wait", "compose", "show", "total"};
    size_t at = 0;
    int n;
    bool timed = false;

    if (size == 0) {
        return 0;
    }
    out[0] = 0;
    for (unsigned int k = 0; k < SWITCH_LATENCY_KINDS; k++) {
        if (m->n[k] == 0 && m->dropped[k] == 0) {
            continue;
        }
        n = snprintf(out + at, size - at, "%s%s %u (dropped %u)", at ? "; " : "", kinds[k], m->n[k], m->dropped[k]);
        at = n < 0 ? at : (at + (size_t)n < size ? at + (size_t)n : size - 1);
        if (m->n[k] == 0) {
            continue;
        }
        timed = true;
        for (unsigned int i = 0; i < SWITCH_LATENCY_STAGES; i++) {
            const unsigned int s = (i + SWITCH_LATENCY_STAGES - 1) % SWITCH_LATENCY_STAGES; /* the total first */
            const uint32_t *v = m->us[k][s];
            n = snprintf(out + at, size - at, "%s%s %u.%u/%u.%u/%u.%u",
                         s == SWITCH_LATENCY_TOTAL ? ": "
                         : i == 1                  ? " = "
                                                   : " + ",
                         stages[s], v[0] / 1000, v[0] / 100 % 10, v[1] / 1000, v[1] / 100 % 10, v[2] / 1000,
                         v[2] / 100 % 10);
            at = n < 0 ? at : (at + (size_t)n < size ? at + (size_t)n : size - 1);
        }
    }
    if (timed && at + 1 < size) {
        n = snprintf(out + at, size - at, " ms p50/p95/max");
        at = n < 0 ? at : (at + (size_t)n < size ? at + (size_t)n : size - 1);
    }
    return at;
}

#endif /* SWITCH_DIAG_LATENCY_H */
