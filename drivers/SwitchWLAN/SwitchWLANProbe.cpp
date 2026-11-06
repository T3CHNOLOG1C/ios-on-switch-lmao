// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only look at the Switch OLED's Wi-Fi/Bluetooth path, to see what the boot chain left before anything powers it.
//
// The hardware (switchroot switch-l4t-platform-t210-nx: tegra210b01-nx-common.dtsi, tegra210b01-fric-gpio.dtsi):
// a BCM4356A3 combo. Wi-Fi on the PCIe controller at 0x01003000, root port 1 (x1 on UPHY lane 0), WL_REG_ON on GPIO H1
// (driven high by default), wake-host on H2. Bluetooth on UART-D (0x70006300, 3 Mbaud, RTS/CTS), BT_REG_ON on H4,
// device wake H3, host wake H5. Nothing here writes a register. A block's registers are read only while its clock
// runs and its reset is released, since an unclocked read stalls the bus.
//
// Interrupts for the radio driver to come (nothing here takes one): the PCIe block raises main-GIC SPI 98 = INTID 130
// ("intr": AFI errors, and legacy INTx through interrupt-map) and SPI 99 = INTID 131 ("msi") (Linux
// tegra210.dtsi:25-31; OLED DTB pcie@1003000). brcmfmac PCIe uses MSI when it can: the AFI decodes MSI writes into
// AFI_MSI_VEC0..7 (256 vectors, enabled in AFI_MSI_EN_VEC0..7, unmasked by AFI_INTR_MASK bit 8; CTCaer pci-tegra.c
// 97-127, tegra_pcie_msi_irq 2940), so one vector on INTID 131 is the simplest path; INTx on INTID 130 shares the
// line with AFI error reporting (tegra_pcie_isr 1193, AFI_INTR_CODE). WL host wake is PH2 = GPIO 58 (bank 1, SPI 33;
// tegra210b01-fric-gpio.dtsi:30, pinmux wifi_wake_ap_ph2 0x31bc), a TegraGPIO nub if the radio ever sleeps.
//
// Prints one line.
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include "SwitchWLANStation.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

