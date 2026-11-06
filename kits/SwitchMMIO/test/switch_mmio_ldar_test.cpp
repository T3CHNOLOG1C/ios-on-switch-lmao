// The real accessors (no model) on host memory: the asm and its constraints, including a zero store.
#include "kits/SwitchMMIO/SwitchMMIO.h"

#include <gtest/gtest.h>

TEST(SwitchMMIOAsm, LoadAcquireAndStore)
{
    alignas(8) uint32_t registers[4] = {1, 2, 3, 4};
    auto *base = reinterpret_cast<volatile uint8_t *>(registers);
    EXPECT_EQ(switch_mmio_read32(base, 8), 3u);
    switch_mmio_write32(base, 4, 0);
    switch_mmio_write32_publish(base, 12, 0x55aa55aa);
    switch_mmio_rmw32(base, 0, 1, 0x80);
    EXPECT_EQ(registers[0], 0x80u);
    EXPECT_EQ(registers[1], 0u);
    EXPECT_EQ(registers[3], 0x55aa55aau);
}

TEST(SwitchMMIOAsm, NarrowAccesses)
{
    alignas(8) uint8_t block[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    auto *base = reinterpret_cast<volatile uint8_t *>(block);
    EXPECT_EQ(switch_mmio_read16(base, 2), 0x4433u);
    EXPECT_EQ(switch_mmio_read8(base, 7), 0x88u);
    switch_mmio_write16(base, 4, 0xbeef);
    EXPECT_EQ(block[4], 0xefu);
    EXPECT_EQ(block[5], 0xbeu);
}
