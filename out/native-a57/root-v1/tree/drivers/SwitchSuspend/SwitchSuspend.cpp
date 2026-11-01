// SPDX-License-Identifier: BSD-2-Clause-Patent
// System sleep (SC7) coordinator for the Switch OLED: see SwitchSuspend.h for the path and the order.
//
// Firmware (feasibility, switchroot ATF = CTCaer/switch-l4t-atf, the bl31.bin this boot chain uses):
//   * plat_supports_system_suspend() is true on T210B01 (soc/t210/plat_setup.c), so PSCI 1.0 SYSTEM_SUSPEND is
//     offered; it needs every other core OFF (lib/psci psci_system_suspend -> PSCI_E_DENIED otherwise).
//   * SC7 entry is the BPMP firmware's (tegra_bpmp_init sees its SIGN_OF_LIFE: hekate's l4t.c starts
//     bootloader/sys/l4t/bpmpfw_b01.bin with mtc_tbl_b01.bin on the COP; it refuses to launch L4T without them),
//     exit is the bootrom running hekate's warmboot (sc7exit_b01.bin, matched to the burnt fuses; hekate warns
//     "Failed to match warmboot with fuses! If you continue, sleep wont work!" at boot otherwise).
//   * ATF resumes the boot CPU at the SYSTEM_SUSPEND entry: physical address, MMU and caches off, at EL2 (BL33 was
//     entered in EL2, so SCR_EL3.HCE is set: lib/psci psci_get_ns_ep_info), x0 = context id, GIC distributor and
//     CPU interface re-initialized by ATF (tegra_gic_init), memory controller restored, CNTFRQ written.
// The PMC (always-on, non-secure on B01: hekate FLAGS_PMC_NON_SECURE) keeps its registers across SC7; the GIC,
// legacy interrupt controller, GPIO controller, pinmux and every clock/reset come back at their reset values.
//
// PMC as switchroot L4T's pmc.c sets it up for this DT (tegra20_pmc_init, tegra210_pmc_irq_set_wake/_set_type;
// OLED DTB pmc@7000e400: core-pwr-good-time 13100/7800 us, core-pwr-off-time 44160 us, core-pwr-req-active-high,
// sys-clock-req-active-high; max77620 nvidia,pmc-wakeup = wake 51 level low, pwr_int_n).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include "SwitchSuspend.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

// The hooks (strong: the kernel link takes no undefined weak references; every one below is in a linked driver,
// tegra_i2c_* once sc7/TegraI2C-suspend.diff is in). NVDEC/host1x has none yet: sleep is held off while it runs.
extern "C" {
int armcpu_system_sleep_enable(char *why, size_t size);
void arm_gic_suspend(void);
void arm_gic_resume(void);
void tegra_gpio_suspend(void);
void tegra_gpio_resume(void);
void tegra_i2c_suspend(void);
void tegra_i2c_resume(void);
void ioswitch_block_suspend(void);
void ioswitch_block_resume(void);
void switch_thermal_suspend(void);
void switch_thermal_resume(void);
void switch_rtc_sleep_enable(void);
void switch_rtc_suspend(void);
int switch_rtc_resume(void);  // 1 when ALARM1 is what woke the console
void switch_touch_suspend(void);
void switch_touch_resume(void);
// Owned by the display and controllers agents (NativeMobileDisplay/TegraDisplayPower.h, SwitchJoyCon/SwitchJoyCon.h).
void tegra_display_suspend(void);
void tegra_display_resume(void);
void switch_joycon_suspend(void);
void switch_joycon_resume(void);
}

