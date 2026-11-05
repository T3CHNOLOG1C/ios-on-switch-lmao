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
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <libkern/c++/OSSymbol.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include "../SwitchBattery/SwitchBattery.h"
#include "../SwitchMotion/SwitchMotion.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraHost1x/TegraHost1x.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "SwitchSuspend.h"
#include "SwitchWake.h"

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
void switch_sensors_resume(void);
void switch_rtc_sleep_enable(void);
void switch_rtc_suspend(void);
int switch_rtc_resume(void);  // 1 when ALARM1 is what woke the console
void switch_touch_suspend(void);
void switch_touch_resume(void);
void switch_als_suspend(void);
void switch_als_resume(void);
// Owned by the display and controllers agents (NativeMobileDisplay/TegraDisplayPower.h, SwitchJoyCon/SwitchJoyCon.h).
void tegra_display_suspend(void);
void tegra_display_resume(void);
void switch_joycon_suspend(void);
void switch_joycon_resume(void);
bool switch_display_power_enabled(void);  // SwitchBrightness
}

namespace {
constexpr vm_offset_t kPmcPagePA = 0x7000E000, kApbMiscPA = 0x70000000, kCarPA = 0x60006000;
// The Tegra RTC at the start of the PMC page (Linux rtc-tegra.c); its APB clock is CAR L4.
enum : uint32_t { RTC_SHADOW_SECONDS = 0x00c, RTC_MILLI_SECONDS = 0x010 };
enum : uint32_t { CAR_RST_DEVICES_L = 0x004, CAR_CLK_OUT_ENB_L = 0x010, CAR_CLK_ENB_L_SET = 0x320, CAR_L_RTC = 1u << 4 };
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
// The wake events (SwitchWakeSources.h switch_wake_pmc_events): 51 pwr_int_n (MAX77620 nIRQ: power key, RTC alarm),
// level low; 14 gpio_pk4 (BM92T36 USB-PD ALERT: plug, unplug, charger contract), level low (OLED DTB bm92t@18
// nvidia,pmc-wakeup), armed only while the line is high, since SwitchPD masks a line stuck low for the boot.
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
// ArmCpu panics on a halt unless SC7 is on (armcpu_system_sleep_enable), so whenever it is not, a CPU assertion
// keeps IOPMrootDomain from sleeping (checkSystemSleepAllowed: kPMCPUAssertion), idle and demand sleep alike.
IOPMDriverAssertionID switch_suspend_hold(IOService *owner, const char *why)
{
    IOPMrootDomain *root = IOService::getPMRootDomain();
    const IOPMDriverAssertionID id =
        root ? root->createPMAssertion(kIOPMDriverAssertionCPUBit, kIOPMDriverAssertionLevelOn, owner, "SwitchSuspend")
             : 0;
    IOLog("SwitchSuspend: system sleep held off%s: %s\n", id ? "" : " (NO assertion: a sleep would panic)", why);
    switch_power_journal("SwitchSuspend: system sleep held off: %s", why);
    return id;
}
}  // namespace

class SwitchSuspend : public IOService {
    OSDeclareDefaultStructors(SwitchSuspend) volatile uint8_t *pmc = nullptr, *apb = nullptr, *car = nullptr;
    uint32_t pad[kPadWords] = {}, mux[kMuxWords] = {};
    uint32_t wakeStatus = 0, wake2Status = 0;
    uint64_t wakeArmed = 0;  // PMC wake events, bit n = event n
    uint64_t quiesceNS = 0, activateNS = 0, suspendedAt = 0;
    uint64_t rtcBeforeMS = 0, rtcAfterMS = 0;  // the Tegra RTC at quiesce and at activate
    bool rtcUsable = false;
    switch_battery_gauge gaugeBefore = {};
    int gaugeBeforeErr = -1;
    unsigned sleeps = 0;
    IOPMDriverAssertionID hold = 0;

