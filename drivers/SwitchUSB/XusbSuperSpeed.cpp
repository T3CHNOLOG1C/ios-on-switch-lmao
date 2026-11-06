// SPDX-License-Identifier: GPL-2.0-only
// SS port 0's UPHY lane up and down (XusbLanes.h), for //drivers/SwitchUSB:superspeed. The role thread (SwitchUSB.cpp)
// moves the lane; the falcon mailbox (SwitchXHCI's work loop) sets its LFPS detection, a register the lane steps never
// write. Bounded register waits only (the ELPG delays), as Linux; no timer, no polling after bring-up.
#include <IOKit/IOLib.h>
#include "XusbLanes.h"
#include "XusbSuperSpeed.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

namespace {
constexpr uint64_t kPadctlPA = 0x7009F000;

volatile uint8_t *gPadctl;
bool gUp, gDefaultsWritten;

uint32_t r(uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t *>(gPadctl + offset);
}

void w(uint32_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(gPadctl + offset) = value;
}

bool mapped()
{
    if (!gPadctl)
        gPadctl = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPadctlPA, 0x1000));
    return gPadctl != nullptr;
}

void runSteps(const xusb_elpg_step *steps)
{
    for (uint32_t i = 0; i < XUSB_SS_STEPS; ++i) {
        w(XUSB_PADCTL_ELPG_PROGRAM_1, xusb_elpg_apply(r(XUSB_PADCTL_ELPG_PROGRAM_1), steps[i]));
        if (steps[i].delay_us)
            IODelay(steps[i].delay_us);
    }
}

// The lane's B01 defaults go in once, with the lane still clamped: tegra210_pex_uphy_enable writes them for every
// xusb lane when it brings PLL0 up, and the PCIe controller that brought it up here has no xusb lane.
void writeDefaults()
{
    if (gDefaultsWritten)
        return;
    for (uint32_t i = 0; i < XUSB_B01_USB3_LANE_WRITES; ++i)
        w(XUSB_PADCTL_UPHY_MISC_CTL8(XUSB_SS_LANE),
          xusb_uphy_cfg(xusb_b01_usb3_lane_data[i][0], xusb_b01_usb3_lane_data[i][1]));
    gDefaultsWritten = true;
}
}  // namespace

bool switch_usb_ss_pll_locked(void)
{
    return mapped() && (r(XUSB_PADCTL_UPHY_PLL_P0_CTL1) & XUSB_UPHY_PLL_LOCKDET);
}

bool switch_usb_ss_lane(bool up)
{
    if (!mapped())
        return false;
    if (up == gUp)
        return true;
    if (up) {
        writeDefaults();
        w(XUSB_PADCTL_USB3_PAD_MUX, xusb_pad_mux_ss(r(XUSB_PADCTL_USB3_PAD_MUX)));
        w(XUSB_PADCTL_SS_PORT_MAP, xusb_ss_port_map(r(XUSB_PADCTL_SS_PORT_MAP)));
        w(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 1), xusb_ectl1(r(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 1))));
        w(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 2), xusb_ectl2(r(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 2))));
        w(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 3), XUSB_ECTL3);
        w(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 4), xusb_ectl4(r(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 4))));
        w(XUSB_PADCTL_USB3_ECTL(XUSB_SS_PORT, 6), XUSB_ECTL6);
        runSteps(xusb_ss_up_steps);
    } else {
        runSteps(xusb_ss_down_steps);
    }
    gUp = up;
    return true;
}

void switch_usb_ss_lfps(bool enable)
{
    if (!mapped())
        return;
    w(XUSB_PADCTL_UPHY_MISC_CTL1(XUSB_SS_LANE), xusb_lfps_detect(r(XUSB_PADCTL_UPHY_MISC_CTL1(XUSB_SS_LANE)), enable));
}

void switch_usb_ss_describe(char *buffer, size_t size)
{
    if (!mapped()) {
        snprintf(buffer, size, "PADCTL not mapped");
        return;
    }
    const uint32_t elpg = r(XUSB_PADCTL_ELPG_PROGRAM_1);
    snprintf(buffer, size, "SS lane %s (USB3_PAD_MUX %#x SS_PORT_MAP %#x ELPG_1 %#x%s, PLL0 %s)", gUp ? "up" : "down",
             r(XUSB_PADCTL_USB3_PAD_MUX), r(XUSB_PADCTL_SS_PORT_MAP), elpg, xusb_ss_clamped(elpg) ? " clamped" : "",
             switch_usb_ss_pll_locked() ? "locked" : "not locked");
}
