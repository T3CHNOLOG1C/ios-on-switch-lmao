// SPDX-License-Identifier: MIT
/*
 * Compositor priority (TegraGM20BSched.h).
 * Maxwell preempts graphics only at a wait-for-idle, so a draw in flight always finishes; the host decides which channel
 * runs next and when it asks the running one to stop. nvkm builds a plain round robin (fifo/nv50.c nv50_runl_update:
 * "TODO: prio, interleaving"), so a compositor kick waited for every app with work to use up its timeslice. Here, as
 * nvgpu's interleave levels (gk20a_runlist_construct_locked: a higher level's entries repeat before each lower one), every
 * high channel follows each other entry of the runlist, so whichever app the host leaves, the compositor is next; and a
 * high channel's kick preempts the app channel GR or the PBDMA is on (PFIFO_PREEMPT, as gf100_chan_preempt), so the host
 * leaves it at its next wait-for-idle instead of at the end of its timeslice. Nothing waits for a preempt: a context that
 * never saves trips the engine's context-switch timeout, which recovers its channel (TegraGM20BRecover.h).
 */
#include <core/device.h>
#include <core/memory.h>
#include <engine/fifo.h>
#include <engine/fifo/runl.h>
#include <engine/fifo/runq.h>
#include <engine/fifo/cgrp.h>
#include <engine/fifo/chan.h>
#include <engine/fifo/chid.h>
#include <engine/fifo/priv.h> /* nv50_runl_update, nvkm_fifo_func */
#include <subdev/timer.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BHost.h"
#include "TegraGM20BSched.h"
#include "TegraGM20BSchedPolicy.h"

#define SCHED_CHIDS 4096
#define SCHED_RUNLS 4

static DECLARE_BITMAP(high, SCHED_CHIDS);

/* nvkm's FIFO and channel functions, the channel preempt wrapped (sched_chan_preempt) */
static struct nvkm_fifo_func fifo_func;
static struct nvkm_chan_func chan_func;
static void (*orig_chan_preempt)(struct nvkm_chan *);

/* a runlist's units (a channel group: its header, if it has one, and its channels) and the order sched_update lists them in */
#define SCHED_UNITS 512
#define SCHED_ORDER 2048

static struct {
	struct nvkm_runl_func func; /* nvkm's, with sched_update */
	struct nvkm_memory *mem;
	u32 segment; /* bytes: two segments, one written while the host may still read the other */
	int next;
	struct nvkm_cgrp *unit[SCHED_UNITS];
	u8 high[SCHED_UNITS];
	u16 entries[SCHED_UNITS], order[SCHED_ORDER];
} rl[SCHED_RUNLS];

static struct {
	u32 kicks, preempted, switching, deferred;
} st;

static int is_high(int chid)
{
	return chid >= 0 && chid < SCHED_CHIDS && test_bit(chid, high);
}

static int cgrp_high(struct nvkm_cgrp *cgrp)
{
	struct nvkm_chan *chan;

	nvkm_cgrp_foreach_chan(chan, cgrp)
	{
		if (is_high(chan->id))
			return 1;
	}
	return 0;
}

/* runl->func->update, under runl->mutex as nvkm calls it. The wait for the host's last update comes first: it may run
 * nvkm's recovery (nvkm_runl_update_pending), which removes the channels it kills and updates the runlist itself, so the
 * groups listed after it are the ones left, and the segment written last time is free. */
