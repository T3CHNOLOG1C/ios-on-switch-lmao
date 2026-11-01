# Run the installed native OLED CPU probe

## Confirmed result: CPU probe v3 passed

The supplied [v3 photograph](../out/switch/IMG_5506.png) shows `COMPLETE` and a zero return from the native ARM64 probe. All 64 displayed words were independently decoded into [32 named 64-bit fields](../out/switch/native-probe-v3-hardware-result.json). The result identifies Cortex-A57 r1p1, EL2, enabled MMU/data/instruction caches, and a firmware-reported 19.2 MHz architectural counter frequency. It also confirms no PAN or LSE, no 16 KiB translation granule, and `HCR_EL2.RW=0`.

This is native execution through Hekate/ATF/U-Boot, before OS cleanup. It does not prove a kernel handoff, a free RAM window, or XNU/iPadOS execution. The next hardware step is the separate [Memory Check](native-memory-check.md).

## Earlier step: installing CPU probe v3

The supplied [v2 screen photo](../out/switch/IMG_5505.png) confirms visible script execution, successful screen selection and matching firmware-helper instruction checks. It stopped at `IMAGE_SIZE_FAILED` before any CPU probe launch. The SD file remained exactly 832 bytes. This firmware's FAT wrapper reports the requested nonzero read length even when the actual read reaches EOF, so v2's 833-byte read could not determine file size.

**IOSwitch CPU probe v3** was installed and verified before the successful run above. V3 first uses `fatsize` to require exactly 832 bytes, then requests exactly 832 and verifies the fixed checksum. The ARM64 probe and firmware remained unchanged. [Installation receipt](../out/switch/native-probe-v3-install.json), [review and 16 modeled cases](../out/research-native/probe-v3-reviewed-final.json).

To leave the persistent screen, hold **POWER for at least 20 seconds**, then start Hekate normally.

## Earlier screen check and v2 preparation

The first **IOSwitch CPU probe** attempt produced a brief flash and retained Hekate logo, with no readable result. All seven files remained intact. Source inspection found that this firmware routes ordinary stdout to serial only; the original `echo` messages and `md.l` output could therefore be invisible. This does not establish whether the CPU probe executed.

Two new entries were copied through the user's USB connection and verified, totaling 885,237 bytes across twelve files. The existing 17 inspected configuration/reference/probe files were unchanged. USB storage was safely ejected. [USB installation receipt](../out/switch/native-probe-usb-v2-install.json).

1. Close USB storage on the Switch, then select **More Configs → IOSwitch Screen Check**.
2. Confirm that **HEARTBEAT A** and **HEARTBEAT B** alternate once per second. Send a photo and report whether it changes. This script uses the firmware's direct video commands and launches no CPU payload.
3. Hold **POWER for at least 20 seconds** to leave the persistent screen, then return to Hekate normally.
4. After visible output is confirmed, **More Configs → IOSwitch CPU probe v2** is ready without another USB transfer. It explicitly selects `vidconsole` and prints stages before loading, cache helpers and native execution. Capture the last stage or the result and hexadecimal rows.

The new paths are `switchroot/ioswitch-display/`, `bootloader/ini/ioswitch-display.ini`, `switchroot/ioswitch-probe-v2/`, and `bootloader/ini/ioswitch-probe-v2.ini`. The original failed-observation package remains unchanged for comparison.

## Original probe installation

The separate **IOSwitch CPU probe** entry contains 445,829 bytes across seven files. This runs a small native ARM64 register test through the existing firmware; it does not start XNU or iPadOS. Prefer the new entries above.

The installer added only `switchroot/ioswitch-probe/` and `bootloader/ini/ioswitch-probe.ini`. The existing Hekate settings, Linux/Android entries and inspected Noble firmware files have matching before/after hashes. All seven new files also matched the reviewed package after unmounting and mounting FAT32 read-only again. The partition is unmounted now. No reboot has been performed.

## On the Switch

1. Save any open Linux work and return to Hekate using your usual method. If Hekate automatically selects an OS, hold **VOL−** during Hekate startup to cancel autoboot.
2. Open **More Configs** and select **IOSwitch CPU probe**.
3. The script should show **IOSwitch native probe status: COMPLETE**, followed by hexadecimal rows starting at `a0000200`. Photograph the status and all result rows, then send the image in this chat. A failure status or a black screen is also useful evidence; it is not a successful probe.
4. The result screen deliberately stays open. Hold **POWER for at least 20 seconds** to turn the console off, then use the usual Hekate startup method. The existing OS entries remain available.

There is no timed reboot in the probe script. The script's success/refusal paths hold on screen; firmware failure before the script starts, a CPU exception, or manually interrupting U-Boot can behave differently. Display and recovery are still awaiting this first hardware test.

## What the result establishes

The test reads CPU identification, current exception level, timer frequency and MMU control registers. It returns to U-Boot after writing only its own small memory buffer. It performs no storage write, device-register write, exception-level change or firmware call.

This observes U-Boot's current environment. A later kernel-handoff test must capture the state after firmware cleanup before XNU can take ownership. A `COMPLETE` result is not a native iPadOS boot.

## Evidence and removal

- [Package and independent review](../out/native-probe-review-receipt.json)
- [Installer review](../out/research-native/installer-reviewed-final.json)
- [Installation receipt](../out/switch/native-probe-install.json)
- [Read-only remount verification](../out/switch/native-probe-persistence.json)
- [Native boot contract](native-boot.md)

To remove this experiment later, delete only `bootloader/ini/ioswitch-probe.ini` and the `switchroot/ioswitch-probe/` directory from the SD card. This does not require editing the main Hekate INI or formatting anything.
