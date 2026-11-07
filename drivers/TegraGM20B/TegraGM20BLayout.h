// SPDX-License-Identifier: MIT
// The GM20B's block-linear layout of a one-level 2D colour image, which every GPU user of this tree takes from here: the raw
// GM20B path's textures and mip chains (raw/fx), the kernel's lock-time detile and the display's tiled scan
// (TegraScanoutCopy.h). It is Mesa NIL's layout for that image (nil/image.rs Image::new_tiled, nil/tiling.rs Tiling::choose
// and clamp, GOBType::TegraColor, Image::nvc0_choose_pte_kind), so NVK's VK_IMAGE_TILING_OPTIMAL image of the same size and
// texel size lies at the same bytes under the same PTE kind, and NVK's view of the memory is free (nvk_image_plane_bind
// reuses a VA whose kind is the plane's). drivers/TegraGM20BMetal/test/raw_layout_test.cpp holds it to NIL's answers.
//
// GOBs of 64 bytes x 8 rows; a block is one GOB wide and 2^y_log2 GOBs tall; blocks lie row by row. Inside a GOB the bytes
// are in the TegraColor arrangement (nil/copy.rs CopyGOBTegra), which is where kind TGM_LAYOUT_KIND memory holds them in
// DRAM: what the CPU sees (the raw path's device self-test) and what DC A scans.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// NV_MMU_PTE_KIND_GENERIC_16BX2: NIL's kind for uncompressed colour of 8 to 128 bits a texel. The 3D and 2D engines lay out
// block-linear memory only under it (a kind-0 mapping scrambles each GOB's interior).
#define TGM_LAYOUT_KIND 0xfeu
#define TGM_LAYOUT_MAX_EXTENT 16384u

struct tgm_layout {
	uint32_t width, height, bpp;
	uint32_t row_bytes;  // the row stride: width x bpp in whole GOBs
	uint32_t rows;       // height in whole blocks
	uint8_t y_log2;      // GOBs a block tall, log2
	uint64_t align;      // max(one block, 4096)
	uint64_t bytes;      // row_bytes x rows, in whole `align`s
};

// Texel sizes NIL tiles (others are pitch-linear: NV_MMU_PTE_KIND_PITCH)
static inline int tgm_layout_tiles_bpp(uint32_t bpp)
{
	return bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8 || bpp == 16;
}

static inline uint32_t tgm_layout_log2_ceil(uint32_t v)
{
	uint32_t l = 0;
	while ((1u << l) < v)
		l++;
	return l;
}

// The block height of a 2D image `height` texels tall: the proprietary driver's choice by height that NIL copies, clamped
// below twice the image's height in GOBs. The hardware clamps each mip level's block height the same way.
static inline uint8_t tgm_layout_y_log2(uint32_t height)
{
	const uint32_t y = height >= 86   ? 4
			   : height >= 43 ? 3
			   : height >= 22 ? 2
			   : height >= 11 ? 1
					  : 0;
	const uint32_t clamp = tgm_layout_log2_ceil((height + 7) / 8);
	return (uint8_t)(y < clamp ? y : clamp);
}

// 0: no tiled layout (an extent of 0 or past TGM_LAYOUT_MAX_EXTENT, a texel size NIL does not tile)
static inline int tgm_layout_tiled(uint32_t width, uint32_t height, uint32_t bpp,
				   struct tgm_layout *out)
{
	if (!width || !height || width > TGM_LAYOUT_MAX_EXTENT || height > TGM_LAYOUT_MAX_EXTENT ||
	    !tgm_layout_tiles_bpp(bpp))
		return 0;
	const uint8_t y = tgm_layout_y_log2(height);
	const uint32_t block_rows = 8u << y;
	const uint64_t block = 512ull << y;
	out->width = width;
	out->height = height;
	out->bpp = bpp;
	out->y_log2 = y;
	out->row_bytes = (width * bpp + 63) & ~63u;
	out->rows = (height + block_rows - 1) / block_rows * block_rows;
	out->align = block > 4096 ? block : 4096;
	out->bytes =
		((uint64_t)out->row_bytes * out->rows + out->align - 1) / out->align * out->align;
	return 1;
}

// Byte xB % 64 of row y % 8 inside a GOB (TegraColor)
static inline uint32_t tgm_layout_gob_offset(uint32_t xB, uint32_t y)
{
	return ((xB % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xB % 32) / 16) * 32 + (y % 2) * 16 +
	       (xB % 16);
}

// Byte xB of row y of an image `row_bytes` across with blocks 2^y_log2 GOBs tall, from its first byte
static inline uint64_t tgm_layout_offset_of(uint32_t row_bytes, uint8_t y_log2, uint32_t xB,
					    uint32_t y)
{
	const uint32_t block_rows = 8u << y_log2;
	const uint64_t block = (uint64_t)(y / block_rows) * (row_bytes / 64) + xB / 64;
	return block * (512ull << y_log2) + (uint64_t)((y % block_rows) / 8) * 512 +
	       tgm_layout_gob_offset(xB, y);
}

static inline uint64_t tgm_layout_offset(const struct tgm_layout *l, uint32_t xB, uint32_t y)
{
	return tgm_layout_offset_of(l->row_bytes, l->y_log2, xB, y);
}

// The CPU's copies of `w` x `h` texels at (x, y) between a tiled image (`tiled`, its first byte) and rows `pitch` bytes
// apart, one GOB line's 16-byte run at a time; the caller keeps the rectangle inside the image and handles the cache lines.
static inline void tgm_layout_write(const struct tgm_layout *l, uint8_t *tiled, uint32_t x,
				    uint32_t y, uint32_t w, uint32_t h, const uint8_t *src,
				    uint64_t pitch)
{
	const uint32_t first = x * l->bpp, end = (x + w) * l->bpp;
	for (uint32_t row = 0; row < h; row++)
		for (uint32_t xB = first; xB < end;) {
			const uint32_t stop = (xB & ~15u) + 16,
				       run = (stop < end ? stop : end) - xB;
			__builtin_memcpy(tiled + tgm_layout_offset(l, xB, y + row),
					 src + row * pitch + (xB - first), run);
			xB += run;
		}
}

static inline void tgm_layout_read(const struct tgm_layout *l, const uint8_t *tiled, uint32_t x,
				   uint32_t y, uint32_t w, uint32_t h, uint8_t *dst, uint64_t pitch)
{
	const uint32_t first = x * l->bpp, end = (x + w) * l->bpp;
	for (uint32_t row = 0; row < h; row++)
		for (uint32_t xB = first; xB < end;) {
			const uint32_t stop = (xB & ~15u) + 16,
				       run = (stop < end ? stop : end) - xB;
			__builtin_memcpy(dst + row * pitch + (xB - first),
					 tiled + tgm_layout_offset(l, xB, y + row), run);
			xB += run;
		}
}

#ifdef __cplusplus
}
#endif
