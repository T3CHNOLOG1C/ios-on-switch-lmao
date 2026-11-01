# Native SD files — V8

This page records the established V8 file-read baseline. The [V9 block-device/APFS test](native-block.md) adds persistent reader state and ordinary read-only filesystem access; it is [installed, verified and safely ejected](../out/switch/native-xnu-v9-install.json) after local qualification. [Physical V9 APFS success](../out/switch/native-xnu-v9-hardware-result.json) is now established by owner report.

The V7 photograph confirms that card metadata was accepted and native sector reads 0 and 1 completed. It stopped at file stage `3`, error `212`, GPT error `7`, before FAT32 or any file bytes. [Photographed result](../out/switch/native-xnu-v7-hardware-result.json).

The parser required at least 16 KiB of declared GPT entries. Hekate's formatter instead reserves that space but declares and CRCs only the entries it creates. A compact-table fixture reproduces the photographed error, two reads and last LBA `1`. V8 separates those quantities: geometry still requires at least 32 reserved sectors, while CRCs and comparisons cover exactly the declared entry bytes. Both headers/arrays, partition identity, bounds and overlap checks remain mandatory. The actual card entry count remains unknown; V8 displays count and entry size when GPT fails. [Source and independent review](../out/research-native/native-gpt-v8-compatibility-review.json).

**V8 is installed, verified and safely ejected. Physical file access is now established by owner report. The owner wrote `samples verified` and `fat files successfully verified`. [Physical result](../out/switch/native-xnu-v8-hardware-result.json).** All ten installed files passed SHA-256 readback; all 145 protected files are preserved. The four retained media files passed complete SHA-256 checks before writes and retained their filesystem identities afterward. [Installation receipt](../out/switch/native-xnu-v8-install.json). The new package contains ten boot files totaling 68,966,978 bytes. Its updater reused all four existing `IOSWITCH` media files, fully hashes them before any SD writes, and checks their filesystem identity before and after publishing the menu. It copied only the new boot payload and retained all older payloads. [V8 package](../out/native-sd-files-v2-fat32/manifest.json).

The V7 installation previously copied and fully verified 9,449,693,955 bytes in 14 files, preserving all 132 earlier protected files. Its [installation receipt](../out/switch/native-xnu-v7-install.json) remains unchanged.

The same Hekate entry remains **More Configs → IOSwitch Native XNU**. V8 retains native XNU and the own signed PID 1 fixture. It still does not start Apple launchd or iPadOS apps.

## What V8 checks

1. A small ARM64 exporter reads the active card descriptor from the pinned U-Boot's RAM. It checks the firmware instruction layout, object links, initialized SDHC state, OCR and CSD capacity, and requires two matching snapshots. It does not call firmware functions or reinitialize the card.
2. The loader validates the version-2 reference page before copying XNU and again before committing the page. XNU validates it independently, then repeats the working native sector-zero comparison.
3. Native CMD17 reads validate both GPT headers and partition arrays, including their CRCs, bounds and non-overlap. The reader selects only the existing FAT partition UUID `2FBBA66A-A356-00C1-CD9F-1DCBB51C2CA9`.
4. The FAT32 reader validates the volume and mirrored FAT entries, follows bounded cluster chains, and hashes the entire 131,777-byte `IOSWITCH/VERIFY.BIN` fixture.
5. It opens all three disk parts with their exact lengths and checks ten SHA-256 samples totaling 49,152 bytes. These include an unaligned read, the 4,000,000,000-byte split boundary and a logical offset beyond 4 GiB.

The native file test stops after 120 seconds or 131,072 sector callbacks. Each individual command retains its own bounded wait. A transport error blocks further commands for that boot; there is no retry, reset or write command. This is a diagnostic file reader, not yet an XNU block-storage device or an APFS root mount.

## Files and proof boundaries

