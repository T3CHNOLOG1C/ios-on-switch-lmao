// SPDX-License-Identifier: BSD-2-Clause-Patent
// The IOWatchdog that lets watchdogd run. launchd starts watchdogd when an IOWatchdog registers (its LaunchEvents match
// IOProviderClass IOWatchdog), and watchdogd os_crash()es without one. watchdogd is what notices a hung SpringBoard or
// backboardd: it pings their oswatchdog endpoints, and one that stops answering gets a full stackshot report
// (stacks-*.ips) and is killed; repeated failures end in a "userspace watchdog timeout" panic.
// The Switch has no SoC watchdog, so enable/disable/pet have no hardware to drive. What AppleARMWatchdogTimer's
// interrupt does for the userspace side (its bark, not device polling) runs here from a 1 s timer: the original check
// that panics when watchdogd stops checking in. The timer starts at watchdogd's first connection, so a boot where launchd
// never starts watchdogd loses only the watchdog, not the boot (the original would panic 600 s after start).
// A late object (static initializer staged): NativeIOSwitchWatchdogPublish.cpp registers it.
#include "OriginalIOWatchdog.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

class NativeIOSwitchWatchdog : public IOWatchdog {
    OSDeclareDefaultStructors(NativeIOSwitchWatchdog) IOWorkLoop *loop_;
    IOTimerEventSource *timer_;
    bool connected_;
    static void tick(OSObject *owner, IOTimerEventSource *timer);

public:
    bool start(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override;
    void enableWatchdog() override {}
    void disableWatchdog() override {}
    void petWatchdog() override {}
};
OSDefineMetaClassAndStructors(NativeIOSwitchWatchdog, IOWatchdog)

    bool NativeIOSwitchWatchdog::start(IOService *provider)
{
    loop_ = IOWorkLoop::workLoop();
    timer_ = IOTimerEventSource::timerEventSource(this, &NativeIOSwitchWatchdog::tick);
    if (!loop_ || !timer_ || loop_->addEventSource(timer_) != kIOReturnSuccess) return false;
    // Registers the service (launchd starts watchdogd) and arms the first check-in deadline.
    if (!ioswitch_original_watchdog_start(this, provider, nullptr, 0)) return false;
    // Userspace monitoring only: no IOKit quiesce watch. Our registry has never been measured quiet within watchdogd's
    // 60 s, and that failure is a boot panic. Cleared before watchdogd can connect (launchd has yet to spawn it).
    clearIOKitMonitoring();
    setProperty("IOKitQuiesceWatchdogEnabled", false);
    return true;
}

// The original user client (entitlement-checked). watchdogd's first connection starts the check-in deadline checks.
IOReturn NativeIOSwitchWatchdog::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                               OSDictionary *properties, IOUserClient **handler)
{
    const IOReturn opened = IOWatchdog::newUserClient(owningTask, securityID, type, properties, handler);
    if (opened == kIOReturnSuccess && !__atomic_exchange_n(&connected_, true, __ATOMIC_ACQ_REL)) {
        IOLog("IOSwitch watchdog: watchdogd connected; userspace watchdog armed\n");
        timer_->setTimeoutMS(1000);
    }
    return opened;
}

void NativeIOSwitchWatchdog::tick(OSObject *owner, IOTimerEventSource *timer)
{
    ioswitch_original_watchdog_check(static_cast<NativeIOSwitchWatchdog *>(owner));
    timer->setTimeoutMS(1000);
}

extern "C" IOService *ioswitch_watchdog_allocate(void)
{
    return new NativeIOSwitchWatchdog;
}
