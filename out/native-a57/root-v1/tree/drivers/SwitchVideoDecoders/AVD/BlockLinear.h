// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVDEC's output layout to pitch linear, on the CPU. NVDEC writes NV12 block linear: 16Bx2 sectors in 64Bx8 GOBs,
// two GOBs stacked per block (GOB height 2, 16 rows), blocks left to right, one block row every 16 rows
// (averne's nvtegra_cpu_copy_plane; yuzu/Ryujinx swizzle tables). Within a GOB the byte at (x, y) sits at
//   ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16.
// Both rows of a sector share a 64-byte cache line, so rows are copied in pairs.
#ifndef BLOCK_LINEAR_H
#define BLOCK_LINEAR_H
#include <stddef.h>
#include <stdint.h>

// Copies the width x height window at (x0, y0) of a block-linear plane (srcPitch bytes per row, a multiple of 64)
// to dst. x0 and width are in bytes (for the NV12 chroma plane: luma pixels).
void block_linear_copy(uint8_t *dst, size_t dstPitch, const uint8_t *src, uint32_t srcPitch, uint32_t x0, uint32_t y0,
                       uint32_t width, uint32_t height);

#endif
