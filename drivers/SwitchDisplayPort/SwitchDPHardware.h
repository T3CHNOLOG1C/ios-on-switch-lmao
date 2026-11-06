// SPDX-License-Identifier: GPL-2.0-only
// Stand-in for kits/SwitchMMIO and kits/SwitchCAR (r26-common), with their signatures, until those land on main; then
// this file goes and the includes name the kits. Unlike the kit, nothing here serializes CAR or PMC against other
// drivers: SwitchDisplayPort only touches its own clock, reset and partition bits through the SET/CLR registers.
#ifndef SWITCH_DP_HARDWARE_H
#define SWITCH_DP_HARDWARE_H

#include <IOKit/IOLib.h>
#include <stddef.h>
#include <stdint.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define SWITCH_BIT(n) (1u << (n))

static inline volatile uint8_t *switch_mmio_map(uint64_t pa, size_t size)
{
    return reinterpret_cast<volatile uint8_t *>(ml_io_map((vm_offset_t)pa, size));
}

static inline uint32_t switch_mmio_read32(const volatile uint8_t *base, uint32_t offset)
{
    return *reinterpret_cast<const volatile uint32_t *>(base + offset);
}

static inline void switch_mmio_write32(volatile uint8_t *base, uint32_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
}

static inline void switch_mmio_rmw32(volatile uint8_t *base, uint32_t offset, uint32_t clear, uint32_t set)
{
    switch_mmio_write32(base, offset, (switch_mmio_read32(base, offset) & ~clear) | set);
}

static inline bool switch_mmio_wait32(const volatile uint8_t *base, uint32_t offset, uint32_t mask, uint32_t value,
                                      uint32_t timeout_us)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(timeout_us, kMicrosecondScale, &deadline);
    while (mach_absolute_time() < deadline) {
        if ((switch_mmio_read32(base, offset) & mask) == value) {
            return true;
        }
    }
    return (switch_mmio_read32(base, offset) & mask) == value;
}

namespace switchcar {
constexpr uint64_t kCarPA = 0x60006000, kPmcPA = 0x7000E400;
// Per bank L, H, U, V, W, X, Y: CLK_OUT_ENB, its SET and CLR, RST_DEVICES, its SET and CLR (Linux clk.c periph_regs).
constexpr uint32_t kBanks[7][6] = {
    {0x010, 0x320, 0x324, 0x004, 0x300, 0x304}, {0x014, 0x328, 0x32c, 0x008, 0x308, 0x30c},
    {0x018, 0x330, 0x334, 0x00c, 0x310, 0x314}, {0x360, 0x440, 0x444, 0x358, 0x430, 0x434},
    {0x364, 0x448, 0x44c, 0x35c, 0x438, 0x43c}, {0x280, 0x284, 0x288, 0x28c, 0x290, 0x294},
    {0x298, 0x29c, 0x2a0, 0x2a4, 0x2a8, 0x2ac},
};
enum : uint32_t { PWRGATE_TOGGLE = 0x30, REMOVE_CLAMPING = 0x34, PWRGATE_STATUS = 0x38, TOGGLE_START = 1u << 8 };
enum : uint32_t { IO_DPD_REQ = 0x1b8, IO_DPD_STATUS = 0x1bc, IO_DPD2_REQ = 0x1c0, IO_DPD2_STATUS = 0x1c4 };
constexpr uint32_t kDpdOff = 1u << 30, kDpdOn = 2u << 30;

inline volatile uint8_t *car()
{
    static volatile uint8_t *base;
    if (base == nullptr) {
        base = switch_mmio_map(kCarPA, 0x1000);
    }
    return base;
}

inline volatile uint8_t *pmc()
{
    static volatile uint8_t *base;
    if (base == nullptr) {
        base = switch_mmio_map(kPmcPA & ~0xFFFull, 0x1000) + (kPmcPA & 0xFFF);
    }
    return base;
}

inline const uint32_t *bank(unsigned id)
{
    return kBanks[id / 32];
}
}  // namespace switchcar

