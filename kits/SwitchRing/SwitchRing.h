// SPDX-License-Identifier: BSD-2-Clause-Patent
// Ring index arithmetic for the two shapes our rings use. Free-running: head and tail count up forever (uint32_t
// wraps cleanly), the ring size is a power of two and the slot is the index masked. Wrapped: indices stay below a
// size fixed at run time (firmware-chosen ring lengths), stepped with a compare instead of a division.
#ifndef SWITCH_RING_H
#define SWITCH_RING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint32_t switch_ring_used(uint32_t head, uint32_t tail)
{
    return head - tail;
}

static inline uint32_t switch_ring_space(uint32_t head, uint32_t tail, uint32_t pow2)
{
    return pow2 - (head - tail);
}

static inline uint32_t switch_ring_slot(uint32_t index, uint32_t pow2)
{
    return index & (pow2 - 1);
}

static inline uint32_t switch_ring_next(uint32_t index, uint32_t size)
{
    return index + 1 == size ? 0 : index + 1;
}

static inline uint32_t switch_ring_advance(uint32_t index, uint32_t count, uint32_t size)
{
    const uint32_t next = index + count;
    return next >= size ? next - size : next;
}

// Items from `read` up to `write` on a wrapped ring.
static inline uint32_t switch_ring_pending(uint32_t read, uint32_t write, uint32_t size)
{
    return write >= read ? write - read : size - read + write;
}

#ifdef __cplusplus
}
#endif

#endif