namespace {
constexpr vm_offset_t kPmcPagePA = 0x7000E000, kApbMiscPA = 0x70000000;
constexpr uint32_t kPmc = 0x400;  // PMC registers within their page
enum : uint32_t {
    PMC_CNTRL = kPmc + 0x000,
    PMC_WAKE_MASK = kPmc + 0x00c,
    PMC_WAKE_LVL = kPmc + 0x010,
    PMC_WAKE_STATUS = kPmc + 0x014,
    PMC_SW_WAKE_STATUS = kPmc + 0x018,
    PMC_COREPWRGOOD_TIMER = kPmc + 0x03c,
    PMC_COREPWROFF_TIMER = kPmc + 0x0e0,
    PMC_WAKE2_MASK = kPmc + 0x160,
    PMC_WAKE2_LVL = kPmc + 0x164,
    PMC_WAKE2_STATUS = kPmc + 0x168,
    PMC_SW_WAKE2_STATUS = kPmc + 0x16c,
};
enum : uint32_t {
    CNTRL_PWRREQ_POLARITY = 1u << 8,
    CNTRL_SYSCLK_POLARITY = 1u << 10,
    CNTRL_SYSCLK_OE = 1u << 11,
    CNTRL_SIDE_EFFECT_LP0 = 1u << 14,
    CNTRL_CPU_PWRREQ_OE = 1u << 16,
};
constexpr uint32_t kPmicWake2Bit = 1u << (51 - 32);  // wake 51, pwr_int_n (MAX77620 nIRQ), level low
// DIV_ROUND_UP(13100 us * 8192 Hz), DIV_ROUND_UP(7800 us * 32768 Hz) & 0xff, DIV_ROUND_UP(44160 us * 32768 Hz).
constexpr uint32_t kCoreOsc = (13100u * 8192u + 999999u) / 1000000u, kCorePmu = (7800u * 32768u + 999999u) / 1000000u;
constexpr uint32_t kCorePowerGood = ((kCoreOsc << 8) & 0xff00u) | (kCorePmu & 0xffu);
constexpr uint32_t kCorePowerOff = (44160u * 32768u + 999999u) / 1000000u;
// Pinmux banks, OLED DTB pinmux@700008d4 reg: pad control 0x8d4 + 0x2a5, mux 0x3000 + 0x294 (APB_MISC offsets).
constexpr uint32_t kPadFirst = 0x8d4, kPadWords = 0x2a8 / 4, kMuxFirst = 0x3000, kMuxWords = 0x294 / 4;
// Platform action priorities (one service, so they only place it among other services' actions).
constexpr uint32_t kActionPriority = 1000;

unsigned bootArg(const char *name)
{
    unsigned value = 0;
    PE_parse_boot_argn(name, &value, sizeof(value));
    return value;
}
}

class SwitchSuspend : public IOService {
    OSDeclareDefaultStructors(SwitchSuspend) volatile uint8_t *pmc = nullptr, *apb = nullptr;
    uint32_t pad[kPadWords] = {}, mux[kMuxWords] = {};
    uint32_t wakeStatus = 0, wake2Status = 0;
    unsigned sleeps = 0;
    IOPMDriverAssertionID hold = 0;

    uint32_t rd(uint32_t o) const { return *reinterpret_cast<volatile uint32_t *>(pmc + o); }
    void wr(uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(pmc + o) = v; }
    const char *missingSuspendPath() const;
    const char *checkPmc(char *why, size_t size);
    void sleepDevices();
    void quiesce();
    void activate();
    void wakeDevices();

public:
    using IOService::callPlatformFunction;
    bool start(IOService *provider) override;
    IOReturn callPlatformFunction(const OSSymbol *function, bool wait, void *p1, void *p2, void *p3, void *p4) override;
};

OSDefineMetaClassAndStructors(SwitchSuspend, IOService)

    // The first turned-on driver that cannot go through SC7 yet, or nullptr.
    const char *SwitchSuspend::missingSuspendPath() const
{
    char mode[8] = "";
    if (bootArg("switch-wlan")) return "switch-wlan (Wi-Fi has no suspend path)";
    if (bootArg("switch-bt")) return "switch-bt (Bluetooth has no suspend path)";
    if (bootArg("switch-audio")) return "switch-audio (audio has no suspend path)";
    if (bootArg("tegra-gpu")) return "tegra-gpu (the GM20B driver has no suspend path)";
    if (bootArg("switch-emc")) return "switch-emc (the BPMP EMC rate is not re-applied after SC7)";
    if (PE_parse_boot_arg_str("switch-cpu-mode", mode, sizeof(mode)) && !strcmp(mode, "dfll")) {
        return "switch-cpu-mode=dfll (the DFLL is not restored after SC7)";
    }
    if (bootArg("switch-usb-host") || bootArg("switch-dp")) return "USB-C host/DP (no suspend path)";
    // When the video agent's tegra_nvdec_suspend/_resume exist: call them after the Joy-Con hooks in sleepDevices() and
    // before them in wakeDevices(), and drop this line.
    if (bootArg("switch-nvdec") || bootArg("switch-host1x"))
        return "switch-nvdec/switch-host1x (no tegra_nvdec_suspend yet)";
    return nullptr;
}

