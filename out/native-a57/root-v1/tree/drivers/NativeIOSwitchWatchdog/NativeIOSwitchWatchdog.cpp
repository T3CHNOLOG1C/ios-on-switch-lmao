// SPDX-License-Identifier: BSD-2-Clause-Patent
// The IOWatchdog that lets watchdogd run. launchd starts watchdogd when an IOWatchdog registers (its LaunchEvents match
// IOProviderClass IOWatchdog), and watchdogd os_crash()es without one. watchdogd is what notices a hung SpringBoard or
// backboardd: it pings their oswatchdog endpoints every 10 s; one that has not answered for 60 s gets a stackshot
// (stacks-*.ips) and is killed, and the third time it ends in a "userspace watchdog timeout" panic, whose panic stackshot
// IOSwitchPanicSave.c keeps.
// watchdogd's two hard requirements on us, each an os_crash() (SIGTRAP) when unmet, and its launchd job is _PanicOnCrash:
//   - its open of this service. Its sandbox names the original classes only: r7p4's watchdogd got "Protobox:
//     watchdogd(50) deny(1) iokit-open-service NativeIOSwitchWatchdog", IOServiceOpen failed, it crashed, and its
//     relaunch's crash panicked. NativeSandboxGrantProcess (IOUserClient.cpp) grants watchdogd the open instead.
//   - an IOService with IOPMUPrimary = 1 (the PMU, where it records the boot stage and the foreground app):
//     NativeIOSwitchPMU below, published with the watchdog.
// Should watchdogd still crash, kern_exit.c takes no panic for it and calls ioswitch_watchdogd_crashed(): userspace
// monitoring is turned off here, so its relaunch reads "disabled" and exits 0, and launchd leaves it down. One line in the
// kernel log; only a hang a running watchdogd reports panics.
// The Switch has no SoC watchdog, so enable/disable/pet have no hardware to drive. The kernel's half of the userspace
// watchdog, the original check that panics when watchdogd stops checking in, runs from one one-shot timer at the check-in
// deadline instead of AppleARMWatchdogTimer's periodic interrupt. Check-ins happen in the original user client, which has
// no hook, but they only move the deadline later (except the first, from start +600 s to now +90 s: covered by never
// arming more than 90 s out), so the timer fires at the deadline it saw and re-arms for the one it finds then: about one
// wake a minute while watchdogd checks in. Nothing is armed before watchdogd connects (a boot where launchd never starts
// it keeps running), after it crashed, or between system sleep and wake, where the deadline is paused like the SoC
// watchdog in SC7.
// A late object (static initializer staged): NativeIOSwitchWatchdogPublish.cpp registers both classes.
#include "OriginalIOWatchdog.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>

class NativeIOSwitchWatchdog;
static NativeIOSwitchWatchdog *gWatchdog;  // set once its start succeeded; never released

class NativeIOSwitchWatchdog : public IOWatchdog {
    OSDeclareDefaultStructors(NativeIOSwitchWatchdog) IOWorkLoop *loop_;
    IOTimerEventSource *timer_;
    IONotifier *power_;
    uint64_t recheck_, overdue_, leeway_;  // 90 s, 1 s, 1 s in mach_absolute_time units
    // Gate-owned (loop_):
    bool connected_;           // watchdogd opened the user client: the deadline is checked
    bool off_;                 // watchdogd crashed: userspace monitoring is off for this boot, nothing armed
    bool asleep_;              // between the system sleep and wake messages: nothing armed
    uint64_t pausedDeadline_;  // the deadline at sleep and the time it had left, 0: none paused
    uint64_t pausedLeft_;

