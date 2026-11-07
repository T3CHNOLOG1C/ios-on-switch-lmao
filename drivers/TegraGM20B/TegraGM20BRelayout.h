// SPDX-License-Identifier: MIT
// The copy engine's moves of a block-linear IOSurface (TegraGM20BLayout.h) between its own pages, which the GPU and the
// display read tiled, and a scratch of rows bytes_per_row apart (a CPU lock's view of it). One move is one push, which
// TegraGM20BRelayout.cpp submits on the kernel's copy channel: a semaphore acquire on each GPU writer it must follow (the
// host waits on the GPU, switching to other channels meanwhile), then its copies, each NON_PIPELINED (it starts once the
// one before wrote everything) with FLUSH_ENABLE:
//  - to rows: the surface's texels into the scratch;
//  - to tiled: the scratch's rows into the surface's texels;
//  - tile in place: what the surface holds as rows (its contents before it became block-linear) into the scratch, then
//    back into the surface as texels;
//  - untile in place: the surface's texels into the scratch as rows, then back into the surface as rows (it is pitch
//    again: no GPU client imports it any more).
// Plain C, so the host test runs the same words through a copy-engine model.
#pragma once
#include <stdint.h>
#include "TegraGM20BLayout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TGM_RELAYOUT_SUBC_COPY 4u
#define TGM_RELAYOUT_MAX_WAITS 8u
#define TGM_RELAYOUT_DWORDS (TGM_RELAYOUT_MAX_WAITS * 5u + 64u)

enum tgm_relayout_op {
	TGM_RELAYOUT_TO_ROWS,
	TGM_RELAYOUT_TO_TILED,
	TGM_RELAYOUT_TILE_IN_PLACE,
	TGM_RELAYOUT_UNTILE_IN_PLACE,
};

// A GPU writer's fence as the move's channel sees it: the GPU address of its 32-bit seqno and the seqno to wait for
struct tgm_relayout_wait {
	uint64_t va;
	uint32_t seqno;
};

// The surface's pages at surface_va (TGM_LAYOUT_KIND: its texels) and again at surface_rows_va (kind 0: what it held as
// rows before it was tiled), and the scratch at rows_va (kind 0), each from its first byte. Block-linear memory is read and
// written only through a TGM_LAYOUT_KIND mapping and rows only through a kind-0 one: each kind places a GOB's bytes in
// DRAM its own way.
struct tgm_relayout_move {
	uint64_t surface_va, surface_rows_va, rows_va;
	uint32_t bytes_per_row;
	enum tgm_relayout_op op;
};

static inline uint32_t tgm_relayout_header(uint32_t subc, uint32_t mthd, uint32_t count)
{
	return (1u << 29) | (count << 16) | (subc << 13) | (mthd >> 2);
}

// SEMAPHOREA..D, ACQ_GEQ with ACQUIRE_SWITCH (cl906f.h)
static inline uint32_t tgm_relayout_acquire(uint32_t *w, const struct tgm_relayout_wait *wait)
{
	w[0] = tgm_relayout_header(0, 0x0010, 4);
	w[1] = (uint32_t)(wait->va >> 32);
	w[2] = (uint32_t)wait->va;
	w[3] = wait->seqno;
	w[4] = 0x4u | 1u << 12;
	return 5;
}

// `src` to `dst`, the texels of `l` (a block-linear side with the layout's block height, its width the row stride in
// texels; a pitch side rows `pitch` apart) through SET_REMAP_COMPONENTS of the texel's components (at most 4 bytes each),
// so lengths count texels
static inline uint32_t tgm_relayout_copy(uint32_t *w, const struct tgm_layout *l, uint64_t src,
					 int src_tiled, uint64_t dst, int dst_tiled, uint32_t pitch)
{
	const uint32_t comp = l->bpp < 4 ? l->bpp : 4, n = l->bpp / comp;
	uint32_t launch = 2u | 1u << 2 | 1u << 9 |
			  1u << 10;  // NON_PIPELINED, FLUSH_ENABLE, MULTI_LINE, REMAP
	uint32_t i = 0;
	w[i++] = tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, 0x0708, 1);
	w[i++] =
		0 | 1u << 4 | 2u << 8 | 3u << 12 | (comp - 1) << 16 | (n - 1) << 20 | (n - 1) << 24;
	w[i++] = tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, 0x0400,
				     8);  // OFFSET_IN_UPPER .. LINE_COUNT
	w[i++] = (uint32_t)(src >> 32);
	w[i++] = (uint32_t)src;
	w[i++] = (uint32_t)(dst >> 32);
	w[i++] = (uint32_t)dst;
	w[i++] = src_tiled ? l->row_bytes : pitch;
	w[i++] = dst_tiled ? l->row_bytes : pitch;
	w[i++] = l->width;
	w[i++] = l->height;
	for (int side = 0; side < 2; side++) {
		if (!(side ? dst_tiled : src_tiled)) {
			launch |= side ? 1u << 8 : 1u << 7;  // DST or SRC_MEMORY_LAYOUT PITCH
			continue;
		}
		// SET_SRC_BLOCK_SIZE (0x728) or SET_DST_BLOCK_SIZE (0x70c), then WIDTH, HEIGHT, DEPTH, LAYER, ORIGIN
		w[i++] = tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, side ? 0x070c : 0x0728, 6);
		w[i++] = (uint32_t)l->y_log2 << 4 | 1u << 12;  // one GOB wide, Fermi's 8-row GOBs
		w[i++] = l->row_bytes / l->bpp;
		w[i++] = l->height;
		w[i++] = 1;
		w[i++] = 0;
		w[i++] = 0;
	}
	w[i++] = tgm_relayout_header(TGM_RELAYOUT_SUBC_COPY, 0x0300, 1);  // LAUNCH_DMA
	w[i++] = launch;
	return i;
}

// The whole push of one move, at most TGM_RELAYOUT_DWORDS words; `nwaits` at most TGM_RELAYOUT_MAX_WAITS
static inline uint32_t tgm_relayout_push(uint32_t *w, const struct tgm_layout *l,
					 const struct tgm_relayout_move *m,
					 const struct tgm_relayout_wait *waits, uint32_t nwaits)
{
	uint32_t i = 0;
	for (uint32_t k = 0; k < nwaits && k < TGM_RELAYOUT_MAX_WAITS; k++)
		i += tgm_relayout_acquire(w + i, &waits[k]);
	switch (m->op) {
	case TGM_RELAYOUT_TO_ROWS:
		return i + tgm_relayout_copy(w + i, l, m->surface_va, 1, m->rows_va, 0,
					     m->bytes_per_row);
	case TGM_RELAYOUT_TO_TILED:
		return i + tgm_relayout_copy(w + i, l, m->rows_va, 0, m->surface_va, 1,
					     m->bytes_per_row);
	case TGM_RELAYOUT_TILE_IN_PLACE:
		i += tgm_relayout_copy(w + i, l, m->surface_rows_va, 0, m->rows_va, 0,
				       m->bytes_per_row);
		return i + tgm_relayout_copy(w + i, l, m->rows_va, 0, m->surface_va, 1,
					     m->bytes_per_row);
	case TGM_RELAYOUT_UNTILE_IN_PLACE:
		i += tgm_relayout_copy(w + i, l, m->surface_va, 1, m->rows_va, 0, m->bytes_per_row);
		return i + tgm_relayout_copy(w + i, l, m->rows_va, 0, m->surface_rows_va, 0,
					     m->bytes_per_row);
	}
	return i;
}

#ifdef __cplusplus
}
#endif