| SD path | Exact size |
| --- | ---: |
| `IOSWITCH/VERIFY.BIN` | 131,777 bytes |
| `IOSWITCH/OS/PART0000.BIN` | 4,000,000,000 bytes |
| `IOSWITCH/OS/PART0001.BIN` | 2,077,580,800 bytes |
| `IOSWITCH/SYSTEMOS/PART0000.BIN` | 3,303,014,400 bytes |

All individual files fit FAT32. The host verifies every byte during copying and full SHA-256 readback. The Switch hashes the complete small fixture and the selected disk ranges; it does **not** hash all 9.38 GB during each diagnostic boot. [Expected hashes and source provenance](../out/native-files-v1/manifest.json).

The deployed path runs directly on the Switch: Hekate → existing ATF/U-Boot → native loader → XNU. The Mac CPU model is a development test with a separately compiled fake SD adapter. Its sparse media contains real sample bytes and complete relevant FAT/GPT metadata; it does not emulate Tegra hardware.

The firmware exporter executes before final firmware cleanup. XNU never dereferences firmware-owned pointers or calls U-Boot. The exporter's code and private stack occupy the existing excluded staging allocation, outside the kernel's capped 80 MiB staging span. [Exporter build](../out/native-firmware-export-v1c/manifest.json), [AArch64 exporter model](../out/native-firmware-export-v1c-model-run/report.json), [V8 package](../out/native-sd-files-v2-fat32/manifest.json).

## Local verification

V8 standard and Hekate-style compact-table model runs both verify the fixture and all ten real-byte samples and return to actual EL0. The compact case declares 11 entries, reserves 32 sectors, and uses different uncovered tail bytes to test CRC coverage. A 129-entry table still refuses before files; all prior damaged-media and invalid-card controls remain required. All 106 current native host tests and 83 exact V8 boot-script cases pass. [V8 artifact review](../out/research-native/native-xnu-v8-artifact-review.json), [script review](../out/research-native/native-xnu-v8-script-review.json).

The [V8 qualification](../out/native-a57/sd-files-v2/qualification.json) passes 29,442 checks across ten runs, including 27,865 frozen V7 file hashes. Independent review verifies raw model memory/registers, source bytes, CRCs, FAT chains and actual EL0 state. The updater passes 27 tests plus five independent file-change cases. [Final install qualification](../out/research-native/native-xnu-v8-install-review.json).

The retained V7 baseline follows.

[Final qualification](../out/native-a57/sd-files-v1/qualification.json) passes 1,391 checks across eight bounded runs. The successful run verifies all expected fixture/sample bytes; damaged GPT, fixture and cross-part data fail as expected; three invalid card records refuse before kernel copy; the exact native kernel retains signed EL0 with SD disabled. The 103 native host tests, 83 boot-script scenarios and 21 installer tests also pass.

Independent review binds eight recompiled native objects to the final kernel and verifies all 350 PMGR constraints. The model needed a smaller fake-media mapping; its conditional source change produces byte-identical native preprocessed code and object, leaving the deployed kernel unchanged. [Model-only difference](../out/native-a57/sd-files-v1/model-only-delta-review.json), [package/install gate](../out/research-native/native-xnu-v7-install-review.json).

## Physical result

The owner reported `samples verified`, then `fat files successfully verified, switch connected once more`, after running V8. This establishes the native finite file test by owner report. No photograph, timing, exact counter values or actual GPT entry count was provided. The success banner is only drawn for file stage `9` and error `0`, after full fixture hashing and ten selected sample matches. The two disk filesystems were not mounted by XNU; this is FAT32 access to their split backing files. [Recorded source-bound evidence](../out/switch/native-xnu-v8-hardware-result.json).

No repeat of V8 is needed to establish this milestone. V8's reader is deliberately finite and holds stack-owned callback state; its static file structures must not be reused after the probe returns. V9 replaces that lifetime contract with persistent state and indexed disk mappings. It has passed read-only APFS file access in the CPU model; physical APFS success is now owner-reported; writable Data and Apple userspace remain unverified.