    uint32_t rd(uint32_t o) const { return *reinterpret_cast<volatile uint32_t *>(pmc + o); }
    uint64_t rtcMS() const
    {
        const uint32_t millis = rd(RTC_MILLI_SECONDS);
        return switch_sleep_rtc_ms(rd(RTC_SHADOW_SECONDS), millis);
    }
    bool rtcClockOn(bool log);
    void wr(uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(pmc + o) = v; }
    static void missingSuspendPaths(char *list, size_t size);
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

namespace {
void appendName(char *list, size_t size, const char *name)
{
    const size_t used = strnlen(list, size);
    if (used + 1 < size) snprintf(list + used, size - used, "%s%s", used ? ", " : "", name);
}
}  // namespace

// The turned-on drivers that cannot go through SC7 yet, as a ", "-separated list (empty when there is none).
void SwitchSuspend::missingSuspendPaths(char *list, size_t size)
{
    list[0] = 0;
    if (bootArg("switch-wlan")) appendName(list, size, "Wi-Fi");
    if (bootArg("switch-bt")) appendName(list, size, "Bluetooth");
    if (bootArg("switch-audio")) appendName(list, size, "audio");
    if (bootArg("tegra-gpu")) appendName(list, size, "GM20B");
    if (bootArg("switch-emc")) appendName(list, size, "EMC rate (BPMP)");
    if (bootArg("switch-usb-host") || bootArg("switch-dp")) appendName(list, size, "USB-C host/DP");
}

// The RTC keeps time through SC7 by itself; only its register interface needs the APB clock, which nothing else here
// turns off. In reset it reads nothing: the wake line then has no time asleep.
bool SwitchSuspend::rtcClockOn(bool log)
{
    auto reg = [this](uint32_t o) { return reinterpret_cast<volatile uint32_t *>(car + o); };
    if (*reg(CAR_RST_DEVICES_L) & CAR_L_RTC) {
        if (log) IOLog("SwitchSuspend: the Tegra RTC is held in reset; no time asleep in the wake lines\n");
        return false;
    }
    if (!(*reg(CAR_CLK_OUT_ENB_L) & CAR_L_RTC)) {
        *reg(CAR_CLK_ENB_L_SET) = CAR_L_RTC;
        (void)*reg(CAR_CLK_OUT_ENB_L);
        IODelay(2);
    }
    return true;
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

namespace {
uint64_t nsSince(uint64_t start)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    return ns;
}

// One step of sleep entry or exit, timed, as one power-journal line (thread context only).
void timed(const char *phase, unsigned index, const char *name, void (*hook)(void))
{
    const uint64_t start = mach_absolute_time();
    hook();
    const uint64_t us = nsSince(start) / 1000;
    switch_power_journal("SwitchSuspend: %s %u %s: %llu.%03llu ms", phase, index, name, us / 1000, us % 1000);
}
}  // namespace

bool SwitchSuspend::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    pmc = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPmcPagePA, 0x1000));
    apb = reinterpret_cast<volatile uint8_t *>(ml_io_map(kApbMiscPA, 0x4000));
    car = reinterpret_cast<volatile uint8_t *>(ml_io_map(kCarPA, 0x1000));
    setName("SwitchSuspend");
    rtcUsable = pmc && car && rtcClockOn(true);
    char why[224] = "", missing[160] = "";
    missingSuspendPaths(missing, sizeof(missing));
    const char *blocker = nullptr;
    if (!pmc || !apb) {
        blocker = "the PMC/APB_MISC could not be mapped";
    } else if (!switch_display_power_enabled()) {
        blocker = "display power is off (switch-sleep=0)";
    } else if (missing[0]) {
        snprintf(why, sizeof(why), "no SC7 path yet: %s", missing);
        blocker = why;
    } else if (!(blocker = checkPmc(why, sizeof(why))) && armcpu_system_sleep_enable(why, sizeof(why))) {
        blocker = why;
    }
    if (blocker) {
        hold = switch_suspend_hold(this, blocker);
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
    switch_power_journal("SwitchSuspend: system sleep on (SC7)");
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
    const uint64_t start = mach_absolute_time();
    switch_power_journal("SwitchSuspend: sleep %u: entering SC7", sleeps + 1);
    // first: it shares LDO6 with the touch panel, and its clock is the display's frame end
    timed("sleep", 0, "als", switch_als_suspend);
    timed("sleep", 1, "display", tegra_display_suspend);
    timed("sleep", 2, "touch", switch_touch_suspend);
    timed("sleep", 2, "motion", switch_motion_suspend);  // IMU off, SPI4 clock off
    timed("sleep", 3, "joycon", switch_joycon_suspend);
    timed("sleep", 4, "host1x", tegra_nvdec_suspend);  // NVDEC, NVENC, VIC, NVJPG quiesced, then host1x
    timed("sleep", 5, "thermal", switch_thermal_suspend);
    timed("sleep", 6, "nvram", [] { PESyncNVRAM(); });  // the NVRAM controller's sync (SwitchNVRAM), SD still up
    // The journal to Data while the SD is up: what follows is in RAM only until the wake (and lost to a forced
    // power-off).
    switch_power_journal_flush(true);
    timed("sleep", 7, "sd", ioswitch_block_suspend);
    timed("sleep", 8, "rtc", switch_rtc_suspend);
    timed("sleep", 8, "battery", switch_battery_suspend);  // the gauge's alert: charge-zone edges only
    gaugeBeforeErr = switch_battery_gauge_sample(&gaugeBefore);
    if (!gaugeBeforeErr) {
        switch_power_journal("SwitchSuspend: sleep 8 gauge: AvgCurrent %d mA, Current %d mA, QH %u", gaugeBefore.avg_ma,
                             gaugeBefore.now_ma, gaugeBefore.qh);
    }
    // Wake sources (SwitchWakeSources.h): the MAX77620's nIRQ (power key, RTC alarm), the USB-PD ALERT (USB-C
    // plug/unplug) and the gauge's ALRT (the battery's charge zone). Statuses cleared first, as
    // tegra210_pmc_irq_set_wake does.
    wakeArmed =
        switch_wake_pmc_armed(tegra_gpio_read(TEGRA_GPIO_PD_ALERT) == 1, tegra_gpio_read(TEGRA_GPIO_GAUGE_ALERT) == 1);
    uint32_t mask[2], high[2], low[2];
    switch_wake_pmc_registers(wakeArmed, mask, high, low);
    wr(PMC_SW_WAKE_STATUS, 0);
    wr(PMC_SW_WAKE2_STATUS, 0);
    wr(PMC_WAKE_STATUS, 0);
    wr(PMC_WAKE2_STATUS, 0);
    wr(PMC_WAKE_LVL, (rd(PMC_WAKE_LVL) | high[0]) & ~low[0]);
    wr(PMC_WAKE_MASK, mask[0]);
    wr(PMC_WAKE2_LVL, (rd(PMC_WAKE2_LVL) | high[1]) & ~low[1]);
    wr(PMC_WAKE2_MASK, mask[1]);
    (void)rd(PMC_WAKE2_MASK);
    switch_power_journal("SwitchSuspend: sleep 9: PMC wake events armed %#llx (WAKE_MASK %#x WAKE2_MASK %#x)",
                         (unsigned long long)wakeArmed, mask[0], mask[1]);
    const uint64_t us = nsSince(start) / 1000;
    switch_power_journal("SwitchSuspend: sleep 10: i2c (last; devices took %llu.%03llu ms)", us / 1000, us % 1000);
    tegra_i2c_suspend();
    suspendedAt = mach_absolute_time();
}

