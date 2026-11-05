// native/storage/sdmmc_read.c (the loader's read-only PIO state machine) against sdmmc_read_model.c's independent
// SDHCI register model: exact readiness samples and masks, only CMD17, nothing that powers, clocks, resets, DMAs or
// writes the medium.
#include <gtest/gtest.h>

extern "C" unsigned sdmmc_read_cases(void);

TEST(NativeSdmmc, RegisterProtocolBoundsFaultsAndNoMediaWriteCommands)
{
    EXPECT_EQ(sdmmc_read_cases(), 550u);
}
