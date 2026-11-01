// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI: the Tegra X1 XUSB host controller under Apple's original AppleUSBXHCI (21H461).
//
// Design. iPadOS 17 on iPad7,1 drives USB host through IOUSBHostFamily + AppleUSBXHCI (generic xHCI:
// rings, slots, ports, hubs, power) with a thin SoC subclass, AppleNubT8011USBXHCI. Once the falcon
// firmware runs, Tegra's host is a plain xHCI 1.0 (Linux drives it with the generic xhci-hcd plus
// XHCI_PLAT | XHCI_SPURIOUS_WAKEUP), so SwitchXHCI is the same kind of thin subclass: it does the Tegra
// bring-up (XusbHost.cpp), hands AppleUSBXHCI a provider nub with the register window and the host
// interrupt, returns the coherency mapper from copyMapper() (UsbDmaMapper.cpp), and keeps what is Tegra's own:
// the falcon mailbox (its own interrupt) and the USB2/SS port pads. Everything above (enumeration,
// IOUSBHostDevice/Interface publication, hubs, HID/mass storage/networking class drivers matching) is
// Apple's unchanged code.
//
// AppleUSBXHCI needs three interrupters (HCSPARAMS1.MaxIntrs >= 3). The interrupt line is shared
// (one GIC SPI, legacy interrupter mode), so when the Tegra firmware reports fewer, SwitchXHCI reports
// three and keeps the register sets of the missing ones in a shadow (regRead32/regWrite32/regWrite64
// overrides, slots 290/292/293). Events of transfers aimed at those interrupters then depend on the
// firmware putting them on ring 0; this is printed once at start so the photo shows which case ran.
//
// Interrupts only: AppleUSBXHCI's own filter owns INTID 71; the mailbox filter is ours (INTID 72). A
// 2 s watchdog panics when IMAN0.IP stays set with the event ring dequeue pointer unmoved (the host
// interrupt is not reaching the CPU) or a mailbox interrupt stays pending with no filter run.
#include "OriginalAppleUSBXHCI.h"
#include "SwitchUSBC.h"
#include "UsbDmaMapper.h"
#include "XusbHost.h"
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
void switchxudc_set_host_owns_port(bool host);     // SwitchXUDC.cpp
// tegra210b01_xusb_firmware, linked by build_switchusbhost.py (empty when none was given).
extern const uint8_t switch_xusb_firmware[] __asm__("_switch_xusb_firmware");
extern const uint8_t switch_xusb_firmware_end[] __asm__("_switch_xusb_firmware_end");
}

namespace {
xhost::Controller gHost;        // mapped once; shared by the role switch and the controller
bool gHostMapped;
IOLock *gRoleLock;

constexpr uint32_t kFalconKHz = 204000;     // CLK_SOURCE_XUSB_FALCON: PLLP 408 MHz / 2 (XusbHost.cpp)
constexpr uint32_t kWatchdogMS = 2000;
constexpr uint32_t kRequiredInterrupters = 3;

bool mapHost()
{
  if (!gHostMapped) gHostMapped = gHost.map();
  return gHostMapped;
}
}

class SwitchXHCI : public AppleUSBXHCI {
  OSDeclareDefaultStructors(SwitchXHCI)

  IOWorkLoop *tegraLoop;                    // mailbox + watchdog; AppleUSBXHCI keeps its own loop
  IOFilterInterruptEventSource *mailbox;
  IOTimerEventSource *watchdog;
  volatile uint32_t mailboxInterrupts;
  uint32_t mailboxSeen, hostSeenErdp;
  bool hostPendingSeen, mailboxPendingSeen;
  // Interrupter fold (see the top comment).
  uint32_t hardwareInterrupters, runtimeOffset;
  uint64_t capBase;                         // AppleUSBXHCI's mapping of the capability registers
  uint32_t shadow[kRequiredInterrupters][8];

