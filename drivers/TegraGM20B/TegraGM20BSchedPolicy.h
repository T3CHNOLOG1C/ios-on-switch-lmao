/* SPDX-License-Identifier: MIT */
/* The compositor priority's decisions (sched.c), apart from the registers and nvkm's lists, so a host test runs them. */
#ifndef TEGRA_GM20B_SCHED_POLICY_H
#define TEGRA_GM20B_SCHED_POLICY_H
#include <stdint.h>
#include "TegraGM20BHost.h"

#define TGM_VICTIM_NONE (-1)
#define TGM_VICTIM_SWITCHING (-2)

/* What a high channel's kick should preempt, from engine 0's (GR's) status, whether its resident channel is high, and the
 * PBDMA's: the app channel holding GR, else the one holding the PBDMA; TGM_VICTIM_SWITCHING while GR is mid-switch (the
 * host already leaves a channel, and with the runlist interleaved goes to a high one next). A TSG id names no channel. */
static inline int tgm_kick_victim(uint32_t eng, int eng_high, int pbdma_busy, uint32_t pbdma_chid,
				  int pbdma_high)
{
	if (eng & PFIFO_ENG_BUSY) {
		if (eng & PFIFO_ENG_CHSW)
			return TGM_VICTIM_SWITCHING;
		if (PFIFO_ENG_CTX_STATUS(eng) == PFIFO_ENG_CTX_VALID && !(eng & PFIFO_ENG_ID_TSG) &&
		    !eng_high)
			return (int)PFIFO_ENG_ID(eng);
	}
	if (pbdma_busy && !pbdma_high)
		return (int)PBDMA_CHID(pbdma_chid);
	return TGM_VICTIM_NONE;
}

/* The runlist's order of units (a channel, or a channel group with its header): every high unit after each low one, so
 * whichever low unit the host leaves, the high ones come next; with no low unit (or no high one) each unit once, in order.
 * entries[i] is unit i's runlist entries; when the interleave would pass max entries, the high units follow every
 * `every`-th low unit only. Writes unit indices to order (room for order_max) and returns how many. */
static inline unsigned tgm_interleave(const uint8_t *high, const uint16_t *entries, unsigned units,
				      unsigned max, uint16_t *order, unsigned order_max)
{
	unsigned base = 0, lows = 0, highs = 0, every = 1, n = 0, seen = 0, i, j;

	for (i = 0; i < units; i++) {
		base += entries[i];
		if (high[i])
			highs += entries[i];
		else
			lows++;
	}
	if (!lows || !highs)
		highs = 0;
	while (highs && every <= lows && base + lows / every * highs > max)
		every++;
	if (every > lows)
		highs = 0;
	for (i = 0; i < units && n < order_max; i++) {
		if (highs && high[i])
			continue;
		order[n++] = (uint16_t)i;
		if (!highs || ++seen % every)
			continue;
		for (j = 0; j < units && n < order_max; j++) {
			if (high[j])
				order[n++] = (uint16_t)j;
		}
	}
	return n;
}

#endif
