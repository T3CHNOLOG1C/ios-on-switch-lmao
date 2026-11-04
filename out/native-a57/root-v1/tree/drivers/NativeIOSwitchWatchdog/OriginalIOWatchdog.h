// SPDX-License-Identifier: BSD-2-Clause-Patent
// The original 21H461 IOWatchdog (com.apple.driver.AppleARMWatchdogTimer, prelinked as an apfs dependency): the base of
// the SoC watchdog driver, which owns watchdogd's user client (IOWatchdogUserClient: check-in, monitoring on/off, the
// "userspace watchdog timeout" panic) and the kernel's side of the userspace watchdog (600 s for watchdogd's first
// check-in, 90 s after each). Audited against the kernelcache (tools in out/agent-scratch/r9-hang): 0xf8 bytes; its
// vtable is IOService's 168 slots with stop (87) and newUserClient (140) replaced, plus three pure virtuals the hardware
// subclass implements. Nothing here is replaced; original-bindings.S binds every original symbol the subclass uses.
#pragma once
#include <IOKit/IOService.h>

class IOWatchdog : public IOService {
    OSDeclareAbstractStructors(IOWatchdog)
        // The original's fields, set up by ioswitch_original_watchdog_start.
        unsigned char original_[0xf8 - 0x88];

protected:
    // 0xa9: IOKit quiesce monitoring on (what watchdogd's method 9 reports; then it waits 60 s for a quiet registry and
    // the kernel panics on timeout). The start sets it from gIOKitDebug's kIOWaitQuietPanics, on by default.
    void clearIOKitMonitoring() { original_[0xa9 - 0x88] = 0; }

public:
    void stop(IOService *provider) override;  // disableWatchdog(), then IOService::stop
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override;
    virtual void enableWatchdog() = 0;   // slot 168: called by the start below while userspace monitoring is on
    virtual void disableWatchdog() = 0;  // slot 169: called by stop
    virtual void petWatchdog() = 0;      // slot 170: AppleARMWatchdogTimer pets from its own interrupt
};
static_assert(sizeof(IOService) == 0x88 && sizeof(IOWatchdog) == 0xf8, "Pinned original IOWatchdog extent");

extern "C" {
// AppleARMWatchdogTimer::start's call (0xfffffff00663e498): reads the wdt/debug/-s boot-args, arms the 600 s deadline
// for watchdogd's first check-in, publishes the configuration properties, registerService(), enableWatchdog() when
// userspace monitoring is on, and returns IOService::start(provider). Flags 0 keep the defaults.
bool ioswitch_original_watchdog_start(IOWatchdog *watchdog, IOService *provider, void *unused, uint32_t flags);
// The userspace check of AppleARMWatchdogTimer's interrupt (0xfffffff00663ebd4): with interrupts off and the
// watchdog's spinlock held, panics "watchdog timeout: no checkins from watchdogd ..." once the check-in deadline passed.
void ioswitch_original_watchdog_check(IOWatchdog *watchdog);
}
