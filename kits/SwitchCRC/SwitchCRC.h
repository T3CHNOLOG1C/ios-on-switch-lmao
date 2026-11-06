// SPDX-License-Identifier: BSD-2-Clause-Patent
// CRC-32 (IEEE 802.3, reflected 0xEDB88320), chained like zlib's crc32(): start from 0, pass the result back in.
// The ARMv8 CRC32 instructions take 8 bytes each and use general registers only, so this is legal under
// -mgeneral-regs-only; target("crc") enables them where the recipe's -march leaves them off.
#ifndef SWITCH_CRC_H
#define SWITCH_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

__attribute__((target("crc"))) static inline uint32_t switch_crc32(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (; length && ((uintptr_t)p & 7); --length)
        crc = __builtin_arm_crc32b(crc, *p++);
    if (!length)
        return ~crc;
    // Aligned from here, so each word is one LDR even under -mstrict-align.
    const uint8_t *aligned = (const uint8_t *)__builtin_assume_aligned(p, 8);
    for (; length >= 8; length -= 8, aligned += 8) {
        uint64_t word = 0;
        __builtin_memcpy(&word, aligned, 8);
        crc = __builtin_arm_crc32d(crc, word);
    }
    if (length & 4) {
        uint32_t word = 0;
        __builtin_memcpy(&word, __builtin_assume_aligned(aligned, 8), 4);
        crc = __builtin_arm_crc32w(crc, word);
        aligned += 4;
    }
    if (length & 2) {
        uint16_t half = 0;
        __builtin_memcpy(&half, __builtin_assume_aligned(aligned, 4), 2);
        crc = __builtin_arm_crc32h(crc, half);
        aligned += 2;
    }
    if (length & 1)
        crc = __builtin_arm_crc32b(crc, *aligned);
    return ~crc;
}

#ifdef __cplusplus
}
#endif

#endif
