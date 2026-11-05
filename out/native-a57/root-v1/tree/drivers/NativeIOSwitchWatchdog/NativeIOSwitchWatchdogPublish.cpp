// SPDX-License-Identifier: BSD-2-Clause-Patent
// Publishes NativeIOSwitchWatchdog.cpp's IOWatchdog and PMU from the board thread (IOSwitchRootDiag.inc), long before
// launchd: load AppleARMWatchdogTimer (prelinked; apfs depends on it) so the IOWatchdog metaclass exists, register our
// classes inside preModLoad/postModLoad, then attach one instance of each to IOResources and start it (the original start
// registers the watchdog). Same staging as IOSwitchAccelStubPublish.cpp. Only when both started does
// ioswitch_watchdog_ready() say so, and kern_exec.c's launchd policy leaves watchdogd enabled; a failure is logged once
// and watchdogd stays disabled for the boot.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include "OriginalIOWatchdog.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void ioswitch_watchdog_static_init(void) __asm__("__GLOBAL__sub_I_NativeIOSwitchWatchdog.cpp");
IOService *ioswitch_watchdog_allocate(void);
IOService *ioswitch_pmu_allocate(void);
int ioswitch_watchdog_publish(void);
int ioswitch_watchdog_ready(void);
}

static volatile int watchdog_ready;  // both services started (read by launchd's exec, kern_exec.c)

int ioswitch_watchdog_ready(void)
{
    return __atomic_load_n(&watchdog_ready, __ATOMIC_ACQUIRE);
}

int ioswitch_watchdog_publish(void)
{
    const kern_return_t loaded = OSKextLoadKextWithIdentifier("com.apple.driver.AppleARMWatchdogTimer");
    const OSSymbol *name = OSSymbol::withCString("IOWatchdog");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (loaded || parent != &IOWatchdog::gMetaClass || parent->getClassSize() != sizeof(IOWatchdog)) {
        IOLog("IOSwitch watchdog: AppleARMWatchdogTimer load 0x%x, IOWatchdog %p size %u\n", loaded, parent,
              parent ? parent->getClassSize() : 0);
        return -1;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return -1;
    ioswitch_watchdog_static_init();
    if (OSMetaClass::postModLoad(handle)) return -1;
    IOService *provider = IOService::getResourceService();
    IOService *watchdog = ioswitch_watchdog_allocate();
    if (!provider || !watchdog || !watchdog->init() || !watchdog->attach(provider)) {
        OSSafeReleaseNULL(watchdog);
        return -1;
    }
    // Never released: once the original start ran, the IOWatchdog is the kext's global and watchdogd's provider.
    if (!watchdog->start(provider)) {
        IOLog("IOSwitch watchdog: start failed\n");
        return -1;
    }
    IOService *pmu = ioswitch_pmu_allocate();
    bool attached = false;
    if (pmu && pmu->init() && (attached = pmu->attach(provider)) && pmu->start(provider)) {
        pmu->release();  // the registry (IOResources) holds it
        __atomic_store_n(&watchdog_ready, 1, __ATOMIC_RELEASE);
        return 0;
    }
    if (attached) pmu->detach(provider);
    OSSafeReleaseNULL(pmu);
    IOLog("IOSwitch watchdog: NativeIOSwitchPMU did not start: watchdogd stays disabled\n");
    return -1;
}
