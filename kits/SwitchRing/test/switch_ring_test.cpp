// kits/SwitchRing against modulo arithmetic, across the uint32_t wrap of free-running indices.
#include "kits/SwitchRing/SwitchRing.h"

#include <gtest/gtest.h>

namespace {
TEST(SwitchRing, FreeRunningAcrossTheWrap)
{
    const uint32_t tail = UINT32_MAX - 2, head = tail + 5;
    EXPECT_EQ(switch_ring_used(head, tail), 5u);
    EXPECT_EQ(switch_ring_space(head, tail, 64), 59u);
    EXPECT_EQ(switch_ring_slot(head, 64), head % 64);
}

TEST(SwitchRing, WrappedMatchesModulo)
{
    for (uint32_t size = 1; size < 70; ++size)
        for (uint32_t i = 0; i < size; ++i) {
            ASSERT_EQ(switch_ring_next(i, size), (i + 1) % size);
            for (uint32_t n = 0; n <= size; ++n)
                ASSERT_EQ(switch_ring_advance(i, n, size), (i + n) % size);
            for (uint32_t w = 0; w < size; ++w)
                ASSERT_EQ(switch_ring_pending(i, w, size), (w + size - i) % size);
        }
}
}  // namespace
