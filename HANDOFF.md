> **Superseded 29 October 2026.** Written by the previous agent; its claims are unverified. See `AUDIT.md` for the current state and the new raw SD driver / `ioswitch-rawsd-v29` boot entry.

# IOSwitch handoff — 29 October 2026

## Start here

The current work is complete as a **local native four-core build and boot package**. It is **not installed or hardware-qualified**. The latest physical raw-APFS boot still fails while waiting for the OS volume UUID. That failure is unresolved; the new build adds a bounded diagnostic for it.

- New kernel: `out/native-a57/root-v1/passes/native-root-smp-v1/kernel.macho`, **52,256,768 bytes**, SHA-256 `5017fe229d1d7ee3be1c76100071f3004dd0a1137110d5b435abb9c10ce785d7`.
- Build receipt and exact frozen sources/objects: `out/native-a57/root-v1/passes/native-root-smp-v1/`.
- Four-core layout and native loader: `out/native-smp-four-layout-v1/`.
- Additive boot package: `out/native-smp-four-boot-v1/`, **8 files / 71,123,263 bytes**. New menu name: **IOSwitch Native APFS 4 cores**.
- Existing installed entries and disk images were not changed during this SMP work. **No SD/device operation, CPU_ON call, VM, emulator or broad test suite was run for this build.**

The user's final request was to finish the current work, then write this project-root `HANDOFF.md`, because they are switching agents. Do not infer that the overall native iPadOS port or graphical startup is complete.

## Goal and standing constraints

Run original iPadOS natively on a Nintendo Switch OLED through **Hekate → existing OLED ATF/U-Boot → native loader → Tegra XNU → iPadOS**. Donor: **iPad7,1 / iPadOS 17.7.11 / 21H461**. The user explicitly requested **all four A57 cores**, not a two-core profile.

- Native hardware work only. Do not restart QEMU/VM/model investigations or run the full test suite. Focused actual-function sanitizer checks and necessary native compilation are acceptable.
- Only one heavy build process at a time; finish source work/review before the combined build. The final combined build has already passed; do not repeat it without a reason.
- Do not rehash/rescan old media, copy unchanged disk images, make full SD/game backups, or touch unrelated Nintendo/CFW/emuMMC/Linux/personal data.
- Do not automatically reboot/reset the Switch. Keep the stock full-screen console and custom HUD off.
- Full measured RAM aperture, 4 KiB kernel/userspace pages and required native SDMMC DMA remain. No PIO fallback in the current raw/full-RAM profile.
- The selected former Android userdata extent is exactly **32 GiB**. The user wanted all of it used by standard iPad-like APFS storage. Do not force unrelated sealed containers into a merge, invent private partition-offset formats, fake crypto success, or loosen root authorization to hide a failure.
- Prefer APIs/logs to UI. Browser work uses Aside when needed. AGENTS instructions require Context7 for current library/API/CLI documentation; refactoring and business-logic review do not require it.

## Physical state and latest failure

**V28 installer** is the last installer copied to the SD, verified and safely ejected: `out/switch/native-installer-button-v28-install.json`. It is **77,168 bytes** and starts after releasing buttons then pressing **VOL UP once** on the erase screen. POWER/holding two buttons is no longer needed; VOL DOWN cancels. Older instructions requiring VOL UP+POWER are historical.

The owner reported that flashing started and then attempted **More Configs → IOSwitch Native APFS**. Photos `out/switch/native-raw-v24-IMG_5534.png` and `...5535.png` show:

1. Original APFS opens `disk0` with **4096-byte blocks × 8,388,608 = 32 GiB**.
2. Container checkpoint **XID 441** is selected.
3. Root waits for UUID **04769D26-2CA0-4825-B443-E2E5F88300F2**.
4. Existing `IOKitBSDInit.cpp` path panics **Failed to mount root device**.

Receipt: `out/switch/native-raw-v24-hardware-result.json`. This proves container discovery/checkpoint reads, not OS-volume publication, a root mount, cryptex attachment, launchd, UI, or complete target readback. The photographed build is **V24 raw entry using the V23 single-core kernel**; none of the new SMP code has run physically.

