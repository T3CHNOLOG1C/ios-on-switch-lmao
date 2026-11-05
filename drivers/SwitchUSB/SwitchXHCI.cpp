// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI: the Tegra X1 XUSB host controller under Apple's original AppleUSBXHCI (21H461).
//
// iPadOS drives USB host through IOUSBHostFamily + AppleUSBXHCI (generic xHCI: rings, slots, ports, hubs, power) with
// a thin SoC subclass (AppleNubT8011USBXHCI on iPad7,1). Once the falcon firmware runs, Tegra's host is a plain xHCI
// 1.0 (Linux drives it with the generic xhci-hcd), so SwitchXHCI is the same kind of thin subclass: XusbHost.cpp does
// the Tegra bring-up, a provider nub carries the register window and the host interrupt, and SwitchXHCI keeps what is
// Tegra's own: DMA coherency (ring buffers allocated non-cacheable, copyMapper() -> UsbDmaMapper.cpp) and the falcon
// mailbox (its own interrupt). Everything above (enumeration, IOUSBHostDevice/Interface publication, hubs, HID and
// mass storage class drivers) is Apple's unchanged code.
//
// AppleUSBXHCI::start refuses a controller with fewer than three interrupters (HCSPARAMS1.MaxIntrs). With one provider
// interrupt it runs them all on the shared line (legacy interrupters), so when the Tegra firmware reports fewer, we
// report three and keep the missing register sets in a shadow (regRead32/regRead64/regWrite32/regWrite64, slots
// 290-293: AppleUSBXHCIInterrupter::enable reads ERSTBA and ERDP back through regRead64); which case ran is logged once.
//
// Interrupts only: AppleUSBXHCI's own filter owns INTID 71, the mailbox filter (INTID 72) is ours. No timer runs in
// steady state. Routing (checked against the sources): the OLED DT's xusb@70090000 has interrupts GIC_SPI 39 (host)
// and 40 (mailbox), IRQ_TYPE_LEVEL_HIGH, through the root interrupt-parent, the Tegra LIC, into the GIC: INTID 71 and
// 72. ArmGIC programs a level SPI (ICFGR edge bit clear), CPU0, priority 0xa0, and the LIC gate (IEP_CLASS IRQ, IER
// set) with GICD_ISENABLER at enable; makeNub gives AppleUSBXHCI the GIC nub's own interrupt specifier. The Tegra side
// is xhci-tegra.c tegra_xusb_config exactly: IPFS EN_FPCI, BAR0, FPCI CFG_1 bus master, IPFS INTR_MASK IP_INT_MASK,
// hysteresis 0x80 (IPFS INTR_ENABLE and the MSI vectors are left at reset, as Linux leaves them). IMAN0.IE and
// USBCMD.INTE are AppleUSBXHCI's.
// Neither line had fired on this hardware when this was written, so their check is a never-booted one until they
// have (XusbPorts.h xusb_irq_judge): when the PD controller reports a device on the port in host role a port-status
// event is due (SwitchUSB.cpp calls switchxhci_expect_port_change); 2 s later, and again 50 ms after that, a line that
// has not reached its handler once this boot (ioswitch_interrupt_count) with an event waiting, or, after the
// controller's first start, with nothing delivered at all, logs one line with the whole path (xHCI, FPCI, IPFS, GIC)
// and turns host mode off for the boot (switch_usb_host_failed: device mode stays). Once a line has fired, IMAN0.IP
// pending (with IE and INTE) and ERDP0 unmoved, or a mailbox interrupt pending with no handler run, panics as for every
// proven line. A mailbox line that fires with nothing to take is turned off with one line, never left to storm CPU0.
#include "OriginalAppleUSBXHCI.h"
#include "SwitchUSB.h"
#include "UsbDmaMapper.h"
#include "XusbHost.h"
#include "XusbPorts.h"
#include <IOKit/IOCatalogue.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include "../ArmGIC/IOSwitchInterrupts.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void switchxhci_static_init(void) __asm__("__GLOBAL__sub_I_SwitchXHCI.cpp");
// tegra210b01_xusb_firmware (firmware.S, drivers/SwitchUSB/firmware/README.md).
extern const uint8_t switch_xusb_firmware[] __asm__("_switch_xusb_firmware");
extern const uint8_t switch_xusb_firmware_end[] __asm__("_switch_xusb_firmware_end");
}

