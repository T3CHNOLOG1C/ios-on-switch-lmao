// The diagnostics SD writer (sdmmc_read.c, sdmmc_dma.c: extent permission before I/O, exact FIFO, fail-stop, CMD13,
// disabled-cache evidence, malformed extensions) against sdmmc_write_model.c's host fault injection, under ASan/UBSan.
#include <gtest/gtest.h>

extern "C" unsigned sdmmc_write_cases(void);

TEST(DiagnosticsSdmmc, BoundedWritesAndCacheAgainstTheModel)
{
    EXPECT_EQ(sdmmc_write_cases(), 86u);
}
