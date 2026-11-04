// SPDX-License-Identifier: BSD-2-Clause-Patent
// Publishes NativeIOSwitchWatchdog.cpp's IOWatchdog from the board thread (IOSwitchRootDiag.inc), long before launchd:
// load AppleARMWatchdogTimer (prelinked; apfs depends on it) so the IOWatchdog metaclass exists, register our subclass
// inside preModLoad/postModLoad, then attach one instance to IOResources and start it (the original start registers it).
// Same staging as IOSwitchAccelStubPublish.cpp. A failure is logged and leaves watchdogd unlaunched, as before.
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
int ioswitch_watchdog_publish(void);
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
    return 0;
}