namespace {
xhost::Controller gHost;
constexpr uint32_t kRequiredInterrupters = 3;
constexpr uint32_t kCheckMS = 2000, kConfirmMS = 50;
constexpr uint32_t kUnclaimedLimit = 1000;  // mailbox interrupts in a row with no SMI to take: a line we cannot clear
}  // namespace

class SwitchXHCI : public AppleUSBXHCI {
    OSDeclareDefaultStructors(SwitchXHCI);

    IOWorkLoop *tegraLoop;  // mailbox and the lost-interrupt check; AppleUSBXHCI keeps its own loop
    IOFilterInterruptEventSource *mailbox;
    IOTimerEventSource *check;
    volatile uint32_t mailboxInterrupts;
    uint32_t unclaimed;  // primary interrupt context only
    volatile bool mailboxStorm;
    xusb_irq_sample hostAtCheck, mailboxAtCheck;
    uint8_t checkStage;
    bool firstCheck, checked, gaveUp;
    // Interrupter fold (see the top comment).
    uint32_t hardwareInterrupters, runtimeOffset;
    uint64_t capBase;  // AppleUSBXHCI's mapping of the capability registers
    uint32_t shadow[kRequiredInterrupters][8];

    static bool mailboxFilter(OSObject *owner, IOFilterInterruptEventSource *);
    static void mailboxAction(OSObject *owner, IOInterruptEventSource *, int);
    void armCheck(uint32_t ms, uint8_t stage);
    void checkFired(IOTimerEventSource *);
    xusb_irq_sample hostSample() const;
    xusb_irq_sample mailboxSample() const;
    uint32_t *shadowFor(uint64_t address);

public:
    static SwitchXHCI *running;
    IOWorkLoop *tegraLoopForCheck() const { return tegraLoop; }
    IOReturn expectPortChange();
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    uint32_t regRead32(uint64_t base, uint32_t offset) override;
    uint64_t regRead64(uint64_t base, uint32_t offset) override;
    void regWrite32(uint64_t base, uint32_t offset, uint32_t value) override;
    void regWrite64(uint64_t base, uint32_t offset, uint64_t value) override;
    static uint64_t dmaReach() { return (gHost.xhci(0x10) & 1) ? (1ull << 40) : (1ull << 32); }  // HCCPARAMS1.AC64
    IOMapper *copyMapper() override
    {
        IOMapper *mapper = switch_usb_dma_mapper(dmaReach());
        if (mapper) mapper->retain();
        return mapper;
    }
};
OSDefineMetaClassAndStructors(SwitchXHCI, AppleUSBXHCI);
SwitchXHCI *SwitchXHCI::running;

bool SwitchXHCI::start(IOService *provider)
{
    hardwareInterrupters = (gHost.xhci(4) >> 8) & 0x7ff;
    runtimeOffset = gHost.runtimeOffset();  // read once: regRead32 also runs in the primary interrupt filter
    // Controller structures non-cacheable: the write-combine cache mode (normal non-cacheable) in place of the one
    // AppleUSBHostController::init picked (AppleNubT8011USBXHCI clears it: its fabric is coherent; ours is not).
    ringBufferOptions() = (ringBufferOptions() & ~(uint32_t)kIOMapCacheMask) | (uint32_t)kIOMapWriteCombineCache;
    // Without the mapper AppleUSBXHCI would run on (copyMapper NULL is no error to it) there is no cache maintenance.
    if (!switch_usb_dma_mapper(dmaReach())) return false;

    tegraLoop = IOWorkLoop::workLoop();
    check = tegraLoop ? IOTimerEventSource::timerEventSource(
                            this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &SwitchXHCI::checkFired))
                      : nullptr;
    if (!check || tegraLoop->addEventSource(check) != kIOReturnSuccess) return false;
    IOService *nub = ioswitch_interrupt_nub(xhost::kMailboxIntid, false);
    mailbox = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchXHCI::mailboxAction,
                                                                       &SwitchXHCI::mailboxFilter, nub, 0);
    if (!mailbox || tegraLoop->addEventSource(mailbox) != kIOReturnSuccess) {
        panic("SwitchUSB: cannot attach the XUSB mailbox interrupt (INTID %u)", xhost::kMailboxIntid);
    }
    mailbox->enable();
    if (!AppleUSBXHCI::start(provider)) return false;
    // xhci-tegra.c tegra_xusb_probe_finish: firmware messages on, then USBCMD.EU3S (bit 11, MFINDEX stops in U3).
    if (!gHost.sendMailbox(xhost::MBOX_MSG_ENABLED, 0)) IOLog("SwitchUSB: XUSB falcon did not take MSG_ENABLED\n");
    if (capBase) {
        const uint32_t usbcmd = gHost.capLength();
        regWrite32(capBase, usbcmd, regRead32(capBase, usbcmd) | 1u << 11);
    }
    return true;
}

