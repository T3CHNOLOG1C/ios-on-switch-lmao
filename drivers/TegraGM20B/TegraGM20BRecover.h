/* SPDX-License-Identifier: MIT */
/* What the GPU is stuck on (recover.c), for gpu.c's client fault isolation. */
#pragma once
#include <stdint.h>

struct nvkm_device;

/* PFIFO's engine context-switch timeout, after every FIFO init (bring-up and each wake): an engine that does not save or
 * load a context within the period raises SCHED_ERROR CTXSW_TIMEOUT, and nvkm recovers the channel it was on. */
void tgm_recover_arm(struct nvkm_device *device);
/* The channels the GPU is on, most likely culprit first: each busy engine's (mid-switch, the context it saves or loads,
 * as nvkm's own recovery picks it), then each busy PBDMA's. Returns how many, at most max. */
int tgm_recover_suspects(struct nvkm_device *device, int *chids, int max);

/* What a wait that ran out with no GPU error reported found (gpu.c hang_recover). */
enum tgm_hang {
	TGM_HANG_STUCK,      /* nothing to recover: the GPU is stuck on a kernel channel, or nothing progresses: panic */
	TGM_HANG_RECOVERING, /* a client's channel the GPU is stuck on is being killed (or the late one died already) */
	TGM_HANG_PROGRESSING /* no channel is stuck: the late work is slow, not hung */
};

struct tgm_hang_candidate {
	uint8_t live;   /* a channel of the kernel's list */
	uint8_t stuck;  /* work pending and nothing completed for the stuck time (gpu.c TGM_STUCK_NS) */
	uint8_t kernel; /* the present's or the scrubber's */
};

/* Candidates in order: tgm_recover_suspects' channels, the channel of its task the late one waits on, the late one. The first
 * stuck one is to blame (*blame, its index; else -1): a client's is killed, a kernel channel's is fatal. With none stuck, a
 * dead late channel is recovered already, and late kernel work with nothing progressing anywhere is fatal. */
static inline enum tgm_hang tgm_hang_blame(const struct tgm_hang_candidate *c, int n, int late_dead, int late_kernel,
					   int any_progress, int *blame)
{
	int i;

	*blame = -1;
	for (i = 0; i < n; i++) {
		if (!c[i].live || !c[i].stuck)
			continue;
		if (c[i].kernel)
			return TGM_HANG_STUCK;
		*blame = i;
		return TGM_HANG_RECOVERING;
	}
	if (late_dead)
		return TGM_HANG_RECOVERING;
	if (late_kernel && !any_progress)
		return TGM_HANG_STUCK;
	return TGM_HANG_PROGRESSING;
}
