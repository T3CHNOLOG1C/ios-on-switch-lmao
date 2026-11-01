// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pitch-linear 32-bit landscape frame -> the Maxwell block-linear surface window A column-scans (NativeLegacyDisplay,
// boot-arg ioswitch-fast-swizzle=1): byte for byte the layout of drivers/NativeMobileDisplay/NativeScanout.h
// native_legacy_block_copy (GOBs of 64 bytes x 8 lines, BLOCK_HEIGHT(4) = 16 GOBs per block, blocks row-major), which keeps
// its bounds checks and remains the fallback.
//
// Why a second loop: kernel code is built with -mgeneral-regs-only -mstrict-align, so the reference loop's 8-byte
// __builtin_memcpy from a `const unsigned char *` cannot assume alignment and compiles to eight ldrb plus shifts and orrs
// per word (about 50 instructions per 16 bytes, most of its ~16 ms per 1280x720 frame on the A57). Here the source is read
// through uint64_t pointers, which needs its base and row stride 16-byte aligned (checked; Metal may pad the stride), so a
// 16-byte piece is one ldp and one stp. No NEON: -mgeneral-regs-only rules out the FP/SIMD registers in kernel C, and a
// pair of X registers moves 16 bytes per instruction like a Q register, which is enough for a loop bound by memory.
//
// Order: every GOB is written front to back (32 pieces of 16 bytes, 512 contiguous bytes, so the write-combined scanout
// sees whole 64-byte bursts), and the eight source lines of a GOB row are walked forward together, 64 bytes each per GOB.
// Piece p of a GOB (byte offset 16p) holds line y = 2 * ((p >> 2) & 3) + (p & 1) of the GOB, bytes x = 32 * (p >> 4) +
// 16 * ((p >> 1) & 1) to x + 15 (the reference's offset formula: bit 8 = x bit 5, bits 7:6 = y bits 2:1, bit 5 = x bit 4,
// bit 4 = y bit 0).
#pragma once
#include <stdint.h>

// destination: the block-linear surface, 512-byte aligned, gobLines / 16 rounded up block rows of blockRowBytes;
// source: gobLines * 8 lines of gobColumns * 64 bytes, `sourceStride` apart. Callers check every bound.
static inline bool tegra_block_swizzle_aligned(void *destination,const void *source,uint64_t sourceStride,
    uint32_t gobLines,uint32_t gobColumns,uint64_t blockRowBytes)
{
    if(((uintptr_t)source|sourceStride)&15 || ((uintptr_t)destination&511))return false;
    const uint64_t words=sourceStride/8;                  // a source line, in 8-byte words
    const uint64_t columnWords=512*16/8;                  // one block column (16 GOBs stacked), in 8-byte words
    for(uint32_t gobLine=0;gobLine<gobLines;++gobLine) {
        const uint64_t *l0=(const uint64_t *)source+(uint64_t)gobLine*8*words;
        const uint64_t *l1=l0+words,*l2=l1+words,*l3=l2+words,*l4=l3+words,*l5=l4+words,*l6=l5+words,*l7=l6+words;
        // Plain stores: the scanout is mapped write-combined (Normal non-cacheable), so pairs become 16-byte stp; nothing
        // reads it back, and the caller's dsb orders them before the controller can fetch.
        uint64_t *gob=(uint64_t *)destination+((uint64_t)(gobLine/16)*blockRowBytes+(uint64_t)(gobLine%16)*512)/8;
        for(uint32_t column=0;column<gobColumns;++column,gob+=columnWords,l0+=8,l1+=8,l2+=8,l3+=8,l4+=8,l5+=8,l6+=8,l7+=8) {
            // Four GOBs ahead on each line: the eight streams stay ahead of the hardware prefetcher's ramp-up.
            __builtin_prefetch(l0+32);__builtin_prefetch(l1+32);__builtin_prefetch(l2+32);__builtin_prefetch(l3+32);
            __builtin_prefetch(l4+32);__builtin_prefetch(l5+32);__builtin_prefetch(l6+32);__builtin_prefetch(l7+32);
            const uint64_t *pair[4][2]={{l0,l1},{l2,l3},{l4,l5},{l6,l7}};
            uint64_t *out=gob;
            for(unsigned half=0;half<2;++half)
                for(unsigned y=0;y<4;++y,out+=8) {
                    // All eight loads before the stores: the compiler cannot move a load above a store to the same type, so
                    // interleaved they would stay single ldr/str instead of ldp/stp.
                    const uint64_t *even=pair[y][0]+half*4,*odd=pair[y][1]+half*4;
                    const uint64_t e0=even[0],e1=even[1],e2=even[2],e3=even[3],o0=odd[0],o1=odd[1],o2=odd[2],o3=odd[3];
                    out[0]=e0;out[1]=e1;   // x 0-15 of the half, line 2y
                    out[2]=o0;out[3]=o1;   // x 0-15, line 2y + 1
                    out[4]=e2;out[5]=e3;   // x 16-31, line 2y
                    out[6]=o2;out[7]=o3;   // x 16-31, line 2y + 1
                }
        }
    }
    return true;
}
