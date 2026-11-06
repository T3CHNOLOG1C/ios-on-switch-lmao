// <switch/pmap.h>: which PTE rewrites in arm_clear_fast_fault need a TLB invalidate.
#include <gtest/gtest.h>

#include <cstdint>

typedef uint64_t pt_entry_t;

#define ARM_PTE_AF        0x0000000000000400ULL
#define ARM_PTE_WRITEABLE 0x0800000000000000ULL

#include <switch/pmap.h>

namespace {

// A valid read-only 4 KiB leaf with AF set (AP[2] is bit 7).
constexpr pt_entry_t kYoung = 0x00600000800047C3ULL;

TEST(SwitchPmap, OnlySettingTheAccessFlagSkipsTheTlbInvalidate)
{
    const pt_entry_t old = kYoung & ~ARM_PTE_AF, writable = kYoung & ~0x80ULL;
    EXPECT_FALSE(switch_pte_needs_tlbi(old, kYoung));
    EXPECT_FALSE(switch_pte_needs_tlbi(old, kYoung | ARM_PTE_WRITEABLE));
    EXPECT_FALSE(switch_pte_needs_tlbi(kYoung, kYoung));
    EXPECT_FALSE(switch_pte_needs_tlbi(kYoung, kYoung ^ ARM_PTE_WRITEABLE));
    EXPECT_TRUE(switch_pte_needs_tlbi(kYoung, old)) << "clearing AF: the old entry may be in a TLB";
    EXPECT_TRUE(switch_pte_needs_tlbi(kYoung, writable)) << "a write upgrade";
    EXPECT_TRUE(switch_pte_needs_tlbi(old, writable)) << "AF and a write upgrade";
    EXPECT_TRUE(switch_pte_needs_tlbi(old, kYoung + 0x4000)) << "another frame";
}

}  // namespace