    void arm();
    uint64_t snapshot(bool *monitoring);
    static void expired(OSObject *owner, IOTimerEventSource *timer);
    static IOReturn connectedGated(OSObject *owner, void *, void *, void *, void *);
    static IOReturn crashedGated(OSObject *owner, void *pid, void *signal, void *, void *);
    static IOReturn sleepGated(OSObject *owner, void *, void *, void *, void *);
    static IOReturn wakeGated(OSObject *owner, void *, void *, void *, void *);
    static IOReturn powerMessage(void *target, void *refCon, UInt32 type, IOService *provider, void *argument,
                                 vm_size_t size);

public:
    bool start(IOService *provider) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                           IOUserClient **handler) override;
    void enableWatchdog() override {}
    void disableWatchdog() override {}
    void petWatchdog() override {}
    void daemonCrashed(int pid, int signal);
};
OSDefineMetaClassAndStructors(NativeIOSwitchWatchdog, IOWatchdog)

    bool NativeIOSwitchWatchdog::start(IOService *provider)
{
    clock_interval_to_absolutetime_interval(90, kSecondScale, &recheck_);
    clock_interval_to_absolutetime_interval(1, kSecondScale, &overdue_);
    leeway_ = overdue_;
    loop_ = IOWorkLoop::workLoop();
    timer_ = IOTimerEventSource::timerEventSource(this, &NativeIOSwitchWatchdog::expired);
    if (!loop_ || !timer_ || loop_->addEventSource(timer_) != kIOReturnSuccess) return false;
    // Before it registers: watchdogd's sandbox would refuse its open of a class it does not name.
    setProperty("NativeSandboxGrantProcess", "watchdogd");
    // Registers the service (launchd starts watchdogd) and arms the first check-in deadline.
    if (!ioswitch_original_watchdog_start(this, provider, nullptr, 0)) return false;
    // Userspace monitoring only: no IOKit quiesce watch. Our registry has never been measured quiet within watchdogd's
    // 60 s, and that failure is a boot panic. Cleared before watchdogd can connect (launchd has yet to spawn it).
    clearIOKitMonitoring();
    setProperty("IOKitQuiesceWatchdogEnabled", false);
    gWatchdog = this;
    // Userspace monitoring off for now: thermalmonitord registers with watchdogd and reports itself unhealthy from boot
    // ("Missing sensor(s): TG0B TP1d ..." -- none of its SMC-style sensors exist here), so watchdogd kills it at 60 s
    // and 120 s and panics "no successful checkins from thermalmonitord (2 induced crashes)" at ~199 s on every boot
    // (r7t, r7u). Off as after a watchdogd crash, until thermalmonitord has its sensors: watchdogd reads "disabled" and
    // exits 0.
    off_ = true;
    {
        IOSimpleLock *lock = checkinLock();
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
        setUserspaceMonitoring(false);
        setCheckinDeadline(0);
        IOSimpleLockUnlockEnableInterrupt(lock, state);
    }
    IOLog("IOSwitch watchdog: userspace watchdog off (thermalmonitord has no sensors to report healthy)\n");
    return true;
}

// The original user client (entitlement-checked). watchdogd's first connection starts the deadline checks.
IOReturn NativeIOSwitchWatchdog::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                               OSDictionary *properties, IOUserClient **handler)
{
    const IOReturn opened = IOWatchdog::newUserClient(owningTask, securityID, type, properties, handler);
    if (opened == kIOReturnSuccess) loop_->runAction(&NativeIOSwitchWatchdog::connectedGated, this);
    return opened;
}

// The monitoring flag and the deadline, as the original reads them.
uint64_t NativeIOSwitchWatchdog::snapshot(bool *monitoring)
{
    IOSimpleLock *lock = checkinLock();
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    *monitoring = userspaceMonitoring();
    const uint64_t deadline = checkinDeadline();
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    return deadline;
}

