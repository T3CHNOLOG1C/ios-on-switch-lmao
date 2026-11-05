// native/loader/emc.c (the loader's EMC raise) against emc_model.c's model of the BPMP mailbox, I2C5 and the counter:
// the raise itself, the kernel's later no-op, and every refusal leaving the clock and channel as they were.
#include <gtest/gtest.h>

extern "C" unsigned emc_scenarios(void);

TEST(LoaderEmc, RaiseAndRefusalsAgainstTheModel)
{
    EXPECT_EQ(emc_scenarios(), 13u);
}
