// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "OriginalIOWatchdog.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void ioswitch_watchdog_static_init(void) __asm__("__GLOBAL__sub_I_NativeIOSwitchWatchdog.cpp");
volatile uint32_t ioswitch_watchdog_published = 0;
volatile uint64_t ioswitch_watchdog_timer_checks = 0;
volatile uint64_t ioswitch_watchdog_pet_calls = 0;
}

class NativeIOSwitchWatchdog : public IOWatchdog {
  OSDeclareDefaultStructors(NativeIOSwitchWatchdog)
  IOWorkLoop *timerLoop = nullptr;
  IOTimerEventSource *timer = nullptr;
  bool enabled = false;
  bool originalStarted = false;
  bool stopping = false; // accessed only while owning timerLoop gate
  static void tick(OSObject *, IOTimerEventSource *);
  static IOReturn armGated(OSObject *, void *, void *, void *, void *);
  static IOReturn cancelGated(OSObject *, void *, void *, void *, void *);
public:
  bool originalReferencesEscaped() const { return originalStarted; }
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;
  void enableWatchdog() override;
  void disableWatchdog() override;
  void petWatchdog() override;
};
OSDefineMetaClassAndStructors(NativeIOSwitchWatchdog, IOWatchdog)

bool NativeIOSwitchWatchdog::start(IOService *provider)
{
  if (timerLoop || originalStarted) return false;
  timerLoop = IOWorkLoop::workLoop();
  if (!timerLoop) return false;
  timer = IOTimerEventSource::timerEventSource(this, &NativeIOSwitchWatchdog::tick);
  if (!timer || timerLoop->addEventSource(timer) != kIOReturnSuccess) return false;
  if (!setProperty("IOSwitchWatchdogImplementation", "XNU software timer") ||
      !setProperty("IOSwitchHardwareResetSupported", kOSBooleanFalse) ||
      !setProperty("IOSwitchKernelHangDetectionSupported", kOSBooleanFalse)) return false;
  setName("NativeIOSwitchWatchdog");
  // Original startup publishes a raw global and can launch a kernel thread.
  // This boot-only provider therefore remains alive even after stop or failure.
  retain();
  originalStarted = true;
  // Flags 0 preserve original userspace and IOKit-quiescence defaults.
  // Original code initializes its locks and deadlines before registerService.
  if (!ioswitch_original_watchdog_start(this, provider, nullptr, 0)) return false;
  if (timerLoop->runAction(&NativeIOSwitchWatchdog::armGated, this) != kIOReturnSuccess) return false;
  IOLog("IOSwitch: original IOWatchdog user client ready; software timer, no hardware reset\n");
  return true;
}

