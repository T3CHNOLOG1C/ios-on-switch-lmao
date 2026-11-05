// native/loader/copy.c's page tables on the host (no privileged instruction or device): copy_tables_model.c decodes
// every descriptor over the 4 GiB the tables map, for each window size and destination offset (32 exact maps), then
// feeds the builder 26 malformed inputs that must be refused with the tables and their guard pages untouched.
#include <gtest/gtest.h>

#include "copy_tables_model.h"

namespace {
class LoaderCopyTables : public testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(copy_tables_setup()); }
    void TearDown() override { copy_tables_teardown(); }
};

class LoaderCopyTablesBySize : public LoaderCopyTables, public testing::WithParamInterface<unsigned> {};
}  // namespace

TEST(LoaderCopyTablesCases, ThirtyTwoExactMaps)
{
    EXPECT_EQ(copy_tables_sizes() * copy_tables_offsets(), 32u);
}

// Each window size at every destination offset: one exact 4 GiB map check each.
TEST_P(LoaderCopyTablesBySize, ExactMapAtEveryOffset)
{
    for (unsigned o = 0; o < copy_tables_offsets(); ++o) {
        SCOPED_TRACE(testing::Message() << "size index " << GetParam() << ", offset index " << o);
        copy_tables_positive(GetParam(), o);
    }
}

INSTANTIATE_TEST_SUITE_P(Sizes, LoaderCopyTablesBySize, testing::Range(0u, 8u));

TEST_F(LoaderCopyTables, MalformedInputsRefusedUntouched)
{
    EXPECT_EQ(copy_tables_negatives(), 26u);
}