void SwitchXHCI::stop(IOService *provider)
{
    if (check) check->cancelTimeout();
    if (mailbox) mailbox->disable();
    AppleUSBXHCI::stop(provider);
}

void SwitchXHCI::free()
{
    if (tegraLoop) {
        if (check) tegraLoop->removeEventSource(check);
        if (mailbox) tegraLoop->removeEventSource(mailbox);
    }
    OSSafeReleaseNULL(check);
    OSSafeReleaseNULL(mailbox);
    OSSafeReleaseNULL(tegraLoop);
    AppleUSBXHCI::free();
}

// Primary interrupt context. INTID 72 with no SMI to acknowledge is ignored a few times (a spurious level); a line
// that keeps firing with nothing to take would storm CPU0, so after kUnclaimedLimit in a row it goes to the action
// (returning true masks the level source until then), which turns it off for good with one line.
bool SwitchXHCI::mailboxFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<SwitchXHCI *>(owner);
    if (gHost.takeMailboxInterrupt()) {
        self->unclaimed = 0;
        __atomic_fetch_add(&self->mailboxInterrupts, 1, __ATOMIC_RELAXED);
        return true;
    }
    if (++self->unclaimed < kUnclaimedLimit) return false;
    self->mailboxStorm = true;
    return true;
}

void SwitchXHCI::mailboxAction(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchXHCI *>(owner);
    if (self->mailboxStorm) {
        self->mailbox->disable();
        char state[200];
        gHost.describe(state, sizeof(state));
        IOLog("SwitchUSB: XUSB mailbox interrupt (INTID %u) fired %u times with no SMI to take, turned off: %s\n",
              xhost::kMailboxIntid, kUnclaimedLimit, state);
        return;
    }
    if (!gHost.serviceMailbox()) {
        char state[200];
        gHost.describe(state, sizeof(state));
        panic("SwitchUSB: XUSB falcon firmware hang (mailbox SMI FW_HANG): %s", state);
    }
}

// A device just appeared on the port in host role: its port-status event is due. The first time after the
// controller's start, a port event is due even if none is pending (the device was on the port as it powered up).
IOReturn SwitchXHCI::expectPortChange()
{
    if (checkStage || gaveUp) return kIOReturnSuccess;
    firstCheck = !checked;
    checked = true;
    armCheck(kCheckMS, 1);
    return kIOReturnSuccess;
}

xusb_irq_sample SwitchXHCI::hostSample() const
{
    uint32_t iman = 0, usbcmd = 0, usbsts = 0, erdp = 0;
    (void)gHost.interruptSample(iman, usbcmd, usbsts, erdp);
    return {ioswitch_interrupt_count(xhost::kHostIntid), erdp, xusb_xhci_pending(iman, usbsts),
            xusb_xhci_deliverable(iman, usbcmd)};
}

xusb_irq_sample SwitchXHCI::mailboxSample() const
{
    return {ioswitch_interrupt_count(xhost::kMailboxIntid), 0, !mailboxStorm && gHost.mailboxPending(), true};
}

void SwitchXHCI::armCheck(uint32_t ms, uint8_t stage)
{
    checkStage = stage;
    hostAtCheck = hostSample();
    mailboxAtCheck = mailboxSample();
    check->setTimeoutMS(ms);
}