// Gated. The one timer, at the earliest time the original check can act.
void NativeIOSwitchWatchdog::arm()
{
    if (!connected_ || off_ || asleep_) {
        timer_->cancelTimeout();
        return;
    }
    bool monitoring;
    const uint64_t deadline = snapshot(&monitoring), now = mach_absolute_time();
    if (!monitoring) {
        // Turned off by watchdogd (system sleep): its re-enable sets now +90 s without a call into us.
        timer_->wakeAtTime(kIOTimeOptionsWithLeeway, now + recheck_, leeway_);
    } else if (deadline > now) {
        timer_->wakeAtTime(kIOTimeOptionsWithLeeway, deadline - now < recheck_ ? deadline : now + recheck_, leeway_);
    } else if (IOPMRootDomainGetWillShutdown()) {
        // Past due during shutdown: the check's own later limits (91/600 s since the last check-in, 121 s after vfs
        // shutdown) need it re-run every second, as the SoC watchdog's interrupt does. Until the power-off.
        timer_->wakeAtTime(now + overdue_);
    } else {
        // Past due and no panic: defanged (debugger, single user). Looked at again when a check-in could have come.
        timer_->wakeAtTime(kIOTimeOptionsWithLeeway, now + recheck_, leeway_);
    }
}

void NativeIOSwitchWatchdog::expired(OSObject *owner, IOTimerEventSource *timer)
{
    auto *self = static_cast<NativeIOSwitchWatchdog *>(owner);
    ioswitch_original_watchdog_check(self);  // panics if the deadline passed, else returns
    self->arm();
}

IOReturn NativeIOSwitchWatchdog::connectedGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<NativeIOSwitchWatchdog *>(owner);
    if (self->connected_ || self->off_) return kIOReturnSuccess;  // off: the relaunch reads "disabled" and exits
    self->connected_ = true;
    // The root domain's last kernel clients: watchdogd has already turned monitoring off for sleep (and back on at
    // wake) by the time these arrive.
    self->power_ = registerPrioritySleepWakeInterest(&NativeIOSwitchWatchdog::powerMessage, self);
    IOLog("IOSwitch watchdog: watchdogd connected; userspace watchdog armed%s\n",
          self->power_ ? "" : " (no sleep notifications)");
    self->arm();
    return kIOReturnSuccess;
}

// watchdogd crashed (kern_exit.c, which took no panic for it): userspace monitoring off, as watchdogd's own disable
// (method 3) leaves it, so the original check stays quiet and watchdogd's relaunch, asking whether monitoring is enabled
// (method 0), exits 0 instead of reconnecting. Off for the rest of the boot.
IOReturn NativeIOSwitchWatchdog::crashedGated(OSObject *owner, void *pid, void *signal, void *, void *)
{
    auto *self = static_cast<NativeIOSwitchWatchdog *>(owner);
    if (self->off_) return kIOReturnSuccess;
    self->off_ = true;
    IOSimpleLock *lock = self->checkinLock();
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    self->setUserspaceMonitoring(false);
    self->setCheckinDeadline(0);
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    self->timer_->cancelTimeout();
    IOLog(
        "IOSwitch watchdog: watchdogd[%d] crashed (signal %d): no panic for it; userspace watchdog off for this boot\n",
        (int)(intptr_t)pid, (int)(intptr_t)signal);
    return kIOReturnSuccess;
}

void NativeIOSwitchWatchdog::daemonCrashed(int pid, int signal)
{
    loop_->runAction(&NativeIOSwitchWatchdog::crashedGated, this, (void *)(intptr_t)pid, (void *)(intptr_t)signal);
}

// Like the SoC watchdog in SC7: the deadline keeps the time it had left (mach_absolute_time may or may not count sleep).
IOReturn NativeIOSwitchWatchdog::sleepGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<NativeIOSwitchWatchdog *>(owner);
    if (self->asleep_) return kIOReturnSuccess;
    self->asleep_ = true;
    self->timer_->cancelTimeout();
    bool monitoring;
    const uint64_t deadline = self->snapshot(&monitoring), now = mach_absolute_time();
    self->pausedDeadline_ = monitoring && deadline > now ? deadline : 0;
    self->pausedLeft_ = self->pausedDeadline_ ? deadline - now : 0;
    return kIOReturnSuccess;
}

