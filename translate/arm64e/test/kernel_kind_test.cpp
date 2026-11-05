#include <gtest/gtest.h>

extern "C" void kernel_kind_scenario(void);

TEST(KernelKind, HostMirrorsTheKernelsLoaderDecision)
{
    kernel_kind_scenario();
}