void SwitchXHCI::checkFired(IOTimerEventSource *)
{
    const xusb_irq_sample host = hostSample(), mailbox = mailboxSample();
    const xusb_irq_verdict hostVerdict = xusb_irq_judge(hostAtCheck, host, firstCheck, checkStage);
    const xusb_irq_verdict mailboxVerdict = xusb_irq_judge(mailboxAtCheck, mailbox, false, checkStage);
    const bool hostSide = hostVerdict >= mailboxVerdict;
    const xusb_irq_verdict verdict = hostSide ? hostVerdict : mailboxVerdict;
    if (verdict == XUSB_IRQ_FINE) {
        checkStage = 0;
        firstCheck = false;
        return;
    }
    if (verdict == XUSB_IRQ_CONFIRM) {
        armCheck(kConfirmMS, 2);
        return;
    }
    const uint32_t intid = hostSide ? xhost::kHostIntid : xhost::kMailboxIntid;
    char state[480], gic[192];
    gHost.describe(state, sizeof(state));
    ioswitch_interrupt_describe(intid, gic, sizeof(gic));
    if (verdict == XUSB_IRQ_PANIC) {
        panic("SwitchUSB: XUSB %s interrupt lost (INTID %u): %s, %s", hostSide ? "host" : "mailbox", intid, state, gic);
    }
    checkStage = 0;
    gaveUp = true;
    const bool waiting = hostSide ? host.pending : mailbox.pending;
    IOLog("SwitchUSB: host mode off for this boot: the XUSB %s interrupt (INTID %u, never fired this boot) did not "
          "reach the CPU %s: %s; %s\n",
          hostSide ? "host" : "mailbox", intid,
          waiting ? "with an event waiting" : "2 s after the controller's first start (no event waiting either)", state,
          gic);
    switch_usb_host_failed();
}

// Interrupter register sets (runtime base + 0x20 + 32 * n) that the hardware does not have.
uint32_t *SwitchXHCI::shadowFor(uint64_t address)
{
    if (!capBase || hardwareInterrupters >= kRequiredInterrupters) return nullptr;
    const uint64_t first = capBase + runtimeOffset + 0x20;
    const uint64_t from = first + 32ull * hardwareInterrupters, to = first + 32ull * kRequiredInterrupters;
    if (address < from || address >= to) return nullptr;
    const uint64_t at = address - first;
    return &shadow[at / 32][(at % 32) / 4];
}

uint32_t SwitchXHCI::regRead32(uint64_t base, uint32_t offset)
{
    if (!capBase && offset == 4) capBase = base;  // start reads the capability registers first
    if (uint32_t *s = shadowFor(base + offset)) return *s;
    uint32_t value = AppleUSBXHCI::regRead32(base, offset);
    if (capBase && base + offset == capBase + 4 && hardwareInterrupters < kRequiredInterrupters) {
        value = (value & ~(0x7ffu << 8)) | kRequiredInterrupters << 8;
    }
    return value;
}

uint64_t SwitchXHCI::regRead64(uint64_t base, uint32_t offset)
{
    if (uint32_t *s = shadowFor(base + offset)) return s[0] | (uint64_t)s[1] << 32;
    return AppleUSBXHCI::regRead64(base, offset);
}

void SwitchXHCI::regWrite32(uint64_t base, uint32_t offset, uint32_t value)
{
    if (uint32_t *s = shadowFor(base + offset)) {
        *s = value;
        return;
    }
    AppleUSBXHCI::regWrite32(base, offset, value);
}

void SwitchXHCI::regWrite64(uint64_t base, uint32_t offset, uint64_t value)
{
    if (uint32_t *s = shadowFor(base + offset)) {
        s[0] = (uint32_t)value;
        s[1] = (uint32_t)(value >> 32);
        return;
    }
    AppleUSBXHCI::regWrite64(base, offset, value);
}