// Boot CPU, interrupts off, the other cores already off: the state ATF does not keep.
void SwitchSuspend::quiesce()
{
    const uint64_t start = mach_absolute_time();
    tegra_gpio_suspend();
    for (uint32_t i = 0; i < kPadWords; ++i) pad[i] = *reinterpret_cast<volatile uint32_t *>(apb + kPadFirst + 4 * i);
    for (uint32_t i = 0; i < kMuxWords; ++i) mux[i] = *reinterpret_cast<volatile uint32_t *>(apb + kMuxFirst + 4 * i);
    arm_gic_suspend();
    rtcBeforeMS = rtcUsable ? rtcMS() : 0;
    quiesceNS = nsSince(start);  // journaled at wake: nothing here may take a lock
}

// Boot CPU after SC7, interrupts still off: pads and GPIOs before any interrupt can be unmasked.
void SwitchSuspend::activate()
{
    const uint64_t start = mach_absolute_time();
    rtcAfterMS = rtcUsable && rtcClockOn(false) ? rtcMS() : rtcBeforeMS;
    for (uint32_t i = 0; i < kMuxWords; ++i) *reinterpret_cast<volatile uint32_t *>(apb + kMuxFirst + 4 * i) = mux[i];
    for (uint32_t i = 0; i < kPadWords; ++i) *reinterpret_cast<volatile uint32_t *>(apb + kPadFirst + 4 * i) = pad[i];
    __asm__ volatile("dsb sy" ::: "memory");
    tegra_gpio_resume();
    arm_gic_resume();
    wakeStatus = rd(PMC_WAKE_STATUS) | rd(PMC_SW_WAKE_STATUS);
    wake2Status = rd(PMC_WAKE2_STATUS) | rd(PMC_SW_WAKE2_STATUS);
    activateNS = nsSince(start);
}