The last SD operation ended with safe ejection. There has been no subsequent SD access during this work. Do not assume a new USB mount exists. The local prepared APFS image was last recorded detached.

## What the new four-core build changes

The active native sources are in **`out/native-a57/root-v1/tree/`**. This is the accumulated prepared build tree; the project directory itself is not a Git repository. Do not run a fresh generic vendor build and assume it preserves these native changes. The new pass's `source/tree/` snapshot and build receipt identify the exact compiled inputs.

- `drivers/ArmCpu/ArmCpu.cpp`: shared GICv2 setup on the boot CPU; each secondary configures its banked SGI/timer/GICC state. Hardware one-hot target masks are discovered and claimed uniquely. Directed SGIR and complete raw IAR→EOIR handling are retained. Peripheral SPIs target the boot CPU.
- Native startup requires exactly four CPUs, `sheng-smp=1`, `ioswitch-smp=1`, and the known SMC PSCI tuple. Same-boot VERSION/FEATURES/AFFINITY checks precede each single CPU_ON attempt. No blind retry or hotplug/power-down implementation.
- Each secondary has a five-second independent observer requiring physical entry/C arrival, local GIC readiness, timer, self-IPI, scheduler-online acknowledgement, and an explicit boot→peer→boot SGI round trip. Only then is stage 8 published and the qualified-secondary count incremented.
- A boot-owned **20-second aggregate observer** requires **stage 8 for CPUs 1, 2 and 3 AND count 3**. Missing nubs, registration failure and startup-thread failure cannot silently leave a smaller system. Failure to allocate an observer panics.
- `osfmk/arm64/start.s`: separate physical secondary entry, current-EL/context/cache guards, early fault vectors, inherited A57 SMPEN verification without modifying SMPEN, EL2 normalization and masked EL1 entry, counter checks, and isolated 128-byte records. Continues through existing `LowResetVectorBase`/registered XNU CPU data.
- `osfmk/arm64/cpu.c` + `ioswitch_smp_tables.inc` included by `arm/pmap/pmap.c`: publish exact CPU data and bounded final kernel table pages with CVAC/DSB. No guessed cache ranges, MMIO/data-leaf traversal or shared-cache invalidation.
- `arm/arm_init.c`: leave temporary secondary vectors after `machine_set_current_thread`, before clearing the temporary user table. `kern/machine.c`: actual scheduler-online release acknowledgement after processor-running bookkeeping/unlocks.
- `diagnostics/ioswitch/IOSwitchSD.{c,h}` and `IOSwitchDiag.c`: preserve the old single-core path; accept only the explicit four-core native-root configuration. Early console uses configuration only and stays stock-video-only. Late SD setup yields up to 20 seconds for **registered** CPU count four. Registration is not treated as scheduler qualification. `IOSwitchFiles.c` had no CPU-count gate and was unchanged.
- `scripts/native_oled_adt.py` and `scripts/prepare_native_smp_layout.py`: four homogeneous MPIDRs 0–3, shared L2, 19.2 MHz timebase, SMC PSCI. New embedded command has exactly `cpus=4 sheng-smp=1 ioswitch-smp=1 ioswitch-storage=raw`. No reduced-core mode.

The combined build actually recompiles SD/Diag, ArmCpu, the block driver and six XNU units; rebuilds all affected archives; links XNU; then prelinks the unchanged **33-original-component closure**, including the four original DiskImages modules. Full RAM/DMA/early-diagnostic/clock/idle fixes remain. New native symbols are scoped to this diagnostic/native build; the generic nonnative configuration has not been qualified with these active-tree changes.

## New APFS failure diagnostic

Active `drivers/NativeIOSwitchBlock/NativeIOSwitchBlock.{cpp,h}` and `iokit/bsddev/IOKitBSDInit.cpp` now add a **raw-only, one-shot** diagnostic immediately before the existing failed root-wait panic. It reads registry snapshots and existing memory counters only: at most **8 IOMedia + 4 AppleAPFSContainer** rows, bounded strings, actual UUID/BSD/provider/busy/size/whole/writable fields and SD/DMA/publication counters. No storage I/O, extra polling, timeout extension, mount-policy change or authorization change.