  static bool mailboxFilter(OSObject *owner, IOFilterInterruptEventSource *);
  static void mailboxAction(OSObject *owner, IOInterruptEventSource *, int);
  void tick(IOTimerEventSource *);
  uint32_t *shadowFor(uint64_t address);

public:
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;
  void free() override;
  uint32_t regRead32(uint64_t base, uint32_t offset) override;
  void regWrite32(uint64_t base, uint32_t offset, uint32_t value) override;
  void regWrite64(uint64_t base, uint32_t offset, uint64_t value) override;
  // The coherency mapper (UsbDmaMapper.h) for every DMA command and buffer of the controller.
  IOMapper *copyMapper() override
  {
    IOMapper *mapper = switch_usb_dma_mapper();
    if (mapper) mapper->retain();
    return mapper;
  }
};
OSDefineMetaClassAndStructors(SwitchXHCI, AppleUSBXHCI)

bool SwitchXHCI::start(IOService *provider)
{
  hardwareInterrupters = (gHost.xhci(4) >> 8) & 0x7ff;
  runtimeOffset = gHost.runtimeOffset();    // read once: regRead32 also runs in the primary interrupt filter
  // As AppleNubT8011USBXHCI::start: ring buffers without the commpage-selected cache mode (our mapper
  // decides their cacheability).
  ringBufferOptions() &= ~0xf00u;

  tegraLoop = IOWorkLoop::workLoop();
  watchdog = tegraLoop ? IOTimerEventSource::timerEventSource(this,
      OSMemberFunctionCast(IOTimerEventSource::Action, this, &SwitchXHCI::tick)) : nullptr;
  if (!watchdog || tegraLoop->addEventSource(watchdog) != kIOReturnSuccess) {
    IOLog("SwitchXHCI: work loop setup failed\n");
    return false;
  }
  IOService *nub = ioswitch_interrupt_nub(xhost::kMailboxIntid, false);
  mailbox = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchXHCI::mailboxAction,
      &SwitchXHCI::mailboxFilter, nub, 0);
  if (!mailbox || tegraLoop->addEventSource(mailbox) != kIOReturnSuccess) {
    panic("SwitchXHCI: cannot attach the XUSB mailbox interrupt (INTID %u)", xhost::kMailboxIntid);
  }
  mailbox->enable();

  if (hardwareInterrupters < kRequiredInterrupters) {
    IOLog("SwitchXHCI: Tegra xHCI has %u interrupter(s), AppleUSBXHCI needs %u: folding onto ring 0\n",
          hardwareInterrupters, kRequiredInterrupters);
  }
  if (!AppleUSBXHCI::start(provider)) {
    IOLog("SwitchXHCI: AppleUSBXHCI::start failed (USBSTS %#x)\n", gHost.xhci(gHost.capLength() + 4));
    return false;
  }
  // xhci-tegra.c probe_finish: firmware messages on, then USBCMD.EU3S (bit 11, MFINDEX stops in U3).
  if (!gHost.sendMailbox(xhost::MBOX_MSG_ENABLED, 0)) IOLog("SwitchXHCI: falcon did not take MSG_ENABLED\n");
  if (capBase) {
    const uint32_t usbcmd = gHost.capLength();
    regWrite32(capBase, usbcmd, regRead32(capBase, usbcmd) | 1u << 11);
  }
  watchdog->setTimeoutMS(kWatchdogMS);
  IOLog("SwitchXHCI: started\n");
  return true;
}

void SwitchXHCI::stop(IOService *provider)
{
  if (watchdog) watchdog->cancelTimeout();
  if (mailbox) mailbox->disable();
  AppleUSBXHCI::stop(provider);
}

void SwitchXHCI::free()
{
  if (tegraLoop) {
    if (watchdog) tegraLoop->removeEventSource(watchdog);
    if (mailbox) tegraLoop->removeEventSource(mailbox);
  }
  OSSafeReleaseNULL(watchdog);
  OSSafeReleaseNULL(mailbox);
  OSSafeReleaseNULL(tegraLoop);
  AppleUSBXHCI::free();
}

bool SwitchXHCI::mailboxFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
  if (!gHost.takeMailboxInterrupt()) return false;
  __atomic_fetch_add(&static_cast<SwitchXHCI *>(owner)->mailboxInterrupts, 1, __ATOMIC_RELAXED);
  return true;
}

