// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 XUSB host controller hardware layer; see XusbHost.h.
#include "XusbHost.h"
#include "XudcHardware.h"
#include <IOKit/IOLib.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

namespace xhost {
namespace {
// Physical bases: Tegra X1 TRM, hekate bdk/soc/t210.h, the OLED DTB.
constexpr uint64_t kCarPA = 0x60006000, kPmcPagePA = 0x7000E000, kFusePagePA = 0x7000F000, kMcPA = 0x70019000,
                   kPadctlPA = 0x7009F000, kFpciPA = 0x70098000, kIpfsPA = 0x70099000;
constexpr uint32_t kPmcOffset = 0x400;

enum : uint32_t {  // clock and reset controller
  RST_DEV_U_SET = 0x310, RST_DEV_U_CLR = 0x314, CLK_ENB_U_SET = 0x330,
  RST_DEV_W_SET = 0x438, RST_DEV_W_CLR = 0x43c, CLK_ENB_W_SET = 0x448,
  RST_DEV_Y_SET = 0x2a8, RST_DEV_Y_CLR = 0x2ac,   // 0x2a4 is RST_DEVICES_Y itself (TegraPCIe.cpp, SwitchAudio.cpp)
  PLLE_SS_CNTL = 0x068, PLLE_BASE = 0x0e8, PLLE_MISC = 0x0ec, PLLE_AUX = 0x48c, XUSBIO_PLL_CFG0 = 0x51c,
  PLLP_MISC1 = 0x680, UPHY_MGMT = 0x6d0,   // PEX_SATA_USB_RX_BYP, re-purposed on T210B01 (clk-tegra210b01.c)
  CLK_SOURCE_XUSB_CORE_HOST = 0x600, CLK_SOURCE_XUSB_FALCON = 0x604, CLK_SOURCE_XUSB_FS = 0x608,
  CLK_SOURCE_XUSB_SS = 0x610,
};
enum : uint32_t {
  U_XUSB_HOST = 1u << 25, W_XUSB_PADCTL = 1u << 14, W_XUSB = 1u << 15, W_XUSB_SS = 1u << 28,
  Y_UPHY_PEX = 1u << 13, PLLP_MISC1_XUSB_EN = 1u << 28, UPHY_MGMT_ENABLE = 1u << 8,
  PLLE_ENABLE = 1u << 31, PLLE_LOCK_OVERRIDE = 1u << 30,
  PLLE_MISC_LOCK = 1u << 11, PLLE_MISC_LOCK_ENABLE = 1u << 9, PLLE_MISC_IDDQ_SW_CTRL = 1u << 14,
  PLLE_MISC_IDDQ_SW_VALUE = 1u << 13, PLLE_MISC_PTS = 1u << 8, PLLE_MISC_VREG = (3u << 4) | (3u << 2),
  PLLE_AUX_SEQ_ENABLE = 1u << 24, PLLE_AUX_SS_SEQ_INCLUDE = 1u << 31, PLLE_AUX_ENABLE_SWCTL = 1u << 4,
  PLLE_AUX_USE_LOCKDET = 1u << 3, PLLE_AUX_SS_SWCTL = 1u << 6,
  PLLE_SS_INTERP_RESET = 1u << 11, PLLE_SS_BYPASS = 1u << 10, PLLE_SS_SSC_BYP = 1u << 12,
  PLLE_SS_CENTER = 1u << 14, PLLE_SS_INVERT = 1u << 15,
  PLLE_SS_COEFF_MASK = 0x1ffu | (0xffu << 16) | (0x3fu << 24), PLLE_SS_COEFF = 0x25u | (1u << 16) | (0x14u << 24),
  XUSBIO_SEQ_ENABLE = 1u << 24, XUSBIO_PADPLL_SLEEP_IDDQ = 1u << 13, XUSBIO_USE_LOCKDET = 1u << 6,
  XUSBIO_CLK_ENABLE_SWCTL = 1u << 2, XUSBIO_PADPLL_RESET_SWCTL = 1u << 0,
};
enum : uint32_t { PWRGATE_TOGGLE = 0x30, REMOVE_CLAMPING = 0x34, PWRGATE_STATUS = 0x38, USB_AO = 0xf0,
                  TOGGLE_START = 1u << 8, PART_XUSBA = 20, PART_XUSBC = 22 };
enum : uint32_t { MC_SMMU_XUSB_HOST_ASID = 0x288 };   // next to XUSB_DEV's 0x28c (XudcHardware.cpp)

enum : uint32_t {  // FPCI configuration space of the host (xhci-tegra.c)
  XUSB_CFG_1 = 0x004, XUSB_CFG_4 = 0x010, XUSB_CFG_ARU_MBOX_CMD = 0x0e4, XUSB_CFG_ARU_MBOX_DATA_IN = 0x0e8,
  XUSB_CFG_ARU_MBOX_DATA_OUT = 0x0ec, XUSB_CFG_ARU_MBOX_OWNER = 0x0f0, XUSB_CFG_ARU_C11PAGESEL = 0x404,
  XUSB_CFG_ARU_C11_CSBRANGE = 0x41c, XUSB_CFG_ARU_SMI_INTR = 0x428, XUSB_CFG_HSPX_CORE_CTRL = 0x600,
  XUSB_CFG_CSB_BASE_ADDR = 0x800,
  CFG_1_ENABLES = 7, CFG_4_BASE_MASK = 0x1ffffu << 15, HSP0 = 1u << 12, HSIC_PLLCLK_VLD = 1u << 24,
  MBOX_DEST_FALC = 1u << 27, MBOX_DEST_SMI = 1u << 29, MBOX_INT_EN = 1u << 31,
  MBOX_OWNER_NONE = 0, MBOX_OWNER_SW = 2, SMI_INTR_FW_HANG = 1u << 1, SMI_INTR_EN = 1u << 3,
};
enum : uint32_t {  // IPFS
  IPFS_CONFIGURATION = 0x180, IPFS_INTR_MASK = 0x188, IPFS_CLKGATE_HYSTERESIS = 0x1bc,
  IPFS_EN_FPCI = 1u << 0, IPFS_IP_INT_MASK = 1u << 16,
};
enum : uint32_t {  // falcon and its memory pool, through the CSB window (xhci-tegra.c)
  FALC_CPUCTL = 0x100, FALC_BOOTVEC = 0x104, FALC_DMACTL = 0x10c, FALC_IMFILLRNG1 = 0x154, FALC_IMFILLCTL = 0x158,
  CSB_MP_ILOAD_ATTR = 0x101a00, CSB_MP_ILOAD_BASE_LO = 0x101a04, CSB_MP_ILOAD_BASE_HI = 0x101a08,
  CSB_MP_L2IMEMOP_SIZE = 0x101a10, CSB_MP_L2IMEMOP_TRIG = 0x101a14, CSB_MP_L2IMEMOP_RESULT = 0x101a18,
  CSB_MP_APMAP = 0x10181c, CPUCTL_STARTCPU = 1u << 1, APMAP_BOOTPATH = 1u << 31,
  L2IMEMOP_INVALIDATE_ALL = 0x40u << 24, L2IMEMOP_LOAD_LOCKED_RESULT = 0x11u << 24, L2IMEMOP_RESULT_VLD = 1u << 31,
  IMEM_BLOCK = 256,
};
enum : uint32_t {  // XUSB pad controller (xusb-tegra210.c)
  USB2_PAD_MUX = 0x004, USB2_PORT_CAP = 0x008, SS_PORT_MAP = 0x014, ELPG_PROGRAM_1 = 0x024, USB3_PAD_MUX = 0x028,
  BATTERY_CHRG_OTGPAD0_CTL1 = 0x084, OTG_PAD0_CTL_0 = 0x088, UPHY_PLL_P0_CTL1 = 0x360, UPHY_PLL_P0_CTL2 = 0x364,
  UPHY_PLL_P0_CTL4 = 0x36c, UPHY_PLL_P0_CTL8 = 0x37c, UPHY_PLL_P0_CTL10 = 0x384,
  UPHY_MISC_PAD_P5_CTL1 = 0x460 + 5 * 0x40, UPHY_MISC_PAD_P5_CTL8 = 0x47c + 5 * 0x40,
  USB3_PAD0_ECTL1 = 0xa60, USB3_PAD0_ECTL2 = 0xa64, USB3_PAD0_ECTL3 = 0xa68, USB3_PAD0_ECTL4 = 0xa6c,
  USB3_PAD0_ECTL6 = 0xa74, USB2_VBUS_ID = 0xc60,
  PORT0_CAP_MASK = 3u, PORT0_CAP_OTG = 3u, VREG_FIX18 = 1u << 6, OTG_PAD_PD = 1u << 26, OTG_PAD_PD_ZI = 1u << 29,
  SS0_MAP_MASK = 7u, SS0_MAP_USB2_0 = 0u, SS3_MAP_MASK = 7u << 15, SS3_MAP_PORT3 = 3u << 15,
  SSP0_CLAMP_EN = 1u << 0, SSP0_CLAMP_EN_EARLY = 1u << 1, SSP0_VCORE_DOWN = 1u << 2,
  SSP3_CLAMP_EN = 1u << 9, SSP3_CLAMP_EN_EARLY = 1u << 10, SSP3_VCORE_DOWN = 1u << 11,
  AUX_MUX_LP0_CLAMP_EN = 1u << 29, AUX_MUX_LP0_CLAMP_EN_EARLY = 1u << 30, AUX_MUX_LP0_VCORE_DOWN = 1u << 31,
  LANE5_FUNC_MASK = 3u << 22, LANE5_FUNC_XUSB = 1u << 22,      // tegra210b01_pcie_lanes, functions[1] = "xusb"
  PLL_CTL1_ENABLE = 1u << 3, PLL_CTL1_PWR_OVRD = 1u << 4, PLL_CTL1_IDDQ = 1u << 0, PLL_CTL1_SLEEP = 3u << 1,
  PLL_CTL1_LOCKDET = 1u << 15, PLL_CTL1_FREQ_MASK = (0xffu << 20) | (3u << 16), PLL_CTL1_NDIV_USB = 0x19u << 20,
  PLL_CTL2_CAL_EN = 1u << 0, PLL_CTL2_CAL_DONE = 1u << 1, PLL_CTL2_CAL_OVRD = 1u << 2,
  PLL_CTL4_REFCLK_SEL = 0xfu << 4, PLL_CTL4_TXCLKREF_SEL = 3u << 12, PLL_CTL4_TXCLKREF_USB = 2u << 12,
  PLL_CTL4_TXCLKREF_EN = 1u << 15, PLL_CTL4_REFCLKBUF_EN = 1u << 8,
  PLL_CTL8_RCAL_EN = 1u << 12, PLL_CTL8_RCAL_CLK_EN = 1u << 13, PLL_CTL8_RCAL_OVRD = 1u << 15,
  PLL_CTL8_RCAL_DONE = 1u << 31, CFG_RESET = 1u << 27, CFG_WS = 1u << 24,
  AUX_RX_MODE_OVRD = 1u << 13, AUX_RX_TERM_EN = 1u << 18, AUX_RX_IDLE_MODE_MASK = 3u << 20, AUX_RX_IDLE_MODE_1 = 1u << 20,
  VBUS_ID_VBUS_ON = 1u << 14, VBUS_ID_OVR_MASK = 0xfu << 18, VBUS_ID_OVR_GROUNDED = 0u << 18,
  VBUS_ID_OVR_FLOATING = 8u << 18, VBUS_ID_SRC_OVR = (1u << 12) | (1u << 16),
};
// T210B01 UPHY programming (xusb-tegra210.c usb3_pll_g1_init_data, usb3_lane_data): {address, data}.
constexpr uint16_t kPllInit[][2] = { {0x02, 0x0000}, {0x03, 0x7051}, {0x25, 0x0130}, {0x1e, 0x0017} };
constexpr uint16_t kUsb3Lane[][2] = {
  {0x01, 0x0002}, {0x04, 0x0032}, {0x07, 0x0022}, {0x35, 0x2587}, {0x49, 0x0fc7}, {0x52, 0x0001},
  {0x53, 0x3c0f}, {0x56, 0xc00f}, {0x5d, 0xff07}, {0x5e, 0x141a}, {0x97, 0x0080},
};

// Firmware header (xhci-tegra.c struct tegra_xusb_fw_header, 256 bytes before the image proper).
constexpr size_t kFwHeader = 256, kFwCodeTag = 8, kFwCodeSize = 12, kFwCreated = 44, kFwVersion = 72, kFwLength = 100,
                 kFwHsicPorts = 116;

inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// The firmware image stays where the falcon reads it (ILOAD_BASE) for as long as the controller runs.
xudc::DmaBuffer gFirmware;
}

bool Controller::waitBits(volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t value, uint32_t us)
{
  for (uint32_t i = 0; i <= us; ++i) {
    if ((r(b, o) & mask) == value) return true;
    IODelay(1);
  }
  return false;
}

uint32_t Controller::pmc(uint32_t o) const { return r(pmcPage, kPmcOffset + o); }
void Controller::pmcWrite(uint32_t o, uint32_t v) { w(pmcPage, kPmcOffset + o, v); }

bool Controller::map()
{
  car = (volatile uint8_t *)ml_io_map(kCarPA, 0x1000);
  pmcPage = (volatile uint8_t *)ml_io_map(kPmcPagePA, 0x1000);
  fusePage = (volatile uint8_t *)ml_io_map(kFusePagePA, 0x1000);
  mc = (volatile uint8_t *)ml_io_map(kMcPA, 0x1000);
  padctl = (volatile uint8_t *)ml_io_map(kPadctlPA, 0x1000);
  regs = (volatile uint8_t *)ml_io_map(kXhciPA, kXhciBytes);
  fpci = (volatile uint8_t *)ml_io_map(kFpciPA, 0x1000);
  ipfs = (volatile uint8_t *)ml_io_map(kIpfsPA, 0x1000);
  return car && pmcPage && fusePage && mc && padctl && regs && fpci && ipfs;
}

bool Controller::smmuBypassed(char *why, size_t size)
{
  uint32_t asid = r(mc, MC_SMMU_XUSB_HOST_ASID);
  if (!(asid & 0x80000000u)) return true;
  snprintf(why, size, "SMMU translation is on for XUSB_HOST (MC 0x%x = %#x)", MC_SMMU_XUSB_HOST_ASID, asid);
  return false;
}

// Linux tegra_powergate_power_up order (as XudcHardware::powerUp): reset asserted, partition on,
// module clock on, clamps removed. The caller releases the reset.
bool Controller::powerUp(uint32_t partition, uint32_t clockSet, uint32_t resetSet, uint32_t bit)
{
  if (pmc(PWRGATE_STATUS) & (1u << partition)) return true;
  w(car, resetSet, bit);
  if (!waitBits(pmcPage, kPmcOffset + PWRGATE_TOGGLE, TOGGLE_START, 0, 5000)) return false;
  pmcWrite(PWRGATE_TOGGLE, partition | TOGGLE_START);
  if (!waitBits(pmcPage, kPmcOffset + PWRGATE_STATUS, 1u << partition, 1u << partition, 5000)) return false;
  IODelay(10);
  w(car, clockSet, bit);
  IODelay(10);
  pmcWrite(REMOVE_CLAMPING, 1u << partition);
  IODelay(10);
  return true;
}

// tegra_xusb_probe order: PHYs (USB2 pad 0 shared with the XUDC; its PLLs and calibration are the
// same, xudc::usb2PadInit), clocks, partitions SS (XUSBA) and host (XUSBC), then tegra_xusb_config
// (IPFS -> FPCI, BAR0, bus master, interrupt routing) and tegra_xusb_disable_hsic_wake (T210B01).
bool Controller::powerOn()
{
  w(car, CLK_ENB_W_SET, W_XUSB);
  w(car, RST_DEV_W_CLR, W_XUSB_PADCTL);
  IODelay(2);
  // Bias pad and OTG pad 0 belong to XUSB (hekate/Linux USB2_PAD_MUX), then the shared pad bring-up.
  rmw(padctl, USB2_PAD_MUX, 3u << 18 | 3u, 1u << 18 | 1u);
  // Skipped when the XUDC already powered the pad (PD and PD_ZI clear): re-running its bias tracking
  // under a live device link would glitch the Mac's connection for nothing.
  if ((r(padctl, OTG_PAD0_CTL_0) & (OTG_PAD_PD | OTG_PAD_PD_ZI)) && !xudc::usb2PadInit(car, fusePage, padctl)) return false;
  // T210B01 uphy_init "disable pad protection": fixed 1.8 V regulator on the USB2 pads.
  rmw(padctl, BATTERY_CHRG_OTGPAD0_CTL1, 0, VREG_FIX18);
  pmcWrite(USB_AO, pmc(USB_AO) & 0xfffffff3u);  // ID and VBUS wake detectors powered (as the XUDC)

  // Host core 102 MHz and falcon 204 MHz from PLLP, FS 48 MHz from PLLU FO, SS 120 MHz from the HSIC
  // 480 MHz branch: the rates of the L4T device tree's assigned-clock-rates, sources as XudcHardware.
  rmw(car, PLLP_MISC1, 0, PLLP_MISC1_XUSB_EN);
  w(car, CLK_SOURCE_XUSB_CORE_HOST, (r(car, CLK_SOURCE_XUSB_CORE_HOST) & 0x1fffff00u) | 1u << 29 | 6);
  w(car, CLK_SOURCE_XUSB_FALCON, (r(car, CLK_SOURCE_XUSB_FALCON) & 0x1fffff00u) | 1u << 29 | 2);
  w(car, CLK_SOURCE_XUSB_FS, (r(car, CLK_SOURCE_XUSB_FS) & 0x1fffffffu) | 2u << 29);
  w(car, CLK_SOURCE_XUSB_SS, (r(car, CLK_SOURCE_XUSB_SS) & 0x1fffff00u) | 3u << 29 | 6);
  IODelay(2);
  if (!powerUp(PART_XUSBA, CLK_ENB_W_SET, RST_DEV_W_SET, W_XUSB_SS) ||
      !powerUp(PART_XUSBC, CLK_ENB_U_SET, RST_DEV_U_SET, U_XUSB_HOST)) {
    IOLog("SwitchXHCI: XUSB partition power-up failed (PWRGATE_STATUS %#x)\n", pmc(PWRGATE_STATUS));
    return false;
  }
  w(car, CLK_ENB_W_SET, W_XUSB_SS);
  w(car, CLK_ENB_U_SET, U_XUSB_HOST);
  w(car, RST_DEV_W_CLR, W_XUSB_SS);
  w(car, RST_DEV_U_CLR, U_XUSB_HOST);
  IODelay(2);

  rmw(ipfs, IPFS_CONFIGURATION, 0, IPFS_EN_FPCI);
  IODelay(10);
  rmw(fpci, XUSB_CFG_4, CFG_4_BASE_MASK, (uint32_t)kXhciPA & CFG_4_BASE_MASK);
  IODelay(100);
  rmw(fpci, XUSB_CFG_1, 0, CFG_1_ENABLES);
  rmw(ipfs, IPFS_INTR_MASK, 0, IPFS_IP_INT_MASK);
  w(ipfs, IPFS_CLKGATE_HYSTERESIS, 0x80);

  rmw(fpci, XUSB_CFG_ARU_C11PAGESEL, 0, HSP0);
  rmw(fpci, XUSB_CFG_HSPX_CORE_CTRL, HSIC_PLLCLK_VLD, 0);
  rmw(fpci, XUSB_CFG_ARU_C11PAGESEL, HSP0, 0);
  return true;
}

// The CSB window: page select in C11_CSBRANGE, 512-byte page at CSB_BASE_ADDR (xhci-tegra.c csb_readl).
uint32_t Controller::csbRead(uint32_t offset)
{
  w(fpci, XUSB_CFG_ARU_C11_CSBRANGE, (offset >> 9) & 0x7fffff);
  return r(fpci, XUSB_CFG_CSB_BASE_ADDR + (offset & 0x1ff));
}

void Controller::csbWrite(uint32_t offset, uint32_t value)
{
  w(fpci, XUSB_CFG_ARU_C11_CSBRANGE, (offset >> 9) & 0x7fffff);
  w(fpci, XUSB_CFG_CSB_BASE_ADDR + (offset & 0x1ff), value);
}

// tegra_xhci_load_firmware: the falcon's boot code is DMAed from system memory into L2IMEM, the
// falcon starts at boot_codetag and, once it runs the xHCI, clears USBSTS.CNR. A controller whose
// ILOAD_BASE is already set kept its firmware (we never reset it after this).
bool Controller::loadFirmware(const uint8_t *image, size_t size, char *why, size_t whySize)
{
  if (csbRead(CSB_MP_ILOAD_BASE_LO) != 0) return true;
  if (!image || size < kFwHeader) { snprintf(why, whySize, "no XUSB firmware linked (tegra210b01_xusb_firmware)"); return false; }
  uint32_t length = le32(image + kFwLength);
  if (length < kFwHeader || length > size) {
    snprintf(why, whySize, "XUSB firmware header says %u bytes, image has %zu", length, size);
    return false;
  }
  if (!gFirmware.va && !gFirmware.allocate((length + 0xfffu) & ~0xfffu)) { snprintf(why, whySize, "no DMA memory for the firmware"); return false; }
  memcpy(gFirmware.va, image, length);
  gFirmware.va[kFwHsicPorts] = 0;           // header->num_hsic_port: no HSIC on this board
  xudc::cacheFlush(gFirmware.va, length);

  uint32_t codeTag = le32(image + kFwCodeTag), codeSize = le32(image + kFwCodeSize);
  uint32_t tagBlocks = (codeTag + IMEM_BLOCK - 1) / IMEM_BLOCK, sizeBlocks = (codeSize + IMEM_BLOCK - 1) / IMEM_BLOCK;
  uint64_t base = (uint64_t)gFirmware.pa + kFwHeader;
  csbWrite(CSB_MP_ILOAD_ATTR, length);
  csbWrite(CSB_MP_ILOAD_BASE_HI, (uint32_t)(base >> 32));
  csbWrite(CSB_MP_ILOAD_BASE_LO, (uint32_t)base);
  csbWrite(CSB_MP_APMAP, APMAP_BOOTPATH);
  csbWrite(CSB_MP_L2IMEMOP_TRIG, L2IMEMOP_INVALIDATE_ALL);
  csbWrite(CSB_MP_L2IMEMOP_SIZE, (tagBlocks & 0x3ff) << 8 | (sizeBlocks & 0xff) << 24);
  csbWrite(CSB_MP_L2IMEMOP_TRIG, L2IMEMOP_LOAD_LOCKED_RESULT);
  csbWrite(FALC_IMFILLCTL, sizeBlocks);
  csbWrite(FALC_IMFILLRNG1, (tagBlocks & 0xffff) | ((tagBlocks + sizeBlocks) & 0xffff) << 16);
  csbWrite(FALC_DMACTL, 0);
  bool loaded = false;
  for (int i = 0; i < 200 && !loaded; ++i) { loaded = csbRead(CSB_MP_L2IMEMOP_RESULT) & L2IMEMOP_RESULT_VLD; if (!loaded) IODelay(50); }
  if (!loaded) { snprintf(why, whySize, "falcon DMA not ready (L2IMEMOP_RESULT %#x)", csbRead(CSB_MP_L2IMEMOP_RESULT)); return false; }
  csbWrite(FALC_BOOTVEC, codeTag);
  csbWrite(FALC_CPUCTL, CPUCTL_STARTCPU);
  const uint32_t usbsts = capLength() + 0x04;
  for (int i = 0; i < 200 && (xhci(usbsts) & (1u << 11)); ++i) IOSleep(1);   // STS_CNR
  if (xhci(usbsts) & (1u << 11)) {
    snprintf(why, whySize, "xHCI not ready after firmware start (USBSTS %#x, falcon CPUCTL %#x)", xhci(usbsts), csbRead(FALC_CPUCTL));
    return false;
  }
  IOLog("SwitchXHCI: XUSB firmware %x.%02x (built %u) running, HCSPARAMS1 %#x\n", image[kFwVersion + 3],
        image[kFwVersion + 2], le32(image + kFwCreated), xhci(4));
  return true;
}

// Linux clk_plle_tegra210_enable (nxk user/clk plle(), same values): software control, out of IDDQ,
// dividers with spread spectrum bypassed (38.4 MHz / 2 * 125 / CML 14 = 100 MHz), lock, spread spectrum on.
bool Controller::plleUp()
{
  if (r(car, PLLE_AUX) & PLLE_AUX_SEQ_ENABLE) return true;     // already under the hardware sequencer
  if (!(r(car, PLLE_BASE) & PLLE_ENABLE)) {
    rmw(car, PLLE_BASE, PLLE_LOCK_OVERRIDE, 0);
    rmw(car, PLLE_AUX, PLLE_AUX_SEQ_ENABLE, PLLE_AUX_ENABLE_SWCTL | PLLE_AUX_SS_SWCTL);
    IODelay(1);
    rmw(car, PLLE_MISC, PLLE_MISC_IDDQ_SW_VALUE, PLLE_MISC_IDDQ_SW_CTRL | PLLE_MISC_LOCK_ENABLE | PLLE_MISC_PTS | PLLE_MISC_VREG);
    IODelay(5);
    rmw(car, PLLE_SS_CNTL, 0, PLLE_SS_BYPASS | PLLE_SS_INTERP_RESET | PLLE_SS_SSC_BYP);
    const uint32_t base = 14u << 24 | 125u << 8 | 2u;
    w(car, PLLE_BASE, base);
    IODelay(1);
    w(car, PLLE_BASE, base | PLLE_ENABLE);
    if (!waitBits(car, PLLE_MISC, PLLE_MISC_LOCK, PLLE_MISC_LOCK, 1000)) { IOLog("SwitchXHCI: PLLE did not lock\n"); return false; }
    uint32_t ss = (r(car, PLLE_SS_CNTL) & ~(PLLE_SS_CENTER | PLLE_SS_INVERT | PLLE_SS_COEFF_MASK)) | PLLE_SS_COEFF;
    w(car, PLLE_SS_CNTL, ss);
    ss &= ~(PLLE_SS_SSC_BYP | PLLE_SS_BYPASS);
    w(car, PLLE_SS_CNTL, ss);
    IODelay(1);
    w(car, PLLE_SS_CNTL, ss & ~PLLE_SS_INTERP_RESET);
    IODelay(1);
    rmw(car, PLLE_AUX, 0, PLLE_AUX_USE_LOCKDET);
  }
  return true;
}

void Controller::laneConfig(uint32_t ctl8, const uint16_t (*data)[2], unsigned count)
{
  for (unsigned i = 0; i < count; ++i) w(padctl, ctl8, (uint32_t)data[i][0] << 16 | data[i][1] | CFG_RESET | CFG_WS);
}

// SuperSpeed port 0 on UPHY lane pcie-5 (tegra210b01_usb3_map). xusb-tegra210.c tegra210_uphy_init with
// the T210B01 branches: PLLE, UPHY management clock (102 MHz, PEX_SATA_USB_RX_BYP), PEX PLL P0 brought up
// through its CTL10 init data and calibrated, handed to the hardware sequencers, unused port 3 mapped
// for power saving, pads out of IDDQ, AUX mux clamps off; then tegra210_pcie_phy_power_on for the lane:
// SS_PORT_MAP 0 -> USB2 port 0, the ECTL equalizer values, the SSP0 ELPG clamps released. The PLL is
// shared with the PCIe lane of the Wi-Fi: when its sequencer already runs, the PLL part is skipped.
bool Controller::superSpeedUp()
{
  if (!plleUp()) return false;
  rmw(car, UPHY_MGMT, 0xffu, 6u);                  // pll_p_out_xusb 408 MHz / 4 (7.1 divider)
  rmw(car, UPHY_MGMT, 0, UPHY_MGMT_ENABLE);
  w(car, RST_DEV_Y_CLR, Y_UPHY_PEX);
  rmw(padctl, USB3_PAD_MUX, LANE5_FUNC_MASK, LANE5_FUNC_XUSB);
  laneConfig(UPHY_MISC_PAD_P5_CTL8, kUsb3Lane, sizeof(kUsb3Lane) / sizeof(kUsb3Lane[0]));

  if (!(r(car, XUSBIO_PLL_CFG0) & XUSBIO_SEQ_ENABLE)) {
    laneConfig(UPHY_PLL_P0_CTL10, kPllInit, sizeof(kPllInit) / sizeof(kPllInit[0]));
    rmw(padctl, UPHY_PLL_P0_CTL1, 0, PLL_CTL1_PWR_OVRD);
    rmw(padctl, UPHY_PLL_P0_CTL2, 0, PLL_CTL2_CAL_OVRD);
    rmw(padctl, UPHY_PLL_P0_CTL8, 0, PLL_CTL8_RCAL_OVRD);
    rmw(padctl, UPHY_PLL_P0_CTL4, PLL_CTL4_TXCLKREF_SEL | PLL_CTL4_REFCLK_SEL, PLL_CTL4_TXCLKREF_USB | PLL_CTL4_TXCLKREF_EN);
    rmw(padctl, UPHY_PLL_P0_CTL1, PLL_CTL1_FREQ_MASK, PLL_CTL1_NDIV_USB);
    rmw(padctl, UPHY_PLL_P0_CTL1, PLL_CTL1_IDDQ, 0);
    rmw(padctl, UPHY_PLL_P0_CTL1, PLL_CTL1_SLEEP, 0);
    IODelay(20);
    rmw(padctl, UPHY_PLL_P0_CTL4, 0, PLL_CTL4_REFCLKBUF_EN);
    rmw(padctl, UPHY_PLL_P0_CTL2, 0, PLL_CTL2_CAL_EN);
    bool ok = waitBits(padctl, UPHY_PLL_P0_CTL2, PLL_CTL2_CAL_DONE, PLL_CTL2_CAL_DONE, 100000);
    rmw(padctl, UPHY_PLL_P0_CTL2, PLL_CTL2_CAL_EN, 0);
    ok = ok && waitBits(padctl, UPHY_PLL_P0_CTL2, PLL_CTL2_CAL_DONE, 0, 100000);
    rmw(padctl, UPHY_PLL_P0_CTL1, 0, PLL_CTL1_ENABLE);
    ok = ok && waitBits(padctl, UPHY_PLL_P0_CTL1, PLL_CTL1_LOCKDET, PLL_CTL1_LOCKDET, 100000);
    rmw(padctl, UPHY_PLL_P0_CTL8, 0, PLL_CTL8_RCAL_EN | PLL_CTL8_RCAL_CLK_EN);
    ok = ok && waitBits(padctl, UPHY_PLL_P0_CTL8, PLL_CTL8_RCAL_DONE, PLL_CTL8_RCAL_DONE, 100000);
    rmw(padctl, UPHY_PLL_P0_CTL8, PLL_CTL8_RCAL_EN, 0);
    ok = ok && waitBits(padctl, UPHY_PLL_P0_CTL8, PLL_CTL8_RCAL_DONE, 0, 100000);
    rmw(padctl, UPHY_PLL_P0_CTL8, PLL_CTL8_RCAL_CLK_EN, 0);
    if (!ok) {
      IOLog("SwitchXHCI: UPHY PLL P0 calibration/lock failed (CTL1 %#x CTL2 %#x CTL8 %#x)\n",
            r(padctl, UPHY_PLL_P0_CTL1), r(padctl, UPHY_PLL_P0_CTL2), r(padctl, UPHY_PLL_P0_CTL8));
      return false;
    }
    // tegra210_xusb_pll_hw_control_enable, then the overrides off and the sequencer on.
    rmw(car, XUSBIO_PLL_CFG0, XUSBIO_CLK_ENABLE_SWCTL | XUSBIO_PADPLL_RESET_SWCTL, XUSBIO_USE_LOCKDET | XUSBIO_PADPLL_SLEEP_IDDQ);
    rmw(padctl, UPHY_PLL_P0_CTL1, PLL_CTL1_PWR_OVRD, 0);
    rmw(padctl, UPHY_PLL_P0_CTL2, PLL_CTL2_CAL_OVRD, 0);
    rmw(padctl, UPHY_PLL_P0_CTL8, PLL_CTL8_RCAL_OVRD, 0);
    IODelay(20);
    rmw(car, XUSBIO_PLL_CFG0, 0, XUSBIO_SEQ_ENABLE);
  }
  // tegra210_plle_hw_sequence_start.
  if (!(r(car, PLLE_AUX) & PLLE_AUX_SEQ_ENABLE)) {
    rmw(car, PLLE_MISC, PLLE_MISC_IDDQ_SW_CTRL, 0);
    rmw(car, PLLE_AUX, PLLE_AUX_ENABLE_SWCTL | PLLE_AUX_SS_SWCTL, PLLE_AUX_USE_LOCKDET | PLLE_AUX_SS_SEQ_INCLUDE);
    IODelay(1);
    rmw(car, PLLE_AUX, 0, PLLE_AUX_SEQ_ENABLE);
  }
  // Unused SS port 3 mapped and unclamped (tegra210_xusb_padctl_init_usb3_port(3)), lane 5 out of IDDQ.
  rmw(padctl, SS_PORT_MAP, SS3_MAP_MASK, SS3_MAP_PORT3);
  rmw(padctl, ELPG_PROGRAM_1, SSP3_VCORE_DOWN, 0);
  rmw(padctl, ELPG_PROGRAM_1, SSP3_CLAMP_EN_EARLY, 0);
  rmw(padctl, ELPG_PROGRAM_1, SSP3_CLAMP_EN, 0);
  rmw(padctl, USB3_PAD_MUX, 0, 1u << (1 + 5));     // FORCE_PCIE_PAD_IDDQ_DISABLE(5)
  rmw(padctl, ELPG_PROGRAM_1, AUX_MUX_LP0_CLAMP_EN, 0);
  IODelay(150);
  rmw(padctl, ELPG_PROGRAM_1, AUX_MUX_LP0_CLAMP_EN_EARLY, 0);
  IODelay(150);
  rmw(padctl, ELPG_PROGRAM_1, AUX_MUX_LP0_VCORE_DOWN, 0);

  // tegra210_pcie_phy_power_on (xusb lane, port 0): map, equalizers (DT prod_c_ss0 asic value is the
  // same TX_TERM 2), clamps.
  rmw(padctl, SS_PORT_MAP, SS0_MAP_MASK, SS0_MAP_USB2_0);
  rmw(padctl, USB3_PAD0_ECTL1, 3u << 16, 2u << 16);
  rmw(padctl, USB3_PAD0_ECTL2, 0xffffu, 0x00fc);
  w(padctl, USB3_PAD0_ECTL3, 0xc0077f1f);
  rmw(padctl, USB3_PAD0_ECTL4, 0xffffu << 16, 0x01c7u << 16);
  w(padctl, USB3_PAD0_ECTL6, 0xfcf01368);
  rmw(padctl, ELPG_PROGRAM_1, SSP0_VCORE_DOWN, 0);
  IODelay(150);
  rmw(padctl, ELPG_PROGRAM_1, SSP0_CLAMP_EN_EARLY, 0);
  IODelay(150);
  rmw(padctl, ELPG_PROGRAM_1, SSP0_CLAMP_EN, 0);
  return true;
}

// DT usb2-0 nvidia,usb3-port-fake = 0: without SuperSpeed the SS port 0 is still mapped to USB2 port 0
// and unclamped (tegra210_usb2_phy_power_on), so the controller's port pairing stays as the firmware expects.
void Controller::mapFakeSuperSpeedPort()
{
  rmw(padctl, SS_PORT_MAP, SS0_MAP_MASK, SS0_MAP_USB2_0);
  rmw(padctl, ELPG_PROGRAM_1, SSP0_VCORE_DOWN, 0);
  IODelay(150);
  rmw(padctl, ELPG_PROGRAM_1, SSP0_CLAMP_EN_EARLY, 0);
  IODelay(150);
  rmw(padctl, ELPG_PROGRAM_1, SSP0_CLAMP_EN, 0);
}

// tegra210_usb3_set_lfps_detect for the lane of SS port 0 (pcie-5).
void Controller::setLfpsDetect(bool enable)
{
  uint32_t v = r(padctl, UPHY_MISC_PAD_P5_CTL1) & ~(AUX_RX_IDLE_MODE_MASK | AUX_RX_TERM_EN | AUX_RX_MODE_OVRD);
  if (!enable) v |= AUX_RX_IDLE_MODE_1 | AUX_RX_TERM_EN | AUX_RX_MODE_OVRD;
  w(padctl, UPHY_MISC_PAD_P5_CTL1, v);
}

// OTG capability, then the overrides (tegra210_xusb_padctl_id_override / vbus_override). Host: VBUS
// override off first (Linux waits 1-2 ms before grounding ID), ID grounded. Device: ID floating, VBUS
// override on, which is what XudcHardware::connect() writes too.
void Controller::setPortRole(PortRole role)
{
  rmw(padctl, USB2_PORT_CAP, PORT0_CAP_MASK, PORT0_CAP_OTG);
  uint32_t v = r(padctl, USB2_VBUS_ID) | VBUS_ID_SRC_OVR;
  if (role == PortRole::Host) {
    if (v & VBUS_ID_VBUS_ON) { w(padctl, USB2_VBUS_ID, v & ~VBUS_ID_VBUS_ON); IODelay(1500); v &= ~VBUS_ID_VBUS_ON; }
    v = (v & ~VBUS_ID_OVR_MASK) | VBUS_ID_OVR_GROUNDED;
  } else {
    v = (v & ~VBUS_ID_OVR_MASK) | VBUS_ID_OVR_FLOATING | VBUS_ID_VBUS_ON;
  }
  w(padctl, USB2_VBUS_ID, v);
}

bool Controller::takeMailboxInterrupt()
{
  uint32_t smi = r(fpci, XUSB_CFG_ARU_SMI_INTR);
  if (!(smi & (SMI_INTR_EN | SMI_INTR_FW_HANG))) return false;
  w(fpci, XUSB_CFG_ARU_SMI_INTR, smi);
  (void)r(fpci, XUSB_CFG_ARU_SMI_INTR);       // Linux reads again against a spurious ARU SMI
  if (smi & SMI_INTR_FW_HANG) mailboxHang = true;
  return true;
}

uint32_t Controller::mailboxPending() const { return r(fpci, XUSB_CFG_ARU_SMI_INTR) & (SMI_INTR_EN | SMI_INTR_FW_HANG); }

// tegra_xusb_mbox_send: only ACK/NAK go out while the firmware still owns the mailbox; anything else
// takes ownership first and waits (bounded) until the firmware has read it.
bool Controller::sendMailbox(uint32_t cmd, uint32_t data)
{
  const bool reply = cmd == MBOX_ACK || cmd == MBOX_NAK;
  if (!reply) {
    if (r(fpci, XUSB_CFG_ARU_MBOX_OWNER) != MBOX_OWNER_NONE) return false;
    w(fpci, XUSB_CFG_ARU_MBOX_OWNER, MBOX_OWNER_SW);
    if (r(fpci, XUSB_CFG_ARU_MBOX_OWNER) != MBOX_OWNER_SW) return false;
  }
  w(fpci, XUSB_CFG_ARU_MBOX_DATA_IN, (cmd & 0xff) << 24 | (data & 0xffffff));
  rmw(fpci, XUSB_CFG_ARU_MBOX_CMD, 0, MBOX_INT_EN | MBOX_DEST_FALC);
  if (reply) return true;
  for (int i = 0; i < 250; ++i) {
    if (r(fpci, XUSB_CFG_ARU_MBOX_OWNER) == MBOX_OWNER_NONE) return true;
    IOSleep(1);
  }
  return false;
}

// tegra_xusb_mbox_thread + tegra_xusb_mbox_handle, T210B01 (no SS clock scaling, no HSIC): the falcon
// clock is fixed (ACK only when it asks for the rate it has), SSPI scaling is acknowledged as is,
// SET_BW needs no answer, LFPS detection is switched on the SS port 0 lane.
bool Controller::serviceMailbox(uint32_t falconKHz)
{
  if (mailboxHang) return false;
  uint32_t message = r(fpci, XUSB_CFG_ARU_MBOX_DATA_OUT), cmd = message >> 24, data = message & 0xffffff;
  rmw(fpci, XUSB_CFG_ARU_MBOX_CMD, MBOX_DEST_SMI, 0);
  if (cmd == MBOX_SET_BW || cmd == MBOX_ACK || cmd == MBOX_NAK) {
    w(fpci, XUSB_CFG_ARU_MBOX_OWNER, MBOX_OWNER_NONE);
    return true;
  }
  uint32_t reply = MBOX_ACK, replyData = data;
  switch (cmd) {
  case MBOX_INC_FALC_CLOCK: case MBOX_DEC_FALC_CLOCK:
    replyData = falconKHz;
    reply = data == falconKHz ? MBOX_ACK : MBOX_NAK;
    break;
  case MBOX_INC_SSPI_CLOCK: case MBOX_DEC_SSPI_CLOCK: case MBOX_SAVE_DFE_CTLE_CTX:
  case MBOX_START_HSIC_IDLE: case MBOX_STOP_HSIC_IDLE:
    break;
  case MBOX_DISABLE_SS_LFPS_DETECTION: case MBOX_ENABLE_SS_LFPS_DETECTION:
    // data bit (1 + port): SS ports are xHCI ports 1-4 (tegra210b01_soc ports.usb3.offset 0).
    if (data & (1u << 1)) setLfpsDetect(cmd == MBOX_ENABLE_SS_LFPS_DETECTION);
    if (cmd == MBOX_DISABLE_SS_LFPS_DETECTION) IODelay(750);   // T210: "stability of directing U3"
    break;
  default:
    IOLog("SwitchXHCI: unknown falcon mailbox message %#x\n", message);
    reply = MBOX_NAK;
    break;
  }
  if (!sendMailbox(reply, replyData)) IOLog("SwitchXHCI: falcon mailbox reply %u to %u failed\n", reply, cmd);
  return true;
}

void Controller::describe(char *buffer, size_t size) const
{
  const uint32_t op = capLength(), rt = runtimeOffset();
  snprintf(buffer, size, "USBSTS %#x USBCMD %#x IMAN0 %#x ERDP0 %#x SMI %#x MBOX_OWNER %#x IPFS_INTR_MASK %#x",
           xhci(op + 4), xhci(op), xhci(rt + 0x20), xhci(rt + 0x38), r(fpci, XUSB_CFG_ARU_SMI_INTR),
           r(fpci, XUSB_CFG_ARU_MBOX_OWNER), r(ipfs, IPFS_INTR_MASK));
}

}  // namespace xhost
