// r17-launch: XNU's arm64 memcmp/bcmp compare a word at a time (osfmk/arm64/switch_memcmp.h) with the byte loops'
// exact results; AMFI's entitlement-key lookups spent 2.9 s per boot in the byte loop (kprof r7v).
#include <gtest/gtest.h>

#include "xnu_memcmp_harness.h"

TEST(XnuMemcmp, EveryLengthAlignmentAndFirstDifference)
{
    unsigned long cases = 0;
    EXPECT_EQ(xnu_memcmp_exhaustive(&cases), 0u) << "of " << cases << " cases";
    // 81 lengths x 8 misalignments x (n + 1) first differences x 3 signs x both orders.
    EXPECT_EQ(cases, 81ul * 8 * 41 * 3 * 2);
}

TEST(XnuMemcmp, RandomInputs)
{
    unsigned long cases = 0;
    EXPECT_EQ(xnu_memcmp_random(17, 200000, &cases), 0u) << "of " << cases << " cases";
    EXPECT_EQ(cases, 200000u);
}
