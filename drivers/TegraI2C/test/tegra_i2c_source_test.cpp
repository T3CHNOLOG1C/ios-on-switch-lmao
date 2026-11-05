// TegraI2C's bus table (kBus): I2C3 alone has chained transactions on. The touch scenarios (drivers/SwitchTouch/test)
// model I2C3's controller; nothing models the others, so this guards that a table edit does not chain them unseen.
#include <gtest/gtest.h>

#include <regex>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

TEST(TegraI2CSource, OnlyI2C3IsChained)
{
    const std::string table =
        switch_ios::test::Between(switch_ios::test::SourceText("../TegraI2C.cpp"), "constexpr BusInfo kBus[kBuses] = {", "};");
    // One entry per bus: "{base, ..., chained, request, channel},  // name".
    std::vector<std::string> chained;
    const std::regex entry(R"(\{\s*(0x[0-9a-fA-F]+),[^{}]*?,\s*(true|false),[^{}]*\})");
    unsigned buses = 0;
    for (std::sregex_iterator it(table.begin(), table.end(), entry), end; it != end; ++it) {
        ++buses;
        if ((*it)[2] == "true") chained.push_back((*it)[1]);
    }
    EXPECT_EQ(buses, 5u) << "the five Tegra210 I2C controllers";
    EXPECT_EQ(chained, std::vector<std::string>{"0x0500"}) << "I2C3 alone is chained";
}
