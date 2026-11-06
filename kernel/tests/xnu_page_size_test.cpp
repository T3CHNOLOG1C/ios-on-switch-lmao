// The page sizes the native-4K guards choose in both trees (xnu_page_size_harness.c): the 16k kernel has 16 KiB VM
// pages everywhere userland looks, as an iPad does; the 4k kernel keeps 4 KiB everywhere.
#include <gtest/gtest.h>

#include <cstdint>

#include "xnu_page_size_harness.h"

namespace {

constexpr uint64_t kSwitchRam = 4ULL << 30;
constexpr xnu_tree kTrees[] = {XNU_10063, XNU_12377};

TEST(XnuPageSize, The16kKernelUses16KibPagesEverywhere)
{
    for (xnu_tree tree : kTrees) {
        xnu_page_shifts s = {};
        xnu_page_shifts_16k(tree, kSwitchRam, &s);
        EXPECT_EQ(s.kernel, 14) << tree;
        EXPECT_EQ(s.user32, 14) << tree;
        EXPECT_EQ(s.commpage, 14) << tree;
        EXPECT_EQ(s.commpage_legacy, 14) << tree;
        EXPECT_EQ(s.user_map, 14) << tree;
        EXPECT_EQ(s.shared_region, 14) << tree;
    }
}

// Apple's own rule: 16 KiB kernel pages only above 1 GiB, user maps at 16 KiB regardless. The console always reports
// 4 GiB (the RAM ledger panics on anything else).
TEST(XnuPageSize, The16kKernelFollowsApplesRamRule)
{
    for (xnu_tree tree : kTrees) {
        xnu_page_shifts s = {};
        xnu_page_shifts_16k(tree, 1ULL << 30, &s);
        EXPECT_EQ(s.kernel, 12) << tree;
        EXPECT_EQ(s.user_map, 14) << tree;
        EXPECT_EQ(s.commpage, 14) << tree;
    }
}

TEST(XnuPageSize, The4kKernelUses4KibPagesEverywhere)
{
    for (xnu_tree tree : kTrees) {
        for (uint64_t ram : {1ULL << 30, kSwitchRam}) {
            xnu_page_shifts s = {};
            xnu_page_shifts_4k(tree, ram, &s);
            EXPECT_EQ(s.kernel, 12) << tree;
            EXPECT_EQ(s.user32, 12) << tree;
            EXPECT_EQ(s.commpage, 12) << tree;
            EXPECT_EQ(s.commpage_legacy, 12) << tree;
            EXPECT_EQ(s.user_map, 12) << tree;
            EXPECT_EQ(s.shared_region, 12) << tree;
        }
    }
}

}  // namespace