// The power-request setup L4T's PMC probe makes (CPU and system-clock requests driven, core and system-clock requests
// active high, no LP0 side effect of a CPU power gate) must already be there: flipping a request's polarity or drive
// while it is live could drop a rail, so it is checked, never written. The core power-good/off timers only time an
// SC7 entry/exit and are written as L4T writes them.
const char *SwitchSuspend::checkPmc(char *why, size_t size)
{
    const uint32_t control = rd(PMC_CNTRL);
    const uint32_t wanted = CNTRL_CPU_PWRREQ_OE | CNTRL_SYSCLK_OE;
    const uint32_t unwanted = CNTRL_SIDE_EFFECT_LP0 | CNTRL_PWRREQ_POLARITY | CNTRL_SYSCLK_POLARITY;
    if ((control & wanted) != wanted || (control & unwanted)) {
        snprintf(why, size, "PMC_CNTRL %#x is not L4T's power-request setup (wants bits 16, 11 set, 14, 10, 8 clear)",
                 control);
        return why;
    }
    wr(PMC_COREPWRGOOD_TIMER, kCorePowerGood);
    wr(PMC_COREPWROFF_TIMER, kCorePowerOff);
    (void)rd(PMC_COREPWROFF_TIMER);
    return nullptr;
}

bool SwitchSuspend::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    pmc = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPmcPagePA, 0x1000));
    apb = reinterpret_cast<volatile uint8_t *>(ml_io_map(kApbMiscPA, 0x4000));
    setName("SwitchSuspend");
    char why[160] = "";
    const char *blocker = nullptr;
    if (!pmc || !apb) {
        blocker = "the PMC/APB_MISC could not be mapped";
    } else if (!(blocker = missingSuspendPath()) && !(blocker = checkPmc(why, sizeof(why))) &&
               armcpu_system_sleep_enable(why, sizeof(why))) {
        blocker = why;
    }
    if (blocker) {
        // IOPMrootDomain::checkSystemSleepAllowed: a CPU assertion keeps the system awake (idle and demand sleep).
        IOPMrootDomain *root = getPMRootDomain();
        if (root)
            hold =
                root->createPMAssertion(kIOPMDriverAssertionCPUBit, kIOPMDriverAssertionLevelOn, this, "SwitchSuspend");
        IOLog("SwitchSuspend: system sleep held off: %s; the display still turns off (switch-sleep=1 behaviour)\n",
              blocker);
        registerService();
        return true;
    }
    switch_rtc_sleep_enable();
    // IOService::registerService() installs these as platform actions (iokit/Kernel/IOPlatformActions.cpp).
    setProperty("IOPlatformSleepAction", kActionPriority, 32);
    setProperty("IOPlatformQuiesceAction", kActionPriority, 32);
    setProperty("IOPlatformActiveAction", kActionPriority, 32);
    setProperty("IOPlatformWakeAction", kActionPriority, 32);
    registerService();
    IOLog("SwitchSuspend: started (PSCI SYSTEM_SUSPEND to SC7, wake on the power key and the MAX77620 RTC)\n");
    return true;
}

IOReturn SwitchSuspend::callPlatformFunction(const OSSymbol *function, bool wait, void *p1, void *p2, void *p3,
                                             void *p4)
{
    if (function->isEqualTo("IOPlatformSleepAction")) {
        sleepDevices();
        return kIOReturnSuccess;
    }
    if (function->isEqualTo("IOPlatformQuiesceAction")) {
        quiesce();
        return kIOReturnSuccess;
    }
    if (function->isEqualTo("IOPlatformActiveAction")) {
        activate();
        return kIOReturnSuccess;
    }
    if (function->isEqualTo("IOPlatformWakeAction")) {
        wakeDevices();
        return kIOReturnSuccess;
    }
    return IOService::callPlatformFunction(function, wait, p1, p2, p3, p4);
}