void SwitchSuspend::wakeDevices()
{
    const uint64_t start = mach_absolute_time();
    // The time between the last sleep step and the first wake step: SC7 and the firmware's warm boot, by this clock.
    const uint64_t asleepMS = suspendedAt ? nsSince(suspendedAt) / 1000000 : 0;
    timed("wake", 0, "i2c", tegra_i2c_resume);
    switch_battery_gauge gaugeAfter = {};
    const int gaugeAfterErr = switch_battery_gauge_sample(&gaugeAfter);
    const uint64_t sc7MS = rtcUsable ? switch_sleep_elapsed_ms(rtcBeforeMS, rtcAfterMS) : 0;
    switch_power_journal("SwitchSuspend: wake %u: asleep %llu.%03llu s (Tegra RTC, quiesce to activate); entry to wake "
                         "%llu ms by the timebase; quiesce %llu us, activate %llu us (interrupts off)",
                         sleeps + 1, sc7MS / 1000, sc7MS % 1000, asleepMS, quiesceNS / 1000, activateNS / 1000);
    if (!gaugeBeforeErr && !gaugeAfterErr) {
        switch_power_journal("SwitchSuspend: wake %u battery: %d mA average asleep (QH %u -> %u, %u uAh a count), "
                             "AvgCurrent %d mA, Current %d mA at wake",
                             sleeps + 1,
                             switch_sleep_average_ma(gaugeBefore.qh, gaugeAfter.qh, SWITCH_BATTERY_QH_UAH, sc7MS),
                             gaugeBefore.qh, gaugeAfter.qh, SWITCH_BATTERY_QH_UAH, gaugeAfter.avg_ma,
                             gaugeAfter.now_ma);
    } else {
        switch_power_journal("SwitchSuspend: wake %u battery: gauge not read (I2C %d before, %d after)", sleeps + 1,
                             gaugeBeforeErr, gaugeAfterErr);
    }
    bool alarm = false;
    {
        const uint64_t at = mach_absolute_time();
        alarm = switch_rtc_resume() == 1;
        switch_power_journal("SwitchSuspend: wake 1 rtc: %llu us%s", nsSince(at) / 1000, alarm ? " (ALARM1)" : "");
    }
    {
        const uint64_t at = mach_absolute_time();
        const int lost = tegra_smmu_resume();  // before any translated client (host1x engines, display) runs
        switch_power_journal("SwitchSuspend: wake 2 smmu: %llu us%s", nsSince(at) / 1000,
                             lost ? " (a swgroup was not restored)" : "");
    }
    timed("wake", 3, "sd", ioswitch_block_resume);
    timed("wake", 4, "battery", switch_battery_resume);  // the normal gauge windows, a re-read
    timed("wake", 5, "thermal", switch_thermal_resume);
    timed("wake", 5, "sensors", switch_sensors_resume);
    timed("wake", 6, "host1x", tegra_nvdec_resume);  // before the display, which sits behind host1x
    timed("wake", 7, "joycon", switch_joycon_resume);
    timed("wake", 8, "motion", switch_motion_resume);
    timed("wake", 8, "touch", switch_touch_resume);
    timed("wake", 9, "display", tegra_display_resume);
    timed("wake", 10, "als", switch_als_resume);  // last: after LDO6 (touch) and the frame ends (display) are back
    ++sleeps;
    const uint32_t status[2] = {wakeStatus, wake2Status};
    const char *reason = alarm ? "rtc" : switch_wake_pmc_reason(status, wakeArmed);
    IOPMrootDomain *root = getPMRootDomain();
    if (root) root->claimSystemWakeEvent(this, kIOPMWakeEventSource, reason, nullptr);
    const uint64_t us = nsSince(start) / 1000;
    switch_power_journal("SwitchSuspend: wake %u: %s (PMC WAKE_STATUS %#x WAKE2_STATUS %#x), devices %llu.%03llu ms",
                         sleeps, reason, wakeStatus, wake2Status, us / 1000, us % 1000);
    if (!strcmp(reason, "unknown")) {
        IOLog("SwitchSuspend: wake %u without an armed PMC wake event (PMC WAKE_STATUS %#x WAKE2_STATUS %#x, armed "
              "%#llx)\n",
              sleeps, wakeStatus, wake2Status, (unsigned long long)wakeArmed);
    }
}

extern "C" int switch_suspend_start(void)
{
    static SwitchSuspend *suspend;
    if (suspend) return 0;
    IOService *resources = IOService::getResourceService();
    SwitchSuspend *candidate = OSTypeAlloc(SwitchSuspend);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        OSSafeReleaseNULL(candidate);
        if (resources) (void)switch_suspend_hold(resources, "SwitchSuspend could not attach");
        return 0;
    }
    if (!candidate->start(resources)) {
        candidate->detach(resources);
        candidate->release();
        (void)switch_suspend_hold(resources, "SwitchSuspend did not start");
        return 0;
    }
    suspend = candidate;  // the attachment keeps it
    candidate->release();
    return 0;
}