// ---------------------------------------------------------------------------
// Bring-up. SwitchXHCI's metaclass needs the loaded original AppleUSBXHCI, so its static initializer is staged
// (Drivers.cmake ioswitch_late_object) and runs here.
namespace {
const OSMetaClass *lookupClass(const char *text)
{
    const OSSymbol *name = OSSymbol::withCString(text);
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    return meta;
}

// An IOResources personality ("IOResourceMatch" IOKit), as in the kext's Info.plist, into the catalogue (matched now).
bool addResourcePersonality(const char *ioClass, const char *category, const char *bundle)
{
    OSDictionary *personality = OSDictionary::withCapacity(5);
    bool ok = personality != nullptr;
    const char *const pairs[][2] = {{"IOClass", ioClass},
                                    {"IOMatchCategory", category},
                                    {"IOProviderClass", "IOResources"},
                                    {"IOResourceMatch", "IOKit"},
                                    {"CFBundleIdentifier", bundle}};
    for (const auto &pair : pairs) {
        const OSSymbol *value = ok ? OSSymbol::withCString(pair[1]) : nullptr;
        ok = value && personality->setObject(pair[0], value);
        OSSafeReleaseNULL(value);
    }
    OSArray *array = ok ? OSArray::withObjects((const OSObject **)&personality, 1) : nullptr;
    ok = array && gIOCatalogue->addDrivers(array, true);
    OSSafeReleaseNULL(array);
    OSSafeReleaseNULL(personality);
    return ok;
}

// Provider nub: register window index 0 and the host interrupt as index 0 (the GIC nub's own interrupt properties, so
// IOService resolves it through ArmGIC).
IOService *makeNub()
{
    IOService *gic = ioswitch_interrupt_nub(xhost::kHostIntid, false);
    OSObject *controllers = gic ? gic->copyProperty(gIOInterruptControllersKey) : nullptr;
    OSObject *specifiers = gic ? gic->copyProperty(gIOInterruptSpecifiersKey) : nullptr;
    OSDictionary *dict = OSDictionary::withCapacity(4);
    IODeviceMemory *window = IODeviceMemory::withRange(xhost::kXhciPA, xhost::kXhciBytes);
    OSArray *memory = window ? OSArray::withObjects((const OSObject **)&window, 1) : nullptr;
    IOService *nub = OSTypeAlloc(IOService);
    bool ok = controllers && specifiers && dict && memory && nub;
    if (ok) {
        dict->setObject(gIOInterruptControllersKey, controllers);
        dict->setObject(gIOInterruptSpecifiersKey, specifiers);
        ok = nub->init(dict) && nub->attach(IOService::getResourceService());
    }
    if (ok) {
        nub->setDeviceMemory(memory);
        nub->setName("usb-xhci");
    }
    OSSafeReleaseNULL(controllers);
    OSSafeReleaseNULL(specifiers);
    OSSafeReleaseNULL(dict);
    OSSafeReleaseNULL(window);
    OSSafeReleaseNULL(memory);
    if (!ok) OSSafeReleaseNULL(nub);
    return nub;
}
}  // namespace

bool switchxhci_start(char *why, size_t whySize)
{
    if (!gHost.map()) {
        snprintf(why, whySize, "MMIO map failed");
        return false;
    }
    if (!gHost.powerOn(why, whySize)) return false;
    if (!gHost.loadFirmware(switch_xusb_firmware, (size_t)(switch_xusb_firmware_end - switch_xusb_firmware), why,
                            whySize))
        return false;
    // The host stack's IOResources personalities, kept out of the boot-time catalogue (emit_prelink.py
    // DEFERRED_PERSONALITIES): AppleUSBHostResources (AppleUSBHostController::start waits for it) and
    // IOUSBMassStorageResource, as their kexts' Info.plist has them.
    if (!addResourcePersonality("AppleUSBHostResources", "AppleUSBHostResources", "com.apple.iokit.IOUSBHostFamily") ||
        !addResourcePersonality("IOUSBMassStorageResource", "IOUSBMassStorageResource",
                                "com.apple.iokit.IOUSBMassStorageDriver")) {
        snprintf(why, whySize, "IOCatalogue refused the USB host resource personalities");
        return false;
    }
    static const char *const kStack[] = {"com.apple.iokit.IOUSBHostFamily", "com.apple.driver.usb.AppleUSBXHCI"};
    for (const char *kext : kStack) {
        if (OSKext::loadKextWithIdentifier(kext, false) != KERN_SUCCESS) {
            snprintf(why, whySize, "%s did not load (is it prelinked?)", kext);
            return false;
        }
    }
    // Class drivers: their personalities match what AppleUSBXHCI publishes; a missing one only loses that class.
    static const char *const kClasses[] = {
        "com.apple.driver.usb.AppleUSBHub", "com.apple.driver.usb.AppleUSBHostCompositeDevice",
        "com.apple.driver.usb.IOUSBHostHIDDevice", "com.apple.iokit.IOUSBMassStorageDriver",
        "com.apple.iokit.IOSCSIBlockCommandsDevice"};
    for (const char *kext : kClasses) {
        if (OSKext::loadKextWithIdentifier(kext, false) != KERN_SUCCESS) IOLog("SwitchUSB: %s did not load\n", kext);
    }
    const OSMetaClass *parent = lookupClass("AppleUSBXHCI");
    if (parent != &AppleUSBXHCI::gMetaClass || parent->getClassSize() != sizeof(AppleUSBXHCI)) {
        snprintf(why, whySize, "the loaded AppleUSBXHCI is not the 21H461 one");
        return false;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        snprintf(why, whySize, "preModLoad failed");
        return false;
    }
    switchxhci_static_init();
    if (OSMetaClass::postModLoad(handle) != KERN_SUCCESS || lookupClass("SwitchXHCI") != &SwitchXHCI::gMetaClass ||
        SwitchXHCI::gMetaClass.getSuperClass() != parent) {
        snprintf(why, whySize, "SwitchXHCI class registration failed");
        return false;
    }
    const uint32_t interrupters = (gHost.xhci(4) >> 8) & 0x7ff;
    IOService *nub = makeNub();
    auto *controller = OSTypeAlloc(SwitchXHCI);
    if (!nub || !controller || !controller->init(nullptr) || !controller->attach(nub)) {
        OSSafeReleaseNULL(controller);
        snprintf(why, whySize, "service setup failed");
        return false;
    }
    if (!controller->start(nub)) {  // the original start may have taken references: the controller is kept
        snprintf(why, whySize, "AppleUSBXHCI::start failed (USBSTS %#x)", gHost.xhci(gHost.capLength() + 4));
        return false;
    }
    __atomic_store_n(&SwitchXHCI::running, controller, __ATOMIC_RELEASE);
    IOLog("SwitchUSB: host controller up (AppleUSBXHCI, %u interrupter%s%s)\n", interrupters,
          interrupters == 1 ? "" : "s", interrupters < kRequiredInterrupters ? ", 3 shown to AppleUSBXHCI" : "");
    return true;
}