Why this is still needed: the bounded source-image metadata review found **no established cause**. NX441's object map resolves OS OID1026 to block15896/XID418 with the exact requested UUID; Data/Preboot also resolve. Checksums checked passed, and every touched metadata block is within a copied sparse extent. That does not prove the installed target bytes or native APFS volume-publication lifecycle.

Read `out/research-native/raw-profile-v24/root-publication-review.md` and `root-publication-metadata.json`. The next hardware log should distinguish no APFS volumes, a volume lacking BSD attachment, a UUID/matching problem, and a latched transport error. Do not patch later cryptex/dyld policies or extend timeouts based on the existing photo: those later stages were not reached.

## Evidence and commands

- Entry/header/native assembly, 32 actual guard cases + 16 ABI offsets, exact patch replay, nonnative-section parity: `out/research-native/native-smp-entry-v1/{REPORT.md,result.json,integration-review.json}`.
- Driver native compile, 67 earlier actual-function cases retained by exact unchanged-body comparison, 15 new aggregate cases: `out/research-native/native-smp-driver-v1/{REPORT.md,result.json}`. Final ArmCpu source SHA `9b9f85c932068483c293564b0573609949cabfce800df31f0bff9418c7a37343`.
- Exact table publication, 20 actual sanitizer cases: `out/research-native/native-smp-v1/table-publication-review.json`. Root patch/native checks: `root-kernel-patch-result.json`, `root-native-check/`.
- SD/console profile/registration gates, native compilation and 517 actual-helper/Diag sanitizer checks: `out/research-native/native-smp-v1/storage-profile-gates/result.json`.
- Root-failure diagnostic, both native units, five actual-helper cases and exact patch replay: `out/research-native/raw-profile-v24/root-wait-diag/{result.json,REVIEW.md}`.
- ADT/profile policy, five focused cases and unchanged default single-core serialization: `out/research-native/native-smp-platform-v1/result.json`.
- Final linked SMP/layout review: `out/research-native/native-smp-entry-v1/final-artifact-review.json` passes. Actual entry PA **0xB51FE800**, 2 KiB-aligned vectors **0xB51FE000**, and four 128-byte records **0xB5860180..0xB5860380** are inside owned executable/data segments below 4 GiB. Eight direct native symbol/branch bindings pass. Final XNU TCR has 40-bit IPS; the loader has seven tables, a 48 KiB stack and ends at **0xA001C000**, below the SD reference at **0xA001F000**.
- Final new-output storage/package review: `out/research-native/native-smp-v1/final-package-storage-review.json`. Frozen-source and native-object bindings, linked root diagnostic, eight new files, legacy wrappers/CRCs and exact embedded four-core/raw arguments pass.
- Combined build source/output receipt: `out/native-a57/root-v1/passes/native-root-smp-v1/build-receipt.json`; logs alongside. Kernel/prelink succeeded, not merely syntax checks.

The build/package commands **have already completed**; their output directories deliberately reject accidental reuse:

```sh
python3 out/research-native/build-native-smp-v1.py --source-frozen
python3 out/research-native/package-native-smp-v1.py
```

`--resume-kernel` exists only for a genuinely interrupted build and verifies frozen snapshots. Do not use it to repeat this successful build. Source patches include `vendor/unixnu/patches/xnu/native-switch-smp-{entry,kernel}.patch`, plus the reviewed storage-profile and raw-diagnostic delta patches in their evidence directories. The complete pass snapshots are the source of truth for this artifact.

## Deployment and next work