void NativeIOSwitchWatchdog::enableWatchdog()
{
  // Original code may invoke hardware hooks under its interrupt spinlock.
  // Never acquire a sleeping lock or call IOTimerEventSource from these hooks.
  __atomic_store_n(&enabled, true, __ATOMIC_RELEASE);
}
void NativeIOSwitchWatchdog::disableWatchdog()
{
  __atomic_store_n(&enabled, false, __ATOMIC_RELEASE);
}
void NativeIOSwitchWatchdog::petWatchdog()
{
  // The periodic source already runs independently of checkin/pet frequency.
  __atomic_fetch_add(&ioswitch_watchdog_pet_calls, 1, __ATOMIC_RELAXED);
}
void NativeIOSwitchWatchdog::tick(OSObject *owner, IOTimerEventSource *source)
{
  auto *self = static_cast<NativeIOSwitchWatchdog *>(owner);
  if (source != self->timer || self->stopping) return;
  if (__atomic_load_n(&self->enabled, __ATOMIC_ACQUIRE)) {
    __atomic_fetch_add(&ioswitch_watchdog_timer_checks, 1, __ATOMIC_RELAXED);
    // Exact original 600s initial / 90s after-checkin deadline and panic policy.
    ioswitch_original_watchdog_check(self);
  }
  // The timer action owns timerLoop's gate. stop's cancel action owns the same
  // gate, so no already-running callback can rearm after cancellation returns.
  // A temporarily disabled hook leaves the source alive for later enable.
  if (!self->stopping && source->setTimeoutMS(1000) != kIOReturnSuccess)
    panic("IOSwitch watchdog timer rearm failed");
}
IOReturn NativeIOSwitchWatchdog::armGated(OSObject *owner, void *, void *, void *, void *)
{
  auto *self = static_cast<NativeIOSwitchWatchdog *>(owner);
  return self->stopping ? kIOReturnNotReady : self->timer->setTimeoutMS(1000);
}
IOReturn NativeIOSwitchWatchdog::cancelGated(OSObject *owner, void *, void *, void *, void *)
{
  auto *self = static_cast<NativeIOSwitchWatchdog *>(owner);
  self->stopping = true;
  self->disableWatchdog();
  if (self->timer) self->timer->cancelTimeout();
  return kIOReturnSuccess;
}
void NativeIOSwitchWatchdog::stop(IOService *provider)
{
  if (timerLoop) timerLoop->runAction(&NativeIOSwitchWatchdog::cancelGated, this);
  if (originalStarted) IOWatchdog::stop(provider);
}
void NativeIOSwitchWatchdog::free()
{
  // After original startup our permanent retain makes this unreachable.
  // Before that point normal allocation failure cleanup has no callbacks.
  if (timer) {
    timer->cancelTimeout();
    if (timerLoop) timerLoop->removeEventSource(timer);
    timer->release(); timer = nullptr;
  }
  if (timerLoop) { timerLoop->release(); timerLoop = nullptr; }
  IOService::free();
}

static const OSMetaClass *lookupClass(const char *text)
{
  const OSSymbol *name = OSSymbol::withCString(text);
  const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
  OSSafeReleaseNULL(name);
  return meta;
}

extern "C" int ioswitch_watchdog_start(void)
{
  static uint32_t state = 0; // boot-only: 0 untouched, 1 preparing, 2 ready, 3 failed
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&state, &expected, 1, false,
      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return expected == 2 ? 0 : -1;
  // The new class initializer is removed from __mod_init_func by the builder.
  // Register it once, only after the exact original parent is synchronously
  // loaded. No Apple hardware service is instantiated or matched by this call.
  if (OSKext::loadKextWithIdentifier("com.apple.driver.AppleARMWatchdogTimer", false) != KERN_SUCCESS) {
    __atomic_store_n(&state, 3, __ATOMIC_RELEASE); return -1;
  }
  const OSMetaClass *parent = lookupClass("IOWatchdog");
  if (parent != &IOWatchdog::gMetaClass || parent->getClassSize() != sizeof(IOWatchdog)) {
    __atomic_store_n(&state, 3, __ATOMIC_RELEASE); return -1;
  }
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) { __atomic_store_n(&state, 3, __ATOMIC_RELEASE); return -1; }
  ioswitch_watchdog_static_init();
  if (OSMetaClass::postModLoad(handle) != KERN_SUCCESS ||
      lookupClass("NativeIOSwitchWatchdog") != &NativeIOSwitchWatchdog::gMetaClass ||
      NativeIOSwitchWatchdog::gMetaClass.getSuperClass() != parent) {
    __atomic_store_n(&state, 3, __ATOMIC_RELEASE); return -1;
  }
  IOService *provider = IOService::getResourceService();
  auto *watchdog = new NativeIOSwitchWatchdog;
  if (!provider || !watchdog || !watchdog->init() || !watchdog->attach(provider)) {
    if (watchdog) watchdog->release();
    __atomic_store_n(&state, 3, __ATOMIC_RELEASE);
    return -1;
  }
  if (!watchdog->start(provider)) {
    // Once original startup begins its raw references require permanent life.
    if (!watchdog->originalReferencesEscaped()) {
      watchdog->detach(provider);
      watchdog->release();
    }
    __atomic_store_n(&state, 3, __ATOMIC_RELEASE);
    return -1;
  }
  // The allocation reference also remains owned by this boot.
  __atomic_store_n(&ioswitch_watchdog_published, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
  return 0;
}