IOReturn NativeIOSwitchWatchdog::wakeGated(OSObject *owner, void *, void *, void *, void *)
{
    auto *self = static_cast<NativeIOSwitchWatchdog *>(owner);
    if (!self->asleep_) return kIOReturnSuccess;
    self->asleep_ = false;
    if (self->pausedDeadline_) {
        // Unless watchdogd changed it meanwhile, the deadline resumes with the time it had left.
        IOSimpleLock *lock = self->checkinLock();
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
        if (self->userspaceMonitoring() && self->checkinDeadline() == self->pausedDeadline_)
            self->setCheckinDeadline(mach_absolute_time() + self->pausedLeft_);
        IOSimpleLockUnlockEnableInterrupt(lock, state);
        self->pausedDeadline_ = self->pausedLeft_ = 0;
    }
    self->arm();
    return kIOReturnSuccess;
}

IOReturn NativeIOSwitchWatchdog::powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t)
{
    auto *self = static_cast<NativeIOSwitchWatchdog *>(target);
    if (type == kIOMessageSystemWillSleep)
        self->loop_->runAction(&NativeIOSwitchWatchdog::sleepGated, self);
    else if (type == kIOMessageSystemWillNotSleep || type == kIOMessageSystemWillPowerOn ||
             type == kIOMessageSystemHasPoweredOn)
        self->loop_->runAction(&NativeIOSwitchWatchdog::wakeGated, self);
    return kIOReturnSuccess;  // acknowledged
}

extern "C" IOService *ioswitch_watchdog_allocate(void)
{
    return new NativeIOSwitchWatchdog;
}

// kern_exit.c: watchdogd ended other than by exit(0). A process context (proc_exit), so the gate may block.
extern "C" void ioswitch_watchdogd_crashed(int pid, int signal)
{
    NativeIOSwitchWatchdog *watchdog = gWatchdog;
    if (watchdog)
        watchdog->daemonCrashed(pid, signal);
    else
        IOLog("IOSwitch watchdog: watchdogd[%d] crashed (signal %d) with no NativeIOSwitchWatchdog: no panic for it\n",
              pid, signal);
}

// The primary PMU watchdogd requires (IOService with IOPropertyMatch {IOPMUPrimary: 1}, else os_crash()). The Switch's
// PMIC (MAX77620) keeps no boot stage across a reset, so this only holds what watchdogd writes: IOPMUBootStage and
// IOPMUBootAppName (the foreground app), readable with ioreg. Its name is not AppleDialogSPMIPMU, which bluetoothd looks
// for; DumpPanic finds it and reads no IOPMUBootError* fault data from it.
class NativeIOSwitchPMU : public IOService {
OSDeclareDefaultStructors(NativeIOSwitchPMU)

    public : bool start(IOService *provider) override;
    IOReturn setProperties(OSObject *properties) override;
};
OSDefineMetaClassAndStructors(NativeIOSwitchPMU, IOService)

    bool NativeIOSwitchPMU::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    setProperty("IOPMUPrimary", 1ULL, 32);  // an OSNumber: watchdogd matches @1, which a boolean would not equal
    setProperty("NativeSandboxGrantProcess", "watchdogd");  // its property writes, as for the watchdog's open
    registerService();
    return true;
}

IOReturn NativeIOSwitchPMU::setProperties(OSObject *properties)
{
    OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict) return kIOReturnBadArgument;
    static const char *const keys[] = {"IOPMUBootStage", "IOPMUBootAppName"};
    unsigned taken = 0;
    for (const char *key : keys) {
        OSObject *value = dict->getObject(key);
        if (!value) continue;
        if (!OSDynamicCast(OSNumber, value) && !OSDynamicCast(OSString, value) && !OSDynamicCast(OSData, value))
            return kIOReturnBadArgument;
        setProperty(key, value);
        ++taken;
    }
    return taken == dict->getCount() ? kIOReturnSuccess : kIOReturnUnsupported;
}

extern "C" IOService *ioswitch_pmu_allocate(void)
{
    return new NativeIOSwitchPMU;
}