void SwitchXHCI::mailboxAction(OSObject *, IOInterruptEventSource *, int)
{
  if (!gHost.serviceMailbox(kFalconKHz)) {
    char state[200];
    gHost.describe(state, sizeof(state));
    panic("SwitchXHCI: XUSB falcon firmware hang (mailbox SMI FW_HANG): %s", state);
  }
}

// Lost-interrupt checks only; no event is ever serviced here.
void SwitchXHCI::tick(IOTimerEventSource *)
{
  const uint32_t rt = gHost.runtimeOffset();
  const bool hostPending = (gHost.xhci(rt + 0x20) & 1) && (gHost.xhci(rt + 0x20) & 2);   // IMAN0 IP && IE
  const uint32_t erdp = gHost.xhci(rt + 0x38);
  if (hostPending && hostPendingSeen && erdp == hostSeenErdp) {
    char state[200], gic[192];
    gHost.describe(state, sizeof(state));
    ioswitch_interrupt_describe(xhost::kHostIntid, gic, sizeof(gic));
    panic("SwitchXHCI: xHCI interrupt lost (INTID %u): %s, %s", xhost::kHostIntid, state, gic);
  }
  hostPendingSeen = hostPending;
  hostSeenErdp = erdp;

  const uint32_t taken = __atomic_load_n(&mailboxInterrupts, __ATOMIC_RELAXED);
  const bool mailboxPending = gHost.mailboxPending();
  if (mailboxPending && mailboxPendingSeen && taken == mailboxSeen) {
    char state[200], gic[192];
    gHost.describe(state, sizeof(state));
    ioswitch_interrupt_describe(xhost::kMailboxIntid, gic, sizeof(gic));
    panic("SwitchXHCI: XUSB mailbox interrupt lost (INTID %u): %s, %s", xhost::kMailboxIntid, state, gic);
  }
  mailboxPendingSeen = mailboxPending;
  mailboxSeen = taken;
  watchdog->setTimeoutMS(kWatchdogMS);
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
  if (!capBase && offset == 4) capBase = base;          // start reads the capability registers first
  if (uint32_t *s = shadowFor(base + offset)) return *s;
  uint32_t value = AppleUSBXHCI::regRead32(base, offset);
  if (capBase && base + offset == capBase + 4 && hardwareInterrupters < kRequiredInterrupters) {
    value = (value & ~(0x7ffu << 8)) | kRequiredInterrupters << 8;
  }
  return value;
}

void SwitchXHCI::regWrite32(uint64_t base, uint32_t offset, uint32_t value)
{
  if (uint32_t *s = shadowFor(base + offset)) { *s = value; return; }
  AppleUSBXHCI::regWrite32(base, offset, value);
}

void SwitchXHCI::regWrite64(uint64_t base, uint32_t offset, uint64_t value)
{
  if (uint32_t *s = shadowFor(base + offset)) { s[0] = (uint32_t)value; s[1] = (uint32_t)(value >> 32); return; }
  AppleUSBXHCI::regWrite64(base, offset, value);
}

