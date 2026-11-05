// The diagnostics SD reader's bounded multi-block transport (sdmmc_read.c, sdmmc_dma.c) against sdmmc_batch_model.c's
// independent SDHCI register model, under ASan/UBSan: CMD17/CMD18, exact FIFO blocks, Auto CMD12, busy, deadlines,
// range refusals before any MMIO write, fail-stop on every injected fault.
#include <gtest/gtest.h>

extern "C" unsigned sdmmc_batch_cases(void);

TEST(DiagnosticsSdmmc, BoundedBatchReadsAgainstTheModel)
{
    EXPECT_EQ(sdmmc_batch_cases(), 48u);
}
