// SPDX-License-Identifier: MIT
// Block-linear IOSurfaces (TegraGM20BTiled.cpp): each surface's layout, decided once on its coherency entry
// (TegraGM20BCoherency.cpp) and read by every GPU importer, the display and a CPU lock. The entry's side of it is a few
// accessors; the entry holds a `tgm_tiled` once decided and frees it with itself.
#pragma once
#include <stdint.h>
#include "TegraGM20BGpu.h"
#include "TegraGM20BLayout.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tgm_tiled {
	int tiled; /* else pitch, for good */
	struct tgm_layout layout;
	uint32_t bytes_per_row;
};

/* TegraGM20BCoherency.cpp: the entry's lock (held across a decision and a move), its layout slot (entry lock), whether
 * it is its surface's only import (refs 1), its page list (entry lock), and the GPU writers a move of it must follow
 * (still running; after a writer lost its slot, every channel's submitted work is waited for here and none is returned) */
void tgm_coh_lock(struct tgm_coh *c);
void tgm_coh_unlock(struct tgm_coh *c);
struct tgm_tiled **tgm_coh_tiled(struct tgm_coh *c);
int tgm_coh_alone(struct tgm_coh *c);
struct tgm_relayout_pages tgm_coh_pages(const struct tgm_coh *c);
uint32_t tgm_coh_writers(struct tgm_coh *c, struct tgm_relayout_writer *out, uint32_t max);
uint32_t tgm_coh_surface_id(const struct tgm_coh *c);

/* TegraGM20BTiled.cpp: the scratch of rows a move goes through (the GPU pool's pages; null: none to be had) */
struct tgm_scratch;
struct tgm_scratch *tgm_scratch_get(uint64_t bytes, struct tgm_relayout_pages *pages);
void tgm_scratch_put(struct tgm_scratch *scratch);
void tgm_tiled_free(struct tgm_tiled *t);
/* Its last GPU import is going: a block-linear surface's texels go back to rows in place (behind its GPU writers) and it
 * is pitch from then on. Takes the entry lock. */
void tgm_tiled_untile(struct tgm_coh *c);
/* The decisions so far (debug kernels): tiled, then pitch because the first import only samples, because another import
 * existed, because the shape does not tile, because the tiling move failed; and the live tiled surfaces and their bytes */
void tgm_tiled_take_stats(uint64_t out[7]);

#ifdef __cplusplus
}
#endif
