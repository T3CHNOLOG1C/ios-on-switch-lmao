// boot/loader/seed.c (the per-boot /chosen/random-seed from SE2's DRBG) against seed_model.c's model of CAR's
// clocks, SE2's registers and DRBG, the AHB write queue and the counter: the shipped case, the entropy clock off, and
// every failure leaving the pinned seed (plus the counter) with a status for XNU's log line.
#include <gtest/gtest.h>

extern "C" unsigned seed_scenarios(void);

TEST(LoaderSeed, MixAgainstTheModel)
{
    EXPECT_EQ(seed_scenarios(), 13u);
}