static inline bool switch_car_clock_enable(unsigned id)
{
    switch_mmio_write32(switchcar::car(), switchcar::bank(id)[1], SWITCH_BIT(id % 32));
    return (switch_mmio_read32(switchcar::car(), switchcar::bank(id)[0]) & SWITCH_BIT(id % 32)) != 0;
}

static inline void switch_car_clock_disable(unsigned id)
{
    switch_mmio_write32(switchcar::car(), switchcar::bank(id)[2], SWITCH_BIT(id % 32));
}

static inline bool switch_car_clock_enabled(unsigned id)
{
    return (switch_mmio_read32(switchcar::car(), switchcar::bank(id)[0]) & SWITCH_BIT(id % 32)) != 0;
}

static inline void switch_car_reset_assert(unsigned id)
{
    switch_mmio_write32(switchcar::car(), switchcar::bank(id)[4], SWITCH_BIT(id % 32));
}

static inline void switch_car_reset_deassert(unsigned id)
{
    switch_mmio_write32(switchcar::car(), switchcar::bank(id)[5], SWITCH_BIT(id % 32));
}

static inline void switch_car_write(uint32_t offset, uint32_t value)
{
    switch_mmio_write32(switchcar::car(), offset, value);
}

static inline uint32_t switch_car_read(uint32_t offset)
{
    return switch_mmio_read32(switchcar::car(), offset);
}

static inline void switch_car_rmw(uint32_t offset, uint32_t clear, uint32_t set)
{
    switch_mmio_rmw32(switchcar::car(), offset, clear, set);
}

static inline bool switch_pmc_partition_is_on(unsigned partition)
{
    return (switch_mmio_read32(switchcar::pmc(), switchcar::PWRGATE_STATUS) & SWITCH_BIT(partition)) != 0;
}

static inline bool switch_pmc_toggle(unsigned partition, bool on)
{
    if (switch_pmc_partition_is_on(partition) == on) {
        return true;
    }
    if (!switch_mmio_wait32(switchcar::pmc(), switchcar::PWRGATE_TOGGLE, switchcar::TOGGLE_START, 0, 5000)) {
        return false;
    }
    switch_mmio_write32(switchcar::pmc(), switchcar::PWRGATE_TOGGLE, partition | switchcar::TOGGLE_START);
    return switch_mmio_wait32(switchcar::pmc(), switchcar::PWRGATE_STATUS, SWITCH_BIT(partition),
                              on ? SWITCH_BIT(partition) : 0, 5000);
}

static inline bool switch_pmc_partition_on(unsigned partition)
{
    return switch_pmc_toggle(partition, true);
}

static inline bool switch_pmc_partition_off(unsigned partition)
{
    return switch_pmc_toggle(partition, false);
}

static inline void switch_pmc_remove_clamping(unsigned partition)
{
    switch_mmio_write32(switchcar::pmc(), switchcar::REMOVE_CLAMPING, SWITCH_BIT(partition));
}

static inline bool switch_pmc_power_up(unsigned partition, unsigned clock_id, unsigned reset_id)
{
    switch_car_reset_assert(reset_id);
    if (!switch_pmc_partition_on(partition)) {
        return false;
    }
    (void)switch_car_clock_enable(clock_id);
    IODelay(10);
    switch_pmc_remove_clamping(partition);
    IODelay(10);
    switch_car_reset_deassert(reset_id);
    return true;
}

static inline bool switch_pmc_io_pad(unsigned dpd, bool on)
{
    const bool second = dpd >= 32;
    const uint32_t bit = SWITCH_BIT(dpd % 32);
    switch_mmio_write32(switchcar::pmc(), second ? switchcar::IO_DPD2_REQ : switchcar::IO_DPD_REQ,
                        (on ? switchcar::kDpdOff : switchcar::kDpdOn) | bit);
    return switch_mmio_wait32(switchcar::pmc(), second ? switchcar::IO_DPD2_STATUS : switchcar::IO_DPD_STATUS, bit,
                              on ? 0 : bit, 1000);
}

static inline bool switch_pmc_io_pad_on(unsigned dpd)
{
    return switch_pmc_io_pad(dpd, true);
}

static inline bool switch_pmc_io_pad_off(unsigned dpd)
{
    return switch_pmc_io_pad(dpd, false);
}

#endif
