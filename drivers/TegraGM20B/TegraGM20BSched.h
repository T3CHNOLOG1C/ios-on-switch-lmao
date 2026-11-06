/* SPDX-License-Identifier: MIT */
/* Compositor priority on the host's runlists (sched.c). The compositor's channels and the GPU present's are high: the
 * runlist lists them after every other entry, and their kicks preempt the app channel the GPU is on. */
#pragma once

struct nvkm_device;

/* Once, after the FIFO exists (M2): every runlist is built interleaved from then on. */
void tgm_sched_init(struct nvkm_device *device);
/* Mark a channel high (rebuilds the runlist, so the host sees it at once) or forget it (before nvkm removes the channel,
 * whose own runlist update then drops it). */
void tgm_sched_high(struct nvkm_device *device, unsigned int chid, int high);

enum tgm_sched_kick {
	TGM_KICK_CLEAR,     /* the GPU was idle or on a high channel */
	TGM_KICK_PREEMPTED, /* an app channel held GR or the PBDMA: preempted */
	TGM_KICK_SWITCHING, /* the host was already switching (a timeslice expired or a preempt is in flight) */
	TGM_KICK_DEFERRED, /* an app channel held it, and nvkm held the runlist (a teardown or a recovery) */
};

/* A high channel kicked work: preempt the app channel holding GR or the PBDMA, if one does. Never waits. */
enum tgm_sched_kick tgm_sched_kicked(struct nvkm_device *device, unsigned int chid);
/* Debug kernels: this window's kicks by what they found, for the stats line. */
void tgm_sched_stats(char *buf, unsigned int len);