namespace {

// Blocks (hekate bdk/soc/t210.h; Linux tegra210.dtsi pcie@1003000: root ports at +0/+0x1000, PADS +0x3000, AFI +0x3800).
constexpr vm_offset_t kCarPA = 0x60006000, kGpioPA = 0x6000D000, kPinmuxPA = 0x70003000, kPmcPA = 0x7000E400;
constexpr vm_offset_t kFusePA = 0x7000F800, kMcPA = 0x70019000, kPadctlPA = 0x7009F000, kPciePA = 0x01000000;

// Clock and reset controller (hekate bdk/soc/clock.h).
constexpr uint32_t kRstDevU = 0x00C, kClkEnbH = 0x014, kClkEnbU = 0x018, kMiscClkEnb = 0x048, kRstDevW = 0x35C;
constexpr uint32_t kPlleBase = 0x0E8, kPlleMisc = 0x0EC;
constexpr uint32_t kUUartD = 1u << 1, kUPcie = 1u << 6, kUAfi = 1u << 8, kUPcieX = 1u << 10;
constexpr uint32_t kWPadctl = 1u << 14, kHFuse = 1u << 7, kFuseVisible = 1u << 28;
constexpr uint32_t kPlleEnable = 1u << 31, kPlleLock = 1u << 11;  // Linux clk-pll.c PLLE_BASE_ENABLE, PLLE_MISC_LOCK

// Power management controller (Linux pmc.c: powergate PCIE = 3; tegra210_io_pads PEX_BIAS/CLK1/CLK2 = DPD bits 4-6).
constexpr uint32_t kPwrgateStatus = 0x038, kIoDpdStatus = 0x1BC, kPartPcie = 1u << 3, kDpdPex = 7u << 4;

// GPIO port H: bank 1, port 3 (hekate bdk/soc/gpio.c), registers CNF/OE/OUT/IN 0x10 apart.
constexpr uint32_t kGpioH = 0x10C;
constexpr unsigned kWlRegOn = 1, kBtRegOn = 4;

// Pinmux of WIFI_EN (PH0) and WIFI_RST (PH1) (hekate bdk/soc/pinmux.h).
constexpr uint32_t kPinWifiEn = 0x1B4, kPinWifiRst = 0x1B8;

// Memory controller SMMU (hekate bdk/mem/mc_t210.h): bit 31 of a client's ASID register routes it through the SMMU.
constexpr uint32_t kSmmuConfig = 0x010, kSmmuAfiAsid = 0x238;

// XUSB pad control (CTCaer xusb-tegra210.c): lane n function in USB3_PAD_MUX bits 12+2n (0 pcie-x1, 1 xusb,
// 2 sata, 3 pcie-x4); UPHY PLL0 CTL1 bit 3 enable, bit 15 lock detect.
constexpr uint32_t kPadMux = 0x028, kUphyPll0Ctl1 = 0x360;

// PCIe (CTCaer pci-tegra.c), offsets from kPciePA.
constexpr uint32_t kRootPort1 = 0x1000, kAfi = 0x3800;
constexpr uint32_t kAfiPcieConfig = 0x0F8, kAfiPex1Ctrl = 0x118, kAfiPexRst = 1u << 0;
constexpr uint32_t kRpLinkStatus = 0x090, kRpVendXp = 0xF00;

// Fuse cache: OPT_LOT_CODE_0 (switchroot U-Boot board/nintendo/switch/nintendo-switch.c).
constexpr uint32_t kFuseLot0 = 0x208;

volatile uint32_t *map(vm_offset_t pa)
{
    return reinterpret_cast<volatile uint32_t *>(ml_io_map(pa, 0x4000));
}

volatile uint32_t *gCar, *gFuse;  // kept: switch_wlan_mac may be called again

inline uint32_t rd(const volatile uint32_t *base, uint32_t offset)
{
    return base[offset / 4];
}

inline unsigned bit(uint32_t value, unsigned n)
{
    return (value >> n) & 1;
}

inline unsigned flag(uint32_t value, uint32_t mask)
{
    return (value & mask) ? 1 : 0;
}

}  // namespace

// The addresses U-Boot puts in device_wifi_mac and device_bt_mac: Nintendo's OUI and the low bytes of the lot code
// fuse (+2 Wi-Fi, +1 Bluetooth). bt may be null.
extern "C" int switch_wlan_mac(uint8_t wifi[6], uint8_t bt[6])
{
    if (!gCar)
        gCar = map(kCarPA);
    if (!gFuse)
        gFuse = map(kFusePA);
    if (!gCar || !gFuse)
        return -1;
    if (!(rd(gCar, kClkEnbH) & kHFuse) || !(rd(gCar, kMiscClkEnb) & kFuseVisible))
        return -1;
    const uint32_t lot0 = rd(gFuse, kFuseLot0);
    if (!lot0)
        return -1;
    const uint8_t prefix[5] = {0x98, 0xB6, 0xE9, static_cast<uint8_t>(lot0 >> 16), static_cast<uint8_t>(lot0 >> 8)};
    for (unsigned i = 0; i < 5; ++i) {
        wifi[i] = prefix[i];
        if (bt)
            bt[i] = prefix[i];
    }
    wifi[5] = static_cast<uint8_t>(lot0 + 2);
    if (bt)
        bt[5] = static_cast<uint8_t>(lot0 + 1);
    return 0;
}