// Thread context, before any core goes down. I2C last: from here to wakeDevices() every transfer waits.
void SwitchSuspend::sleepDevices()
{
    tegra_display_suspend();
    switch_touch_suspend();
    switch_joycon_suspend();
    switch_thermal_suspend();
    ioswitch_block_suspend();
    switch_rtc_suspend();
    // Exactly one wake source: the MAX77620's nIRQ. Statuses cleared first, as tegra210_pmc_irq_set_wake does.
    wr(PMC_SW_WAKE_STATUS, 0);
    wr(PMC_SW_WAKE2_STATUS, 0);
    wr(PMC_WAKE_STATUS, 0);
    wr(PMC_WAKE2_STATUS, 0);
    wr(PMC_WAKE_MASK, 0);
    wr(PMC_WAKE2_LVL, rd(PMC_WAKE2_LVL) & ~kPmicWake2Bit);
    wr(PMC_WAKE2_MASK, kPmicWake2Bit);
    (void)rd(PMC_WAKE2_MASK);
    tegra_i2c_suspend();
}

// Boot CPU, interrupts off, the other cores already off: the state ATF does not keep.
void SwitchSuspend::quiesce()
{
    tegra_gpio_suspend();
    for (uint32_t i = 0; i < kPadWords; ++i) pad[i] = *reinterpret_cast<volatile uint32_t *>(apb + kPadFirst + 4 * i);
    for (uint32_t i = 0; i < kMuxWords; ++i) mux[i] = *reinterpret_cast<volatile uint32_t *>(apb + kMuxFirst + 4 * i);
    arm_gic_suspend();
}

// Boot CPU after SC7, interrupts still off: pads and GPIOs before any interrupt can be unmasked.
void SwitchSuspend::activate()
{
    for (uint32_t i = 0; i < kMuxWords; ++i) *reinterpret_cast<volatile uint32_t *>(apb + kMuxFirst + 4 * i) = mux[i];
    for (uint32_t i = 0; i < kPadWords; ++i) *reinterpret_cast<volatile uint32_t *>(apb + kPadFirst + 4 * i) = pad[i];
    __asm__ volatile("dsb sy" ::: "memory");
    tegra_gpio_resume();
    arm_gic_resume();
    wakeStatus = rd(PMC_WAKE_STATUS) | rd(PMC_SW_WAKE_STATUS);
    wake2Status = rd(PMC_WAKE2_STATUS) | rd(PMC_SW_WAKE2_STATUS);
}

void SwitchSuspend::wakeDevices()
{
    tegra_i2c_resume();
    const bool alarm = switch_rtc_resume() == 1;
    ioswitch_block_resume();
    switch_thermal_resume();
    switch_joycon_resume();
    switch_touch_resume();
    tegra_display_resume();
    ++sleeps;
    const char *reason = alarm ? "rtc" : (wake2Status & kPmicWake2Bit) ? "pwrbtn" : "unknown";
    IOPMrootDomain *root = getPMRootDomain();
    if (root) root->claimSystemWakeEvent(this, kIOPMWakeEventSource, reason, nullptr);
    if (!(wake2Status & kPmicWake2Bit)) {
        IOLog("SwitchSuspend: wake %u without the PMIC wake event (PMC WAKE_STATUS %#x WAKE2_STATUS %#x)\n", sleeps,
              wakeStatus, wake2Status);
    }
}

extern "C" int switch_suspend_start(void)
{
    static SwitchSuspend *suspend;
    if (suspend || bootArg("switch-sleep") != 2) return 0;
    IOService *resources = IOService::getResourceService();
    SwitchSuspend *candidate = OSTypeAlloc(SwitchSuspend);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        IOLog("SwitchSuspend: attach failed; system sleep stays off\n");
        OSSafeReleaseNULL(candidate);
        return 0;
    }
    if (!candidate->start(resources)) {
        candidate->detach(resources);
        candidate->release();
        return 0;
    }
    suspend = candidate;  // the attachment keeps it
    candidate->release();
    return 0;
}
