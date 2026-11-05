# Native watchdog bring-up

**r13: watchdogd runs again.** On its first hardware boot (r7p4, 2026-11-04 22:59) watchdogd crashed 1.5 s after each
launch (pids 50, 131, 184, about 10 s apart), and the third crash panicked through its launchd `_PanicOnCrash`
(`PanicOnConsecutiveCrash`): "watchdogd[184] exited -- exit reason namespace 2 subcode 0x5", SIGTRAP from `os_crash`.
Root cause, from that boot's kernel log (decoded with a rebuilt kernel `dsc`): `Protobox: watchdogd(50) deny(1)
iokit-open-service NativeIOSwitchWatchdog` at 22:59:33.5038, then 0.1 ms later watchdogd's error line (its
`IOServiceOpen failed with error : 0x%x` path) and the crash; the same for pid 131 at 22:59:42.6207. Its sandbox only
names the original watchdog classes. Fixes, all in `drivers/NativeIOSwitchWatchdog` and XNU:

- `NativeSandboxGrantProcess = watchdogd` on the IOWatchdog (IOUserClient.cpp grants the open, as for our other drivers).
- `NativeIOSwitchPMU`: watchdogd's next hard requirement, an IOService with `IOPMUPrimary = 1` (it `os_crash()`es
  without one) where it writes `IOPMUBootStage` and `IOPMUBootAppName`.
- `kern_exec.c` leaves `com.apple.watchdogd` off the launchd disabled list only when both services started
  (`ioswitch_watchdog_ready`).
- `kern_exit.c`: a watchdogd crash takes no panic. Its crash behavior is dropped and the driver turns userspace
  monitoring off, so the relaunch reads "disabled" and exits 0 (launchd keeps it down). One log line.
- A userspace watchdog timeout warm-resets 60 s after the panic (without `ioswitch-panic-reboot`). The panic stackshot is
  kept with the panic record and written as `/private/var/ioswitch/panic-<UTC>.kcdata` next to the `.log`, with copies in
  mobile's CrashReporter (`idevicecrashreport` over USB). `bazel run //deploy:pull_logs` copies both and decodes the
  stackshot with XNU's `kcdata.py` into `.stackshot.ips` and the hung process's `.threads.txt`.

Apple's timeouts are kept: a ping every 10 s; a service silent for 60 s gets a stackshot and is killed (induced crash);
the third time, watchdogd panics "userspace watchdog timeout: no successful checkins from <label> (2 induced crashes)
...". The kernel panics if watchdogd itself stops checking in (600 s after it connects, then 90 s after each check-in).

Everything below is history from before r13.

**V18 keeps watchdogd excluded through original launchd and fixes repeated-boot preference handling.** Existing root-owned regular preferences on the checked writable Data mount are preserved, including launchd rewrites. When absent, the initial watchdog-only policy is written to a same-directory temporary file, read back, fsynced and closed, then published with an exclusive rename. The directory is synced before marker creation. No native watchdog adapter or hardware-reset code is linked. [Installation](../out/switch/native-xnu-v18-install.json), [audit](native-audit.md), [persistent Data](native-persistent-data.md). Focused policy tests pass; physical startup and reboot behavior remain unverified.

The V15 and earlier notes below are historical.

**V15 is installed and safely ejected.** It excludes `com.apple.watchdogd` through original launchd's disabled-job policy in disposable RAM Data. The V14 software watchdog adapter is removed from the link and its startup call is removed. Original Apple job files and executables are unchanged. [Installation receipt](../out/switch/native-xnu-v15-install.json), [build receipt](../out/native-a57/root-v1/passes/native-root-watchdog-policy-v1/build-receipt.json). Physical policy consumption and subsequent startup remain unverified.

The V14 photograph still shows `watchdogd[99]` exiting with signal 5 (`SIGTRAP`) and triggering a critical-service panic. The exact trap PC is absent, so the failing assertion is unknown. The adapter did not resolve it. The console's doubled characters are fixed. [V14 physical evidence](../out/switch/native-xnu-v14-hardware-result.json). V13 showed the same failure category with `watchdogd[97]`; neither photo establishes OOM or SD corruption. [V13 evidence](../out/switch/native-xnu-v13-hardware-result.json).

The pinned HakiOS source avoids this dependency by excluding `watchdog` jobs in `userland/mkjobs.py` (deny list at line 216, filtering at line 290). Its separate timer in `xnu/files/osfmk/arm64/loader_return.c` feeds Qualcomm's firmware watchdog every ten seconds. That mechanism does not implement Apple's `IOWatchdog` user-client contract. The pinned source is commit `88513176b1e96ecf0b764cd62c6f4e6ac71345e3` under `vendor/hakios`.

V15 applies the same service-exclusion strategy using original launchd's supported policy format. Before PID1 execution, the kernel creates `/private/var/db/com.apple.xpc.launchd/disabled.plist` containing only `com.apple.watchdogd: true`, plus an empty `disabled.migrated` marker. Independent review of the pinned launchd confirms these exact paths, dictionary boolean semantics, migration behavior and absence of `ForceEnableHack` on the original job. The existing seed's saved 206-entry creation manifests contain neither file.

The helper is limited to the native RAM-root configuration with `ioswitch-root-boot=1`. It requires the established writable APFS RAM Data device, validates root ownership and modes, refuses existing policy files, checks complete writes and reads, fsyncs and closes the files, and halts boot on failure. The override disappears with RAM Data at reboot and is recreated for this profile. No virtual-machine spoofing or general critical-service panic suppression is added. Source: [kern_exec.c](../kernel/xnu-10063.141.1/bsd/kern/kern_exec.c) (`git log -- kernel/xnu-10063.141.1/bsd/kern/kern_exec.c`).

Focused checks of the actual extracted C helpers passed 88 ASan/UBSan scenarios covering successful creation, wrong mount/ownership, existing files, short I/O, corrupt readback, API failures and reference closure. These use inert host stubs and do not establish launchd runtime behavior. [Policy review receipt](../out/research-native/native-watchdog-policy-review.json).

V14 had retained the original Apple `IOWatchdog` user client and check-in policy through a software-timer subclass. Its static review checked 171 virtual slots and seven original bindings, but the physical boot still trapped. [Historical ABI review](../out/research-native/native-watchdog-adapter-review.json), [original daemon contract](../out/research-native/native-watchdog-userclient-contract.md). V15 excludes both the daemon and that added provider; it supplies no userspace watchdog or hardware-reset watchdog.