// ---------------------------------------------------------------------------
// Role switching. The XUDC and the host share USB2 pad 0; xusb-tegra210.c routes it by OTG capability
// plus ID/VBUS overrides. Host: the XUDC lets go first (its pull-up off), then ID grounded. Device:
// ID floating, VBUS override on, then the XUDC reconnects if its family wants the bus.
extern "C" void switchxhci_set_role(bool host)
{
  IOLock *lock = __atomic_load_n(&gRoleLock, __ATOMIC_ACQUIRE);
  if (!lock) {
    IOLock *fresh = IOLockAlloc(), *none = nullptr;
    if (!fresh) return;
    if (__atomic_compare_exchange_n(&gRoleLock, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) lock = fresh;
    else { IOLockFree(fresh); lock = none; }
  }
  if (!mapHost()) { IOLog("SwitchXHCI: MMIO map failed\n"); return; }
  IOLockLock(lock);
  if (host) switchxudc_set_host_owns_port(true);
  gHost.setPortRole(host ? xhost::PortRole::Host : xhost::PortRole::Device);
  if (!host) switchxudc_set_host_owns_port(false);
  IOLockUnlock(lock);
}

// ---------------------------------------------------------------------------
// Late registration (as switchxudc_start): SwitchXHCI's metaclass needs the loaded original AppleUSBXHCI.
namespace {
const OSMetaClass *lookupClass(const char *text)
{
  const OSSymbol *name = OSSymbol::withCString(text);
  const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
  OSSafeReleaseNULL(name);
  return meta;
}

// Provider nub: register window index 0 and the host interrupt as index 0 (the GIC nub's own interrupt
// properties, so IOService resolves it through ArmGIC). The mapper comes from copyMapper(), not from an
// "iommu-parent" property: an IOService object in a property would not serialize for ioreg.
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
}

extern "C" int switchxhci_start(bool superSpeed)
{
  static uint32_t state;  // 0 untouched, 1 starting, 2 running, 3 failed
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return expected == 2 ? 0 : -1;
  auto fail = [](const char *why) { IOLog("SwitchXHCI: %s\n", why); __atomic_store_n(&state, 3, __ATOMIC_RELEASE); return -1; };
  char why[160] = "";

  if (!mapHost()) return fail("MMIO map failed");
  if (!gHost.smmuBypassed(why, sizeof(why))) return fail(why);
  if (!gHost.powerOn()) return fail("XUSB host power-up failed");
  const size_t firmwareBytes = (size_t)(switch_xusb_firmware_end - switch_xusb_firmware);
  if (!gHost.loadFirmware(firmwareBytes ? switch_xusb_firmware : nullptr, firmwareBytes, why, sizeof(why))) return fail(why);
  if (superSpeed) {
    if (!gHost.superSpeedUp()) IOLog("SwitchXHCI: SuperSpeed lane bring-up failed, USB2 only\n");
  } else {
    gHost.mapFakeSuperSpeedPort();
  }

  static const char *const kRequired[] = {"com.apple.iokit.IOUSBHostFamily", "com.apple.driver.usb.AppleUSBXHCI"};
  for (const char *kext : kRequired) {
    if (OSKext::loadKextWithIdentifier(kext, false) != KERN_SUCCESS) return fail("IOUSBHostFamily/AppleUSBXHCI did not load");
  }
  // Class drivers: hubs, composite devices, HID. Their personalities match the devices AppleUSBXHCI
  // publishes; a missing one only loses that class.
  static const char *const kClasses[] = {"com.apple.driver.usb.AppleUSBHub", "com.apple.driver.usb.AppleUSBHostCompositeDevice",
                                         "com.apple.driver.usb.IOUSBHostHIDDevice", "com.apple.driver.AppleUSBHostMergeProperties"};
  for (const char *kext : kClasses) {
    if (OSKext::loadKextWithIdentifier(kext, false) != KERN_SUCCESS) IOLog("SwitchXHCI: %s did not load\n", kext);
  }
  const OSMetaClass *parent = lookupClass("AppleUSBXHCI");
  if (parent != &AppleUSBXHCI::gMetaClass || parent->getClassSize() != sizeof(AppleUSBXHCI))
    return fail("original AppleUSBXHCI mismatch");
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) return fail("preModLoad failed");
  switchxhci_static_init();
  if (OSMetaClass::postModLoad(handle) != KERN_SUCCESS || lookupClass("SwitchXHCI") != &SwitchXHCI::gMetaClass ||
      SwitchXHCI::gMetaClass.getSuperClass() != parent)
    return fail("SwitchXHCI registration failed");

  IOMapper *mapper = switch_usb_dma_mapper();
  IOService *nub = mapper ? makeNub() : nullptr;
  auto *controller = new SwitchXHCI;
  if (!nub || !controller || !controller->init(nullptr) || !controller->attach(nub)) {
    OSSafeReleaseNULL(controller);
    return fail("service setup failed");
  }
  // Once the original start ran, the family holds references: never released.
  if (!controller->start(nub)) return fail("start failed");
  __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
  return 0;
}
