// SPDX-License-Identifier: MIT
/* See TegraGM20BRecover.h. */
#include <core/device.h>
#include <engine/fifo.h>
#include <engine/fifo/runl.h>
#include <engine/fifo/runq.h>
#include "TegraGM20BHost.h"
#include "TegraGM20BRecover.h"

/* PFIFO_ENG_TIMEOUT (nvgpu hw_fifo_gm20b.h fifo_eng_timeout_r): nouveau leaves it as reset, and with it off a context
 * switch that never completes stalls every channel with no interrupt at all. Its period counts PTIMER microseconds, and
 * PTIMER runs at 19.2/31.25 of real time on this Tegra (gpu.c "stats"; nvgpu scales it the same way, scale_ptimer). A
 * graphics context saves at a wait-for-idle, so the period also bounds the draw in flight when the host asks for a
 * switch: 250 ms is ten times the longest single draw seen (Maps' 4x MSAA passes, 13-19 ms). */
#define ENG_TIMEOUT 0x002a0c
#define ENG_TIMEOUT_DETECTION 0x80000000
#define CTXSW_TIMEOUT_US 250000
#define PTIMER_US(us) ((us) * 1920 / 3125)

void tgm_recover_arm(struct nvkm_device *device)
{
	nvkm_wr32(device, ENG_TIMEOUT, ENG_TIMEOUT_DETECTION | PTIMER_US(CTXSW_TIMEOUT_US));
}

static int add(int *chids, int n, int max, int chid)
{
	int i;

	for (i = 0; i < n; i++) {
		if (chids[i] == chid)
			return n;
	}
	if (n < max)
		chids[n++] = chid;
	return n;
}

int tgm_recover_suspects(struct nvkm_device *device, int *chids, int max)
{
	struct nvkm_runl *runl;
	struct nvkm_engn *engn;
	bool cgid;
	int n = 0, id, i;

	nvkm_runl_foreach(runl, device->fifo)
	{
		nvkm_runl_foreach_engn_cond(engn, runl, engn->func->cxid)
		{
			if (!(nvkm_rd32(device, PFIFO_ENG_STATUS(engn->id)) & PFIFO_ENG_BUSY))
				continue;
			id = engn->func->cxid(engn, &cgid);
			if (id >= 0 && !cgid)
				n = add(chids, n, max, id);
		}
	}
	nvkm_runl_foreach(runl, device->fifo)
	{
		for (i = 0; i < runl->runq_nr; i++) {
			if (!runl->runq[i] ||
			    !(nvkm_rd32(device, PBDMA_STATUS(runl->runq[i]->id)) & PBDMA_BUSY))
				continue;
			n = add(chids, n, max,
				PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runl->runq[i]->id))));
		}
	}
	return n;
}
