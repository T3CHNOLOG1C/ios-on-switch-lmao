// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 (T210B01) XUSB host controller hardware: everything Apple's AppleUSBXHCI cannot know about.
// After powerOn() and loadFirmware() the block at 0x70090000 is a plain xHCI 1.0 controller and
// AppleUSBXHCI (through SwitchXHCI) drives it; this layer keeps only the Tegra side: clocks and power
// partitions, the IPFS/FPCI bridge (BAR0, bus master, interrupt routing), the falcon microcontroller
// that implements the xHCI and its firmware, the falcon mailbox (its own interrupt) and the shared pad
// controller (USB2 port 0 role overrides, the T210B01 UPHY lane of SuperSpeed port 0).
//
// Sources: CTCaer switch-l4t-kernel-4.9 drivers/usb/host/xhci-tegra.c (tegra210b01_soc, tegra_xusb_config,
// tegra_xhci_load_firmware, tegra_xusb_mbox_*, tegra_xusb_disable_hsic_wake), drivers/phy/tegra/
// xusb-tegra210.c (usb2 phy power on, vbus/id override, uphy_init, pex_uphy_enable T210B01 branch,
// pcie_phy_power_on, usb3_set_lfps_detect, init_usb3_port), clk-tegra210b01.c (UPHY management clock,
// PLLE hardware sequencer), MyEcoria/nxk user/clk (PLLE enable, MIT). OLED DTB xusb@70090000:
// registers 0x70090000/0x8000 (xHCI), 0x70098000 (FPCI), 0x70099000 (IPFS); interrupts SPI 39 host
// (INTID 71), SPI 40 mailbox (INTID 72), SPI 49 padctl (unused here); usb2-0 "otg" with usb3-0 companion
// on UPHY lane pcie-5 (tegra210b01_usb3_map); iommus = XUSB_HOST, which must be off (physical DMA).
//
// Waits on hardware status with no interrupt behind it (PLL lock, falcon DMA, CNR after boot, mailbox
// ownership) are bounded register polls at bring-up, as in Linux; events are interrupt-driven only.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace xhost {

constexpr uint64_t kXhciPA = 0x70090000, kXhciBytes = 0x8000;
constexpr uint32_t kHostIntid = 32 + 39, kMailboxIntid = 32 + 40;

// Falcon mailbox commands (xhci-tegra.c enum tegra_xusb_mbox_cmd).
enum MboxCmd : uint32_t {
    MBOX_MSG_ENABLED = 1,
    MBOX_INC_FALC_CLOCK,
    MBOX_DEC_FALC_CLOCK,
    MBOX_INC_SSPI_CLOCK,
    MBOX_DEC_SSPI_CLOCK,
    MBOX_SET_BW,
    MBOX_SET_SS_PWR_GATING,
    MBOX_SET_SS_PWR_UNGATING,
    MBOX_SAVE_DFE_CTLE_CTX,
    MBOX_AIRPLANE_MODE_ENABLED,
    MBOX_AIRPLANE_MODE_DISABLED,
    MBOX_START_HSIC_IDLE,
    MBOX_STOP_HSIC_IDLE,
    MBOX_DBC_WAKE_STACK,
    MBOX_HSIC_PRETEND_CONNECT,
    MBOX_RESET_SSPI,
    MBOX_DISABLE_SS_LFPS_DETECTION,
    MBOX_ENABLE_SS_LFPS_DETECTION,
    MBOX_ACK = 128,
    MBOX_NAK = 129,
};

enum class PortRole : uint8_t { Device, Host };

class Controller {
public:
    bool map();  // MMIO only, no access
    // SMMU translation for the XUSB_HOST client must be off: AppleUSBXHCI hands the controller physical
    // addresses (identity SwitchDMAMapper). Returns false (and says why) when it is on.
    bool smmuBypassed(char *why, size_t size);
    bool powerOn();  // PLLs, pads, partitions XUSBA+XUSBC, clocks, IPFS/FPCI
    bool loadFirmware(const uint8_t *image, size_t size, char *why, size_t whySize);
    bool superSpeedUp();           // PLLE, UPHY PLL P0, lane pcie-5 -> SS port 0 (switch-usb3=1)
    void mapFakeSuperSpeedPort();  // SS port 0 mapped to USB2 port 0 without a lane (DT usb3-port-fake)

    // USB2 port 0 (the USB-C port's D+/D-) between the XUDC and this host: OTG capability, then the ID
    // and VBUS overrides the hardware routes by (xusb-tegra210.c id/vbus override).
    void setPortRole(PortRole role);

    // xHCI registers through our own mapping (bring-up, the interrupt watchdog).
    uint32_t xhci(uint32_t offset) const { return *reinterpret_cast<volatile uint32_t *>(regs + offset); }
    uint32_t capLength() const { return xhci(0) & 0xff; }
    uint32_t runtimeOffset() const { return xhci(0x18) & ~0x1fu; }

    // Mailbox. takeMailboxInterrupt() runs in the primary interrupt filter: it acknowledges SMI_INTR
    // (W1C) and says whether a message waits (or the firmware hung). serviceMailbox() runs on the work
    // loop: reads the message, answers it (ACK/NAK), returns false only on a firmware hang.
    bool takeMailboxInterrupt();
    bool serviceMailbox(uint32_t falconKHz);
    bool sendMailbox(uint32_t cmd, uint32_t data);   // acquires ownership, waits for the firmware to take it
    uint32_t mailboxPending() const;                 // SMI_INTR enable/hang bits (watchdog)
    void describe(char *buffer, size_t size) const;  // one line: USBSTS, IMAN0, ERDP0, falcon, SMI, owner

private:
    volatile uint8_t *car = nullptr, *pmcPage = nullptr, *fusePage = nullptr, *mc = nullptr, *padctl = nullptr,
                     *regs = nullptr, *fpci = nullptr, *ipfs = nullptr;
    bool mailboxHang = false;

    static uint32_t r(volatile uint8_t *b, uint32_t o) { return *reinterpret_cast<volatile uint32_t *>(b + o); }
    static void w(volatile uint8_t *b, uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(b + o) = v; }
    static void rmw(volatile uint8_t *b, uint32_t o, uint32_t clear, uint32_t set)
    {
        w(b, o, (r(b, o) & ~clear) | set);
    }
    static bool waitBits(volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t value, uint32_t us);
    uint32_t pmc(uint32_t o) const;
    void pmcWrite(uint32_t o, uint32_t v);
    bool powerUp(uint32_t partition, uint32_t clockSet, uint32_t resetSet, uint32_t bit);
    uint32_t csbRead(uint32_t offset);
    void csbWrite(uint32_t offset, uint32_t value);
    bool plleUp();
    void laneConfig(uint32_t ctl8, const uint16_t (*data)[2], unsigned count);
    void setLfpsDetect(bool enable);
};

}  // namespace xhost