static int sched_update(struct nvkm_runl *runl)
{
	const u32 size = runl->func->size;
	struct nvkm_memory *mem = rl[runl->id].mem;
	struct nvkm_cgrp **unit = rl[runl->id].unit;
	struct nvkm_cgrp *cgrp;
	struct nvkm_chan *chan;
	u32 units = 0, n, i, start, offset;

	if (runl->func->pending(runl))
		runl->func->wait(runl);
	if (runl->cgrp_nr > SCHED_UNITS)
		return nv50_runl_update(runl);
	nvkm_runl_foreach_cgrp(cgrp, runl)
	{
		if (units == SCHED_UNITS)
			break;
		unit[units] = cgrp;
		rl[runl->id].high[units] = cgrp_high(cgrp);
		rl[runl->id].entries[units++] = cgrp->hw + cgrp->chan_nr;
	}
	n = tgm_interleave(rl[runl->id].high, rl[runl->id].entries, units,
			   rl[runl->id].segment / size, rl[runl->id].order, SCHED_ORDER);
	start = rl[runl->id].next * rl[runl->id].segment;
	rl[runl->id].next ^= 1;
	offset = start;
	nvkm_kmap(mem);
	for (i = 0; i < n; i++) {
		cgrp = unit[rl[runl->id].order[i]];
		if (cgrp->hw) {
			runl->func->insert_cgrp(cgrp, mem, offset);
			offset += size;
		}
		nvkm_cgrp_foreach_chan(chan, cgrp)
		{
			runl->func->insert_chan(chan, mem, offset);
			offset += size;
		}
	}
	nvkm_done(mem);
	list_rotate_left(&runl->cgrps);
	runl->func->commit(runl, mem, start, (offset - start) / size);
	return 0;
}

/* nvkm writes PFIFO_PREEMPT and waits for its own preempt to finish; one written over a high kick's preempt still in
 * flight (tgm_sched_kicked does not wait for its own) is lost, and nvkm would take the channel for preempted. */
static void sched_chan_preempt(struct nvkm_chan *chan)
{
	struct nvkm_device *device = chan->cgrp->runl->fifo->engine.subdev.device;

	nvkm_wait_msec(device, chan->cgrp->runl->fifo->timeout.chan_msec, PFIFO_PREEMPT,
		       PFIFO_PREEMPT_PENDING, 0);
	orig_chan_preempt(chan);
}

void tgm_sched_init(struct nvkm_device *device)
{
	struct nvkm_fifo *fifo = device->fifo;
	struct nvkm_runl *runl;
	u32 entries;

	if (fifo->func->chan.func->preempt && fifo->func != &fifo_func) {
		chan_func = *fifo->func->chan.func;
		orig_chan_preempt = chan_func.preempt;
		chan_func.preempt = sched_chan_preempt;
		fifo_func = *fifo->func;
		fifo_func.chan.func = &chan_func;
		fifo->func = &fifo_func;
	}

	nvkm_runl_foreach(runl, device->fifo)
	{
		if (runl->id < 0 || runl->id >= SCHED_RUNLS || rl[runl->id].mem ||
		    runl->func->update != nv50_runl_update || !runl->func->pending)
			continue;
		/* nvkm's whole ring holds two of these: every entry once, and a high group after each low one */
		entries = 2 * ((runl->cgid ? runl->cgid->nr : 0) + runl->chid->nr);
		rl[runl->id].segment = ALIGN(entries * runl->func->size, 0x1000);
		if (nvkm_memory_new(device, NVKM_MEM_TARGET_INST, 2 * rl[runl->id].segment, 0x1000,
				    true, &rl[runl->id].mem)) {
			nvxnu_log("tegra-gm20b: runlist %d keeps nvkm's round robin: no memory\n",
				  runl->id);
			continue;
		}
		mutex_lock(&runl->mutex);
		rl[runl->id].func = *runl->func;
		rl[runl->id].func.update = sched_update;
		runl->func = &rl[runl->id].func;
		atomic_set(&runl->changed, 1);
		nvkm_runl_update_locked(runl, true);
		mutex_unlock(&runl->mutex);
	}
}

