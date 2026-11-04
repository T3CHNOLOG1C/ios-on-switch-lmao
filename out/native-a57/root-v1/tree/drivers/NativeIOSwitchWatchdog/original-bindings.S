// Original IOWatchdog imports of NativeIOSwitchWatchdog (21H461 com.apple.driver.AppleARMWatchdogTimer), at the
// kernelcache address + prelink slide (-0x1000000). Audited in out/agent-scratch/r9-hang (vtable slots 0/1 brk, 7
// getMetaClass, 87 stop, 140 newUserClient, 168-170 pure; the rest IOService's). The vtable is for -fapple-kext's
// qualified calls (IOWatchdog::newUserClient goes through the original's slot 140).
.globl __ZN10IOWatchdog10gMetaClassE
.set __ZN10IOWatchdog10gMetaClassE, 0xfffffff006b2d268
.globl __ZTV10IOWatchdog
.set __ZTV10IOWatchdog, 0xfffffff005f247d8
.globl __ZN10IOWatchdogC2EPK11OSMetaClass
.set __ZN10IOWatchdogC2EPK11OSMetaClass, 0xfffffff00563e400
.globl __ZN10IOWatchdogD2Ev
.set __ZN10IOWatchdogD2Ev, 0xfffffff00563e420
.globl __ZN10IOWatchdog4stopEP9IOService
.set __ZN10IOWatchdog4stopEP9IOService, 0xfffffff00563eb6c
.globl __ZN10IOWatchdog13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient
.set __ZN10IOWatchdog13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient, 0xfffffff00563ee14
.globl _ioswitch_original_watchdog_start
.set _ioswitch_original_watchdog_start, 0xfffffff00563e498
.globl _ioswitch_original_watchdog_check
.set _ioswitch_original_watchdog_check, 0xfffffff00563ebd4
