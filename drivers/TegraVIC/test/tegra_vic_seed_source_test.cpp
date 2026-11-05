// A TransformSurface destination's seed moves when its job is done (r18-planes), as Apple's AppleM2ScalerCSCDriver moves
// it: TegraVIC's engine thread calls the IOSurface kext's device write end on the destination after the job and before
// the caller hears back, through a binding abi_check.py verifies (its first bytes, the seed's own loads and stores, and
// that Apple's scaler imports that very function).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using ioswitch::test::Between;
using ioswitch::test::SourceText;
using testing::HasSubstr;

namespace {
void ExpectBefore(const std::string &text, const std::string &a, const std::string &b)
{
    const size_t at = text.find(a), bt = text.find(b);
    ASSERT_NE(at, std::string::npos) << a;
    ASSERT_NE(bt, std::string::npos) << b;
    EXPECT_LT(at, bt) << a << " before " << b;
}
}  // namespace

TEST(TegraVICSeed, DestinationSeedMovesBeforeTheReply)
{
    const std::string engine =
        Between(SourceText("../TegraVICUserClient.cpp"), "void engineThread(void *, wait_result_t)", "\n}\n");
    ExpectBefore(engine, "runJob(job)", "tegra_vic_surface_written(&job->dst);");
    ExpectBefore(engine, "tegra_vic_surface_written(&job->dst);", "tegra_vic_surface_release(&job->dst);");
    ExpectBefore(engine, "tegra_vic_surface_written(&job->dst);", "iouc_send_async_result64");
    EXPECT_THAT(SourceText("../TegraVICSurface.cpp"), HasSubstr("tegra_vic_ios_device_unlock(s->surface, 0)"));
}

TEST(TegraVICSeed, BindingIsChecked)
{
    const std::string check = SourceText("../abi_check.py");
    EXPECT_THAT(check, HasSubstr("('_tegra_vic_ios_device_unlock', 0xfffffff005ba0fc8,"));
    EXPECT_THAT(check, HasSubstr("('_tegra_vic_ios_device_unlock', 0xfffffff006e5ea88, 'AppleM2ScalerCSCDriver')"));
    EXPECT_THAT(SourceText("../original-bindings.S"),
                HasSubstr(".set _tegra_vic_ios_device_unlock, 0xfffffff004ba0fc8"));
}