1. Review the final artifact receipts in the evidence list. The local package has an independent new prefix `switchroot/ioswitch-native-smp-v1/`, with boot scripts, BL31/BL33, wrapped loader and new kernel window, plus `bootloader/ini/ioswitch-native-smp-v1.ini` published last. It depends on existing V10 `oled-sku3.dtb` and `exporter.uimg`. Existing **IOSwitch Native APFS** V24 and **IOSwitch Native FAT V19** must remain available.
2. There is **no new SMP SD installer script or deployment receipt**. When the owner reconnects Hekate UMS, use the established exact-device/FAT/USB/mount-session checks and additive pending-file/readback/menu-last/sync/eject pattern. Verify only the new small boot package; do not recopy or hash APFS files. The previous raw updater is `scripts/install_native_raw_boot_v24.py`; it has old fixed paths/preconditions and is **not** a ready SMP deployer. Do not rerun old cleanup/install helpers.
3. This update needs boot files only. **Do not rerun the destructive APFS image installer just to change the kernel.** Do not host-flash the raw partition. Do not delete the single-core fallbacks.
4. Physical validation must establish all four cores' entry/GIC/timer/self/cross-IPI/scheduler acknowledgements and then capture the complete APFS failure diagnostic if root still times out. Do not call CPU_ON success or the presence of four ADT nodes hardware SMP success.
5. After root publication/mount succeeds, qualify the existing original DiskImages/cryptex/dyld/Data path, graphical presentation/input, performance and reboot persistence. None is established for the raw profile yet.

## Storage and retained research

- Sparse source: **RAW27.IWI**, six parts, standard **32 GiB** logical APFS container; **9,620,553,728 bytes stored (8.96 GiB)**, **9,418,506,240 bytes copied (8.77 GiB)**, **24,941,232,128 free bytes omitted (23.23 GiB)**. Omitted free blocks intentionally retain prior target bytes. V28 changes only buttons/UI; sparse copying is unchanged. No full-image hash/readback is part of this current installer.
- Former Android userdata: start sector **1,810,368,512**, byte offset **926,908,678,144**, unique GUID **5703F8E9-0032-DF6B-2674-B1A569FFC4EE**. GPT backup is only **4,608 bytes** of declared metadata: `out/switch/native-gpt-before-raw-v24.json`. No game/full-SD backup was made.
- Local candidate: `out/native-single-apfs-trial-v1/ipad-candidate.cdr`. Container **50F8A67F-AFC1-4A1B-BCBB-82162674C67F**; OS **04769D26-2CA0-4825-B443-E2E5F88300F2**; Data **D1B1D053-D0A6-496C-8625-A947A3DA863E**; Preboot **4AA4A761-5138-439D-9A30-D4138F6887FA**. Full original Data population was verified: 10,995 entries / 467,599,350 bytes. Candidate preparation receipts are alongside.
- Outer raw physical store is writable. System mounts read-only; Data and Preboot read-write. Unchanged sealed System/App cryptex files live in Preboot and are exposed through original DiskImages with read-only backing and strict real-vnode/UUID binding. Dyld cache directory is `/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld`. Read-only filesystem mounts and read-only physical media are distinct; do not globally make System writable or invent authentication.
- `out/research-native/sol-community-ports-review.md` covers all seven requested community ports and three upstreams. `sol-switchroot-usb-research.md` covers native USB prerequisites; `sol-switch-smp-audit.md` preserves firmware/SMP research. USB networking/log streaming/SSH is **researched, not implemented**. OLED uses B01 XDCI `0x700D0000`; Qualcomm DWC3 code is not directly portable. Candidate approach: native CDC-ECM BSD ifnet + UDP kernel logs, separate userspace SSH later; no Linux deployment was requested.
- Display type zero was proven to mean internal display. Prior FAT V19 progressed through native APFS writes/transactions, but graphical presentation remains unproved. Clock/idle/4 KiB evidence lives in `docs/native-timebase.md`, `docs/native-audit.md`, `out/research-native/sol-v18-4k-audit.md` and related V19 reports.
- Host free space was approximately **3.8 GiB after this last build**. Avoid duplicate large images/old-media scans. Earlier chronological `out/research-native/ROOT-*-ACTIVE.md` files are historical checkpoints; this handoff supersedes their current-state claims.