namespace {
// The last PP cycle (SwitchUSB.cpp's role line): port, the three PORTSC values, whether PP followed. Written on
// AppleUSBXHCI's work loop, read by the report a second later.
struct PowerCycle {
    unsigned port;
    uint32_t trace[3];
    bool followed;
} gCycle;
}  // namespace

// The pad first (the ID grounded before the port is powered, as tegra_xhci_set_host_mode), then, with the controller
// already running, the OTG port's PP off and on again so the firmware powers it with the pad in the host role. The
// first time, AppleUSBXHCI::start powers the ports itself, after this. Device: ID floating; PP is left on (the XUDC
// ran with it on after every r7w host session).
void switchxhci_set_host_role(bool host)
{
    if (!gHost.map()) return;
    gHost.setHostRole(host);
    SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    if (!host || !controller) return;
    controller->getWorkLoop()->runAction(
        [](OSObject *, void *, void *, void *, void *) -> IOReturn {
            PowerCycle cycle = {gHost.otgPort(), {}, false};
            if (cycle.port) cycle.followed = gHost.cyclePortPower(cycle.port, cycle.trace);
            gCycle = cycle;
            return kIOReturnSuccess;
        },
        controller);
}

void switchxhci_describe(char *buffer, size_t size)
{
    if (!size) return;
    if (!gHost.map()) {
        snprintf(buffer, size, "XUSB not mapped");
        return;
    }
    char pads[400], ports[300];
    gHost.describePads(pads, sizeof(pads));
    gHost.describePorts(ports, sizeof(ports));
    const PowerCycle cycle = gCycle;
    const unsigned host = ioswitch_interrupt_count(xhost::kHostIntid),
                   mailbox = ioswitch_interrupt_count(xhost::kMailboxIntid);
    if (cycle.port) {
        snprintf(buffer, size, "%s; INTID 71 x%u 72 x%u; last PP cycle port %u %#x>%#x>%#x%s; %s", ports, host, mailbox,
                 cycle.port, cycle.trace[0], cycle.trace[1], cycle.trace[2],
                 cycle.followed ? "" : " (PP did not follow)", pads);
    } else {
        snprintf(buffer, size, "%s; INTID 71 x%u 72 x%u; no PP cycle; %s", ports, host, mailbox, pads);
    }
}

void switchxhci_expect_port_change(void)
{
    SwitchXHCI *controller = __atomic_load_n(&SwitchXHCI::running, __ATOMIC_ACQUIRE);
    IOWorkLoop *loop = controller ? controller->tegraLoopForCheck() : nullptr;
    if (loop)
        loop->runAction([](OSObject *owner, void *, void *, void *,
                           void *) { return static_cast<SwitchXHCI *>(owner)->expectPortChange(); },
                        controller);
}
