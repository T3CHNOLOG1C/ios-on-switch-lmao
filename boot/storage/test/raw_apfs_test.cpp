// boot/storage/raw_apfs.c, the raw APFS block backend, against raw_apfs_model.c's fixture of its block I/O.
#include <gtest/gtest.h>

extern "C" unsigned raw_apfs_cases(void);

TEST(NativeRawApfs, BackendAgainstTheFixture)
{
    EXPECT_EQ(raw_apfs_cases(), 63u);
}