extern "C" void switch_wlan_probe(void)
{
    const volatile uint32_t *car = map(kCarPA);
    const volatile uint32_t *pmc = map(kPmcPA);
    const volatile uint32_t *gpio = map(kGpioPA);
    const volatile uint32_t *pinmux = map(kPinmuxPA);
    const volatile uint32_t *mc = map(kMcPA);
    const volatile uint32_t *padctl = map(kPadctlPA);
    const volatile uint32_t *pcie = map(kPciePA);
    if (!car || !pmc || !gpio || !pinmux || !mc || !padctl || !pcie) {
        IOLog("SwitchWLAN: map failed\n");
        return;
    }

    const uint32_t rstU = rd(car, kRstDevU);
    const uint32_t clkU = rd(car, kClkEnbU);
    const uint32_t rstW = rd(car, kRstDevW);
    const uint32_t plle = rd(car, kPlleBase);
    const uint32_t plleMisc = rd(car, kPlleMisc);
    const uint32_t gates = rd(pmc, kPwrgateStatus);
    const uint32_t dpd = rd(pmc, kIoDpdStatus);
    const uint32_t out = rd(gpio, kGpioH + 0x20);
    const uint32_t oe = rd(gpio, kGpioH + 0x10);
    const uint32_t in = rd(gpio, kGpioH + 0x30);
    const uint32_t smmu = rd(mc, kSmmuConfig);
    const uint32_t afiAsid = rd(mc, kSmmuAfiAsid);

    // Pad control is reset-only; read it once out of reset.
    uint32_t padMux = ~0u;
    uint32_t uphy = ~0u;
    if (!(rstW & kWPadctl)) {
        padMux = rd(padctl, kPadMux);
        uphy = rd(padctl, kUphyPll0Ctl1);
    }

    // The AFI needs the partition powered, AFI and PCIE clocked and out of reset; the root port also needs PCIEXCLK
    // released and its own reset (AFI_PEX1_CTRL bit 0) deasserted.
    uint32_t afiConfig = ~0u;
    uint32_t pex1 = ~0u;
    uint32_t rpId = ~0u;
    uint32_t rpLink = ~0u;
    uint32_t rpVend = ~0u;
    const bool afiLive = ((gates & kPartPcie) != 0u) && (clkU & (kUAfi | kUPcie)) == (kUAfi | kUPcie) &&
                         ((rstU & (kUAfi | kUPcie)) == 0u);
    if (afiLive) {
        afiConfig = rd(pcie, kAfi + kAfiPcieConfig);
        pex1 = rd(pcie, kAfi + kAfiPex1Ctrl);
        if (!(rstU & kUPcieX) && (pex1 & kAfiPexRst)) {
            rpId = rd(pcie, kRootPort1);
            rpLink = rd(pcie, kRootPort1 + kRpLinkStatus);
            rpVend = rd(pcie, kRootPort1 + kRpVendXp);
        }
    }

    uint8_t mac[6] = {};
    const bool haveMac = switch_wlan_mac(mac, nullptr) == 0;

    IOLog("SwitchWLAN: pcie part=%u clk afi/pcie=%u%u rst afi/pcie/x=%u%u%u plle en=%u lock=%u pex-dpd=%x "
          "smmu=%x afi-asid=%08x | padmux=%08x uphy0=%08x | afi cfg=%08x pex1=%08x rp1 id=%08x link=%08x vend=%08x | "
          "gpioH out=%02x oe=%02x in=%02x wl_reg_on=%u bt_reg_on=%u pin en/rst=%03x/%03x | uartd clk=%u rst=%u | "
          "mac=%02x:%02x:%02x:%02x:%02x:%02x%s\n",
          flag(gates, kPartPcie), flag(clkU, kUAfi), flag(clkU, kUPcie), flag(rstU, kUAfi), flag(rstU, kUPcie),
          flag(rstU, kUPcieX), flag(plle, kPlleEnable), flag(plleMisc, kPlleLock), (dpd & kDpdPex) >> 4, smmu & 1,
          afiAsid, padMux, uphy, afiConfig, pex1, rpId, rpLink, rpVend, out & 0xFF, oe & 0xFF, in & 0xFF,
          bit(in, kWlRegOn), bit(in, kBtRegOn), rd(pinmux, kPinWifiEn) & 0xFFF, rd(pinmux, kPinWifiRst) & 0xFFF,
          flag(clkU, kUUartD), flag(rstU, kUUartD), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
          haveMac ? "" : " (fuses not visible)");
}
