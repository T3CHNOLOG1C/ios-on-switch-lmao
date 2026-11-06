// kits/SwitchFalcon: the v1 image parser on built and damaged images, and the boot's register sequence against a
// model falcon whose DMA queue is never full and always drains.
#include "kits/SwitchFalcon/SwitchFalcon.h"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {
void put32(std::vector<uint8_t> &image, size_t at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        image[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

// Header, OS header at 0x40, bin data at 0x100: 0x300 bytes of code at bin offset 0, 0x120 of data at 0x300.
std::vector<uint8_t> image()
{
    std::vector<uint8_t> fw(0x800);
    put32(fw, 0, 0x10de);
    put32(fw, 4, 1);
    put32(fw, 8, 0x800);
    put32(fw, 12, 0x40);
    put32(fw, 16, 0x100);
    put32(fw, 20, 0x700);
    put32(fw, 24, 0x80);
    put32(fw, 28, 0x400);
    put32(fw, 0x40, 0);
    put32(fw, 0x44, 0x300);
    put32(fw, 0x48, 0x300);
    put32(fw, 0x4c, 0x120);
    put32(fw, 0x50, 1);
    return fw;
}

TEST(SwitchFalconParse, ReadsTheHeaders)
{
    const auto fw = image();
    switch_falcon_image parsed;
    ASSERT_TRUE(switch_falcon_parse(fw.data(), fw.size(), &parsed));
    EXPECT_EQ(parsed.bin_data, 0x100u);
    EXPECT_EQ(parsed.code_size, 0x300u);
    EXPECT_EQ(parsed.data, 0x300u);
    EXPECT_EQ(parsed.data_size, 0x120u);
    EXPECT_EQ(parsed.apps, 1u);
    EXPECT_EQ(parsed.fce_header, 0x80u);
    EXPECT_EQ(parsed.fce_data, 0x400u);
}

TEST(SwitchFalconParse, RejectsDamagedImages)
{
    switch_falcon_image parsed;
    auto fw = image();
    EXPECT_FALSE(switch_falcon_parse(fw.data(), 0xff, &parsed));
    put32(fw, 0, 0x1234);
    EXPECT_FALSE(switch_falcon_parse(fw.data(), fw.size(), &parsed));
    fw = image();
    put32(fw, 8, 0x900);
    EXPECT_FALSE(switch_falcon_parse(fw.data(), fw.size(), &parsed));
    // An image size below the OS header's: size - 20 must not wrap and let the OS header point anywhere.
    fw = image();
    put32(fw, 8, 8);
    put32(fw, 12, 0x10000000);
    EXPECT_FALSE(switch_falcon_parse(fw.data(), fw.size(), &parsed));
    fw = image();
    put32(fw, 0x44, 0x701);
    EXPECT_FALSE(switch_falcon_parse(fw.data(), fw.size(), &parsed));
    fw = image();
    put32(fw, 0x44, 0);
    EXPECT_FALSE(switch_falcon_parse(fw.data(), fw.size(), &parsed));
}

std::map<uint32_t, uint32_t> gRegisters;
std::vector<std::pair<uint32_t, uint32_t>> gWrites;
bool gScrubbing;
uint64_t gNow;
uint8_t gEngine[0x2000];

uint32_t offsetOf(uintptr_t address)
{
    return static_cast<uint32_t>(address - reinterpret_cast<uintptr_t>(gEngine));
}

TEST(SwitchFalconBoot, QueuesDataThenCodeAndStarts)
{
    gRegisters.clear();
    gWrites.clear();
    gScrubbing = false;
    const auto fw = image();
    switch_falcon_image parsed;
    ASSERT_TRUE(switch_falcon_parse(fw.data(), fw.size(), &parsed));
    ASSERT_EQ(switch_falcon_load(gEngine, 0x80000000, &parsed), SWITCH_FALCON_OK);
    switch_falcon_start(gEngine);

    std::vector<std::pair<uint32_t, uint32_t>> want = {
        {SWITCH_FALCON_DMACTL, 0},
        {SWITCH_FALCON_DMATRFBASE, (0x80000000u + 0x100) >> 8},
    };
    for (uint32_t offset = 0; offset < 0x120; offset += 256) {
        want.push_back({SWITCH_FALCON_DMATRFMOFFS, offset});
        want.push_back({SWITCH_FALCON_DMATRFFBOFFS, 0x300 + offset});
        want.push_back({SWITCH_FALCON_DMATRFCMD, 6u << 8});
    }
    for (uint32_t offset = 0; offset < 0x300; offset += 256) {
        want.push_back({SWITCH_FALCON_DMATRFMOFFS, offset});
        want.push_back({SWITCH_FALCON_DMATRFFBOFFS, offset});
        want.push_back({SWITCH_FALCON_DMATRFCMD, (6u << 8) | (1u << 4)});
    }
    want.push_back({SWITCH_FALCON_IRQMSET, 0xfff2u});
    want.push_back({SWITCH_FALCON_IRQDEST, 0xfff0u});
    want.push_back({SWITCH_FALCON_ITFEN, 3});
    want.push_back({SWITCH_FALCON_BOOTVEC, 0});
    want.push_back({SWITCH_FALCON_CPUCTL, 1u << 1});
    EXPECT_EQ(gWrites, want);
}

TEST(SwitchFalconBoot, ReportsScrubbingThatNeverEnds)
{
    gWrites.clear();
    gScrubbing = true;
    switch_falcon_image parsed = {};
    EXPECT_EQ(switch_falcon_load(gEngine, 0, &parsed), SWITCH_FALCON_SCRUBBING);
    EXPECT_TRUE(gWrites.empty());
    char text[160];
    EXPECT_NE(std::string(switch_falcon_state(gEngine, text, sizeof(text))).find("CPUCTL"), std::string::npos);
}
}  // namespace

extern "C" uint32_t switch_mmio_model_read32(uintptr_t address)
{
    const uint32_t offset = offsetOf(address);
    if (offset == SWITCH_FALCON_DMACTL)
        return gScrubbing ? 1u << 2 : 0;  // DMEM still scrubbing
    if (offset == SWITCH_FALCON_DMATRFCMD)
        return 1u << 1;
    return gRegisters[offset];
}

extern "C" void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    gRegisters[offsetOf(address)] = value;
    gWrites.emplace_back(offsetOf(address), value);
}

extern "C" uint64_t mach_absolute_time(void)
{
    return gNow += 1000;
}

extern "C" void clock_interval_to_deadline(uint32_t interval, uint32_t scale, uint64_t *deadline)
{
    *deadline = gNow + (uint64_t)interval * scale;
}

extern "C" uintptr_t ml_io_map(uintptr_t phys_addr, uintptr_t)
{
    return phys_addr;
}