void tgm_sched_high(struct nvkm_device *device, unsigned int chid, int on)
{
	struct nvkm_runl *runl;

	if (chid >= SCHED_CHIDS)
		return;
	if (!on) {
		clear_bit(chid, high);
		return;
	}
	set_bit(chid, high);
	nvkm_runl_foreach(runl, device->fifo)
	{
		if (runl->func->update != sched_update)
			continue;
		mutex_lock(&runl->mutex);
		atomic_set(&runl->changed, 1);
		nvkm_runl_update_locked(runl, true);
		mutex_unlock(&runl->mutex);
	}
}

static enum tgm_sched_kick counted(enum tgm_sched_kick kick)
{
	if (!TGM_STATS)
		return kick;
	__atomic_fetch_add(&st.kicks, 1, __ATOMIC_RELAXED);
	if (kick == TGM_KICK_PREEMPTED)
		__atomic_fetch_add(&st.preempted, 1, __ATOMIC_RELAXED);
	else if (kick == TGM_KICK_SWITCHING)
		__atomic_fetch_add(&st.switching, 1, __ATOMIC_RELAXED);
	else if (kick == TGM_KICK_DEFERRED)
		__atomic_fetch_add(&st.deferred, 1, __ATOMIC_RELAXED);
	return kick;
}

/* The app channel GR or the PBDMA serving the runlist is on (TegraGM20BSchedPolicy.h) */
static int sched_victim(struct nvkm_device *device, struct nvkm_runl *runl)
{
	struct nvkm_engn *engn;
	u32 eng = 0, pbdma = 0, chid = 0;

	nvkm_runl_foreach_engn(engn, runl)
	{
		if (engn->engine->subdev.type == NVKM_ENGINE_GR) {
			eng = nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id));
			break;
		}
	}
	if (runl->runq_nr && runl->runq[0]) {
		pbdma = nvkm_rd32(device, PBDMA_STATUS(runl->runq[0]->id)) & PBDMA_BUSY;
		chid = PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runl->runq[0]->id)));
	}
	return tgm_kick_victim(eng, is_high(PFIFO_ENG_ID(eng)), pbdma != 0, chid, is_high(chid));
}

enum tgm_sched_kick tgm_sched_kicked(struct nvkm_device *device, unsigned int chid)
{
	struct nvkm_runl *runl;
	int victim;

	nvkm_runl_foreach(runl, device->fifo)
	{
		if (runl->func->update != sched_update || !is_high(chid) ||
		    (int)chid >= runl->chid->nr || !test_bit(chid, runl->chid->used))
			continue;
		victim = sched_victim(device, runl);
		if (victim == TGM_VICTIM_SWITCHING)
			return counted(TGM_KICK_SWITCHING);
		if (victim == TGM_VICTIM_NONE)
			return counted(TGM_KICK_CLEAR);
		/* nvkm writes PFIFO_PREEMPT under runl->mutex and waits for it there; one of its own is in flight */
		if (!mutex_trylock(&runl->mutex))
			return counted(TGM_KICK_DEFERRED);
		if (nvkm_rd32(device, PFIFO_PREEMPT) & PFIFO_PREEMPT_PENDING) {
			mutex_unlock(&runl->mutex);
			return counted(TGM_KICK_SWITCHING);
		}
		nvkm_wr32(device, PFIFO_PREEMPT, victim);
		mutex_unlock(&runl->mutex);
		return counted(TGM_KICK_PREEMPTED);
	}
	return TGM_KICK_CLEAR;
}

void tgm_sched_stats(char *buf, unsigned int len)
{
	u32 kicks = __atomic_exchange_n(&st.kicks, 0, __ATOMIC_RELAXED),
	    preempted = __atomic_exchange_n(&st.preempted, 0, __ATOMIC_RELAXED),
	    switching = __atomic_exchange_n(&st.switching, 0, __ATOMIC_RELAXED),
	    deferred = __atomic_exchange_n(&st.deferred, 0, __ATOMIC_RELAXED);

	scnprintf(buf, len,
		  "%u high kicks: %u preempted an app, %u found a switch under way, %u deferred",
		  kicks, preempted, switching, deferred);
}
