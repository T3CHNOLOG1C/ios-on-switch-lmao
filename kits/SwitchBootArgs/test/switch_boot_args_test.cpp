// kits/SwitchBootArgs: flags and numbers, present, absent and zero.
#include "kits/SwitchBootArgs/SwitchBootArgs.h"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>

namespace {
std::map<std::string, uint32_t> gArgs;

TEST(SwitchBootArgs, FlagsAndNumbers)
{
    gArgs = {{"switch-no-bt", 1}, {"switch-off", 0}, {"switch-diag", 3}};
    EXPECT_TRUE(switch_boot_flag("switch-no-bt"));
    EXPECT_FALSE(switch_boot_flag("switch-off"));
    EXPECT_FALSE(switch_boot_flag("switch-absent"));
    EXPECT_EQ(switch_boot_u32("switch-diag", 1), 3u);
    EXPECT_EQ(switch_boot_u32("switch-off", 9), 0u);
    EXPECT_EQ(switch_boot_u32("switch-absent", 9), 9u);
}
}  // namespace

extern "C" int PE_parse_boot_argn(const char *name, void *value, int size)
{
    const auto at = gArgs.find(name);
    if (at == gArgs.end() || size != sizeof(uint32_t))
        return 0;
    std::memcpy(value, &at->second, sizeof(uint32_t));
    return 1;
}
