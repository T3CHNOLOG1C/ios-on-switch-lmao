// XusbLanes.h (//drivers/SwitchUSB:superspeed) on the host: when SS port 0's lane is up across DisplayPort alt mode's
// decisions and the role, what bringing it up and down writes (against the Linux sequences it follows, and never a bit of
// USB2 pad 0, its port or another lane), the falcon's SuperSpeed mailbox messages, and which AppleUSBXHCI writes
// SwitchXHCI adjusts.
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <vector>

#include "../XusbLanes.h"
#include "../XusbPorts.h"

namespace {

using Regs = std::map<uint32_t, uint32_t>;

// What XusbSuperSpeed.cpp writes to bring the lane up and down, applied to a register model.
void laneUp(Regs &r, std::vector<uint32_t> *elpgTrace = nullptr)
{
    for (const auto &write : xusb_b01_usb3_lane_data)
        r[XUSB_PADCTL_UPHY_MISC_CTL8(XUSB_SS_LANE)] = xusb_uphy_cfg(write[0], write[1]);
    r[XUSB_PADCTL_USB3_PAD_MUX] = xusb_pad_mux_ss(r[XUSB_PADCTL_USB3_PAD_MUX]);
    r[XUSB_PADCTL_SS_PORT_MAP] = xusb_ss_port_map(r[XUSB_PADCTL_SS_PORT_MAP]);
    r[XUSB_PADCTL_USB3_ECTL(0, 1)] = xusb_ectl1(r[XUSB_PADCTL_USB3_ECTL(0, 1)]);
    r[XUSB_PADCTL_USB3_ECTL(0, 2)] = xusb_ectl2(r[XUSB_PADCTL_USB3_ECTL(0, 2)]);
    r[XUSB_PADCTL_USB3_ECTL(0, 3)] = XUSB_ECTL3;
    r[XUSB_PADCTL_USB3_ECTL(0, 4)] = xusb_ectl4(r[XUSB_PADCTL_USB3_ECTL(0, 4)]);
    r[XUSB_PADCTL_USB3_ECTL(0, 6)] = XUSB_ECTL6;
    for (const auto &step : xusb_ss_up_steps) {
        r[XUSB_PADCTL_ELPG_PROGRAM_1] = xusb_elpg_apply(r[XUSB_PADCTL_ELPG_PROGRAM_1], step);
        if (elpgTrace)
            elpgTrace->push_back(r[XUSB_PADCTL_ELPG_PROGRAM_1]);
    }
}

void laneDown(Regs &r, std::vector<uint32_t> *elpgTrace = nullptr)
{
    for (const auto &step : xusb_ss_down_steps) {
        r[XUSB_PADCTL_ELPG_PROGRAM_1] = xusb_elpg_apply(r[XUSB_PADCTL_ELPG_PROGRAM_1], step);
        if (elpgTrace)
            elpgTrace->push_back(r[XUSB_PADCTL_ELPG_PROGRAM_1]);
    }
}

// Reset-like values with every bit outside SS port 0 and lane 5 set, so a stray write shows.
Regs boardState()
{
    return {
        {XUSB_PADCTL_USB3_PAD_MUX, 0xffffffffu & ~(3u << 22) & ~(1u << 6)},
        {XUSB_PADCTL_SS_PORT_MAP, 0x7777777u},
        {XUSB_PADCTL_ELPG_PROGRAM_1, 0xffffffffu},
        {XUSB_USB2_VBUS_ID, 0x12345678u},
        {XUSB_USB2_PORT_CAP, 0x3u},
        {XUSB_CHRG_OTGPAD0_CTL0, 0xabcdu},
        {XUSB_PADCTL_UPHY_MISC_CTL8(0), 0x55u},
        {XUSB_PADCTL_UPHY_MISC_CTL1(0), 0x66u},
    };
}

TEST(SwitchUsbLanes, LaneOnlyForAHostWhosePairIsFree)
{
    for (bool host : {false, true}) {
        for (auto lanes : {SWITCH_USB3_LANES_PENDING, SWITCH_USB3_LANES_FREE, SWITCH_USB3_LANES_TAKEN}) {
            for (bool pll : {false, true})
                EXPECT_EQ(xusb_ss_lane_wanted(host, lanes, pll), host && pll && lanes == SWITCH_USB3_LANES_FREE);
        }
    }
}

// r24-dpalt's sequencer as agreed: PENDING at attach, FREE for pin D (or no DisplayPort), TAKEN for pin C, hub enable as
// a second call, FREE+false at exit or detach. The lane is never up while the pairs are undecided or DisplayPort's.
TEST(SwitchUsbLanes, DockSessions)
{
    struct Step {
        bool host;
        switch_usb3_lanes lanes;
        bool up;
    };

    const std::vector<std::vector<Step>> sessions = {
        // Pin D: USB2 host first (the DR swap), SuperSpeed once DP Configure is acknowledged, kept with the hub on.
        {{false, SWITCH_USB3_LANES_PENDING, false},
         {true, SWITCH_USB3_LANES_PENDING, false},
         {true, SWITCH_USB3_LANES_FREE, true},
         {true, SWITCH_USB3_LANES_FREE, true},
         {false, SWITCH_USB3_LANES_FREE, false}},
        // Pin C: USB2 only for the whole session.
        {{false, SWITCH_USB3_LANES_PENDING, false},
         {true, SWITCH_USB3_LANES_PENDING, false},
         {true, SWITCH_USB3_LANES_TAKEN, false},
         {true, SWITCH_USB3_LANES_FREE, true}},
        // An OTG adapter: no alt mode engine, the pairs stay free.
        {{true, SWITCH_USB3_LANES_FREE, true}, {false, SWITCH_USB3_LANES_FREE, false}},
    };
    for (const auto &session : sessions) {
        Regs regs = boardState();
        bool up = false;
        for (const Step &step : session) {
            const bool want = xusb_ss_lane_wanted(step.host, step.lanes, true);
            if (want && !up)
                laneUp(regs);
            if (!want && up)
                laneDown(regs);
            up = want;
            EXPECT_EQ(up, step.up);
            EXPECT_EQ(xusb_ss_clamped(regs[XUSB_PADCTL_ELPG_PROGRAM_1]), !up);
        }
    }
}

TEST(SwitchUsbLanes, UpAndDownTouchOnlyThePortsOwnBits)
{
    const Regs before = boardState();
    Regs regs = before;
    laneUp(regs);
    EXPECT_EQ(regs[XUSB_PADCTL_USB3_PAD_MUX] & ~((3u << 22) | (1u << 6)), before.at(XUSB_PADCTL_USB3_PAD_MUX));
    EXPECT_EQ((regs[XUSB_PADCTL_USB3_PAD_MUX] >> 22) & 3u, 1u) << "lane 5 carries xusb";
    EXPECT_TRUE(regs[XUSB_PADCTL_USB3_PAD_MUX] & (1u << 6)) << "lane 5 out of IDDQ";
    EXPECT_EQ(regs[XUSB_PADCTL_SS_PORT_MAP] & ~7u, before.at(XUSB_PADCTL_SS_PORT_MAP) & ~7u);
    EXPECT_EQ(regs[XUSB_PADCTL_SS_PORT_MAP] & 7u, 0u) << "SS port 0 paired with USB2 port 0";
    EXPECT_EQ(regs[XUSB_PADCTL_ELPG_PROGRAM_1], 0xffffffffu & ~7u);
    for (uint32_t untouched : {XUSB_USB2_VBUS_ID, XUSB_USB2_PORT_CAP, XUSB_CHRG_OTGPAD0_CTL0,
                               XUSB_PADCTL_UPHY_MISC_CTL8(0), XUSB_PADCTL_UPHY_MISC_CTL1(0)})
        EXPECT_EQ(regs[untouched], before.at(untouched)) << std::hex << untouched;
    laneDown(regs);
    EXPECT_EQ(regs[XUSB_PADCTL_ELPG_PROGRAM_1], 0xffffffffu);
    EXPECT_EQ(regs[XUSB_USB2_VBUS_ID], before.at(XUSB_USB2_VBUS_ID));
}

// tegra210_pcie_phy_power_on/off: VCORE_DOWN, CLAMP_EN_EARLY, CLAMP_EN cleared in that order; set back in reverse.
TEST(SwitchUsbLanes, ClampOrderAsLinux)
{
    Regs regs = {{XUSB_PADCTL_ELPG_PROGRAM_1, 7u}};
    std::vector<uint32_t> up, down;
    laneUp(regs, &up);
    EXPECT_EQ(up, (std::vector<uint32_t>{3u, 1u, 0u}));
    laneDown(regs, &down);
    EXPECT_EQ(down, (std::vector<uint32_t>{2u, 3u, 7u}));
    EXPECT_GE(xusb_ss_up_steps[0].delay_us, 100u);
    EXPECT_GE(xusb_ss_up_steps[1].delay_us, 100u);
    EXPECT_GE(xusb_ss_down_steps[0].delay_us, 100u);
    EXPECT_GE(xusb_ss_down_steps[1].delay_us, 250u);
}

TEST(SwitchUsbLanes, PadSettingsAsLinuxAndProds)
{
    EXPECT_EQ(XUSB_PADCTL_UPHY_MISC_CTL8(5), 0x5bcu);
    EXPECT_EQ(XUSB_PADCTL_UPHY_MISC_CTL1(5), 0x5a0u);
    EXPECT_EQ(XUSB_PADCTL_USB3_ECTL(0, 1), 0xa60u);
    EXPECT_EQ(XUSB_PADCTL_USB3_ECTL(0, 6), 0xa74u);
    EXPECT_EQ(xusb_ectl1(0), 0x20027u);
    EXPECT_EQ(xusb_ectl1(0xffffffffu), 0xfffeffe7u);
    EXPECT_EQ(xusb_ectl2(0xabcd1234u), 0xabcd00fcu);
    EXPECT_EQ(xusb_ectl4(0x1234abcdu), 0x01c7abcdu);
    ASSERT_EQ(XUSB_B01_USB3_LANE_WRITES, 11u);
    EXPECT_EQ(xusb_uphy_cfg(0x35, 0x2587), 0x09352587u);
    EXPECT_EQ(xusb_uphy_cfg(0x97, 0x0080), 0x09970080u);
}

TEST(SwitchUsbLanes, LfpsDetection)
{
    const uint32_t other = 0x0f00ff0fu & ~((3u << 20) | (1u << 18) | (1u << 13));
    EXPECT_EQ(xusb_lfps_detect(other, true), other);
    EXPECT_EQ(xusb_lfps_detect(other, false), other | (1u << 20) | (1u << 18) | (1u << 13));
    EXPECT_EQ(xusb_lfps_detect(xusb_lfps_detect(other, false), true), other);
}

TEST(SwitchUsbLanes, FalconSuperSpeedMessages)
{
    auto d = xusb_mbox_ss_decide(XUSB_MBOX_DISABLE_SS_LFPS_DETECTION, 1u << 1);
    EXPECT_TRUE(d.handled && d.lfps && !d.lfps_enable && d.lfps_settle);
    EXPECT_EQ(d.reply, XUSB_MBOX_ACK);
    EXPECT_EQ(d.data, 1u << 1);
    d = xusb_mbox_ss_decide(XUSB_MBOX_ENABLE_SS_LFPS_DETECTION, 1u << 1);
    EXPECT_TRUE(d.handled && d.lfps && d.lfps_enable && !d.lfps_settle);
    d = xusb_mbox_ss_decide(XUSB_MBOX_DISABLE_SS_LFPS_DETECTION, 1u << 2);
    EXPECT_TRUE(d.handled && !d.lfps) << "SS port 1 is not on this board";
    EXPECT_EQ(d.reply, XUSB_MBOX_ACK);
    d = xusb_mbox_ss_decide(XUSB_MBOX_SAVE_DFE_CTLE_CTX, 0);
    EXPECT_TRUE(d.handled);
    EXPECT_EQ(d.reply, XUSB_MBOX_NAK) << "T210 has no DFE/CTLE context save (xhci-tegra NAKs)";
    for (uint32_t cmd : {2u, 3u, 4u, 5u, 6u, 12u, 13u})
        EXPECT_FALSE(xusb_mbox_ss_decide(cmd, 0).handled) << cmd;
}

// Tegra XUSB: SS ports 1-4, USB2 ports 5-8 (+HSIC); capability length 0x40, runtime at 0x2000 here.
TEST(SwitchUsbLanes, WritesSwitchXhciAdjusts)
{
    const uint32_t cap = 0x40, rt = 0x2000;
    auto kind = [&](uint32_t offset) {
        return xhci_write_kind(offset, cap, rt, 1, 4, 1);
    };
    for (unsigned port = 1; port <= 4; ++port) {
        EXPECT_EQ(kind(cap + 0x400 + 0x10 * (port - 1) + 4), XHCI_WRITE_SS_PORTPMSC) << port;
        EXPECT_EQ(kind(cap + 0x400 + 0x10 * (port - 1)), XHCI_WRITE_OTHER) << "PORTSC " << port;
    }
    EXPECT_EQ(kind(cap + 0x400 + 0x10 * 4 + 4), XHCI_WRITE_OTHER) << "a USB2 port's PORTPMSC";
    EXPECT_EQ(kind(rt + 0x24), XHCI_WRITE_IMOD);
    EXPECT_EQ(kind(rt + 0x20), XHCI_WRITE_OTHER) << "IMAN";
    EXPECT_EQ(kind(rt + 0x44), XHCI_WRITE_OTHER) << "an interrupter the hardware does not have";
    EXPECT_EQ(xhci_portpmsc_ss(0x0001ff7fu), 0x00010000u) << "U1/U2 timeouts 0, FLA kept";
    EXPECT_EQ(xhci_imod_floor(0), 160u);
    EXPECT_EQ(xhci_imod_floor(0x00050010u), 0x000500a0u);
    EXPECT_EQ(xhci_imod_floor(4000u), 4000u);
}

}  // namespace
