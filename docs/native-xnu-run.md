# Native XNU original-root boot attempt

**The four-core native APFS kernel and separate boot package are built locally; they are not installed or hardware-qualified.** All four A57s are requested, with bounded entry/GIC/timer/IPI/scheduler checks. The latest raw boot still fails before root mounting. Read [HANDOFF.md](../HANDOFF.md) for the exact artifacts, evidence, constraints and next steps.

The latest physical raw boot opened the 32 GiB APFS container and selected checkpoint XID 441, then timed out waiting for the OS UUID and panicked `Failed to mount root device`. No cause has been established. The new kernel adds a raw-only, bounded media/container diagnostic at that existing failure. [Photographed result](../out/switch/native-raw-v24-hardware-result.json). The installed V28 installer and existing single-core entries remain; updating the kernel does not require rerunning the destructive image installer.

V23 fixes overlapping kernel virtual reservations introduced by the full-RAM layout. The actual old enumeration reproduces a fixed-range rejection before the stock console enables; V22's photograph reached stage 21, consistent with that failure. The corrected enumeration preserves the complete reserved union without overlaps or a RAM reduction. Full RAM, 4 KiB pages, required DMA and early fault/panic diagnostics remain. The owner later reported a root-mount failure; the exact preceding output was not captured. [Correction and evidence](native-virtual-regions.md).

V22 corrects the early diagnostic gate for the actual full-RAM/stock-video profile and makes early C panics visible. V21 stopped on COPY COMPLETE by owner report; its disabled diagnostics hid whether XNU entered or faulted. The underlying stop is still unidentified. V22 retains full RAM and required DMA; multicore startup remains pending. [Entry correction and next-screen guide](native-entry.md).

V21 removes the **768 MiB OS memory cap**. XNU now uses both measured RAM banks across the 4 GiB aperture, with live firmware reservations, the framebuffer, kernel and VM metadata kept out of the free queues. The remaining 768 MiB envelope protects only initial placement and temporary mappings. The native 4 KiB page policy remains active at every RAM size. [RAM implementation and checks](native-ram.md), [new loader artifact review](../out/research-native/full-ram-v21/v21-artifact-review.json).

V20's first hardware run stopped at the MC guard before any DMA transfer. V21 corrects the guard: TrustZone-only global registers are diagnostic, while readable MC memory identity and SDMMC1's own bypass state are required. Native data phases still require **64-bit DMA with no PIO fallback**. DMA correctness, speed, upper-bank allocation and graphical startup need the next hardware boot. [DMA evidence](native-dma.md).

V19's latest photograph still shows the console, with completed APFS transactions through XID 550 and flushes around 17–55 ms. It has no visible panic or wrapped iterator warning, but does not establish a successful UI boot. The class-2 protection and indexing warnings also occur in the retained Mac run that reached Home. Actual native swap validation/copy/retirement state is still needed. [V19 hardware record](../out/switch/native-xnu-v19-hardware-result.json), [display comparison](../out/research-native/sol-v19-display-startup-review.md).

V21 retains the V19 fractional-counter correction and idle-cache fix, and the V18 log, vnode, Data and copy fixes. Display type zero means internal display. Inspected 4 KiB contracts pass, with bounded private-driver coverage. [Clock correction](native-timebase.md), [audit](native-audit.md), [4 KiB review](../out/research-native/sol-v18-4k-audit.md).

The separate local raw candidate now has a standard **32 GiB APFS container with System, Data and Preboot roles sharing space**. Complete original Data contents, including staged apps, passed verification: 10,995 entries and 467,599,350 bytes. The unchanged sealed cryptex images are files in Preboot. The image is detached; no Android partition changed. V24 supplies the separate installer and explicit raw boot profile described above. [Candidate](../out/native-single-apfs-trial-v1/candidate.json), [Data verification](../out/native-single-apfs-trial-v1/data-population.json).

The V10 and earlier sections below preserve installation and test history. Their diagnostic banners and authentication claims apply to those builds, not the current V23 profile.

**V10 is installed, verified and safely ejected.** All 12 files passed readback, all 163 tracked existing files are preserved, and the retained 9.38 GB of media passed full SHA-256 verification. [Installation receipt](../out/switch/native-xnu-v10-install.json). This candidate attempts original iPadOS root and Apple startup. OS and SystemOS stay read-only; Data is writable RAM storage and resets at reboot. [Evidence and limitations](native-root.md).

Close Hekate USB storage and choose **More Configs → IOSwitch Native XNU**. Photograph the whole final screen. The native diagnostic shows root, cache, Data and original PID 1 progress. Original PID 1 syscalls establish execution through Apple's loader; they do not establish launchd main, service readiness or the Home Screen. Increasing authenticated-read counters mean the attempt is still progressing. Physical timing is unknown. Hold **POWER for at least 20 seconds** to exit if it stops.

The following sections preserve the earlier V9 and storage-test history; their success banners are not the V10 target.

**V9 native APFS access succeeded by owner report.** The owner reported `apfs files verified - read only`; the installed path requires both read-only mounts and complete 574-byte and 49,152-byte files with matching hashes. No photograph or timing was supplied. [Physical V9 result](../out/switch/native-xnu-v9-hardware-result.json). Root remains the own diagnostic RAM disk; Apple launchd, writable Data and the iPadOS UI are not yet established.

**V9 is installed, verified and safely ejected.** All ten boot files match their hashes and all 154 tracked existing files are preserved. The retained 9.38 GB of media passed complete SHA-256 verification before the new menu was published. [Installation receipt](../out/switch/native-xnu-v9-install.json). It adds persistent read-only block devices and original APFS data mounts while keeping the own signed RAM-disk PID 1. Five model runs establish full file reads, transport refusal, corrupted-file detection, retained signed-code rejection and exact native-kernel EL0 execution with storage disabled. Physical V9 success is now recorded by owner report above. [Current test](native-block.md), [final install qualification](../out/research-native/native-xnu-v9-install-review.json).

The same single menu entry remains **More Configs → IOSwitch Native XNU**. Close Hekate USB storage and select it. Capture the whole final screen. The V9 success banner is **`APFS FILES VERIFIED - READ ONLY`**; the earlier FAT-file banner alone does not establish APFS success. Hold **POWER for at least 20 seconds** to exit. This test reads files through the own signed process; Apple launchd and iPadOS apps are not running.

## Established V8 file-read baseline

**V8 native FAT32 and split-file verification succeeded by owner report.** The owner confirmed `samples verified` and `fat files successfully verified`; no photograph or timing was supplied. That path requires a matching complete fixture hash and ten selected sample hashes. [Physical result](../out/switch/native-xnu-v8-hardware-result.json). V8's payload is retained as the working hardware baseline; its installation verified ten files and preserved 145 protected files. [Installation receipt](../out/switch/native-xnu-v8-install.json), [details](native-files.md).

## V6 sector-zero result

**Physical v6 success reported:** the owner replied `matches!` after running the installed entry. In context this reports the `SD SECTOR 0 MATCHES U-BOOT - READ ONLY` banner, which is reached only after a native 512-byte read and complete byte/CRC equality with the same-boot reference. No photograph or exact register values were supplied. [Physical result](../out/switch/native-xnu-v6-hardware-result.json). General block storage and iPadOS remain unfinished.

V5's [photograph](../out/switch/IMG_5517.png) identifies the refusal: mask `01`, version `0303` (SDHCI 4.00). All seven other readiness checks passed, and native XNU plus the signed EL0 process still worked. The transport stopped before CMD17 or register writes. [Decoded physical result](../out/switch/native-xnu-v5-hardware-result.json).

V6 accepts the two reviewed SDHCI specification values, `2` (3.00) and `3` (4.00). The SD Association specification supports the existing single-block PIO sequence with nonzero 16-bit block count and no DMA or automatic commands. The other seven checks and the command sequence are unchanged. [Compatibility evidence](../out/research-native/native-sd-v6-compatibility-review.json), [independent source review](../out/research-native/native-sd-v6-gate-review.json).

**V6 is installed and safely ejected.** All nine files match their hashes and all 124 protected files are unchanged. The previous v1–v5 payloads remain on SD; the prior menu and receipt are backed up locally. [Installation receipt](../out/switch/native-xnu-v6-install.json), [final package qualification](../out/research-native/native-xnu-v6-install-review.json). Use the same entry: **More Configs → IOSwitch Native XNU**.

Local qualification passes 1,006 checks across 13 bounded CPU-model runs, including the photographed version/power tuple, both accepted versions, unsupported-version refusals, command failures and a deliberate data mismatch. Enabled storage runs use separately compiled fake registers; the exact native kernel reaches EL0 with SD disabled. All 550 host transport scenarios, 66 boot-script cases and 20 updater cases pass. [Model qualification](../out/native-a57/sd-read-v3/qualification.json). The separate physical result above establishes sector-zero success by user report.

For a future run, photograph the entire final screen. Success must say `SD SECTOR 0 MATCHES U-BOOT - READ ONLY`, with error `0`, stage `5`, read words `80` (hexadecimal: 128), equal CRCs and mismatch bytes `0`. The owner has reported this success banner; the individual displayed values have not been independently captured. Hold **POWER for at least 20 seconds** to exit.

`READINESS FAIL MASK` uses these hexadecimal bits:

| Mask bit | Failed check |
| --- | --- |
| `01` | SDHCI version low byte is neither 2 nor 3 |
| `02` | Command/data or transfer activity is present |
| `04` | Card-present/stable bits are absent |
| `08` | Host bus mode is not the expected four-bit mode |
| `10` | Controller power bit is off |
| `20` | Clock enable/stability bits are incomplete |
| `40` | Software-reset register is nonzero |
| `80` | Block length low 12 bits are not 512 |

Multiple failures are ORed. `VERSION / POWER / RESET` packs three 16-bit values: version at bits 32–47, power at bits 16–31, reset at bits 0–15. `CLOCK / HOST / BLOCK` uses the same layout. These are the exact samples used in the decision. Block-size register `7200` is valid: `7000` is the inherited DMA boundary and `0200` is the 512-byte length. V5 preserved V4's gates and added these exact samples and masks; its [installation receipt](../out/switch/native-xnu-v5-install.json) records nine verified files and 116 protected files unchanged.

## V4 package and observed result

V4 retains the signed PID 1 test and adds one bounded, read-only SD sector comparison after that process succeeds. Its Hekate entry remains **More Configs → IOSwitch Native XNU**. The native kernel reads only whole-device sector zero through SDMMC1 and compares all 512 bytes with a same-boot U-Boot reference. It does not mount a filesystem or run Apple userspace.

**V4 status:** installed and safely ejected. All nine files match their hashes; all 108 protected files, including the working v3 payload, remain unchanged. [V4 installation receipt](../out/switch/native-xnu-v4-install.json), [final exact-package qualification](../out/research-native/native-xnu-v4-install-review.json). The physical run reached signed EL0 and then refused SD readiness before CMD17. [Photographed result](../out/switch/native-xnu-v4-hardware-result.json). The [manifest](../out/native-sd-v1-fat32/manifest.json) contains nine files totaling 68,753,824 bytes. The updater retained the v1/v2/v3 payloads, backed up the prior menu locally and changed the same single entry after verifying the new files. [Package review](../out/research-native/native-xnu-v4-artifact-review.json), [66 boot-script cases](../out/research-native/native-xnu-v4-script-review.json), [20 updater cases and review](../out/research-native/native-xnu-v4-updater-review.json).

## V4 result interpretation

Close Hekate USB storage and select the same entry. After `EL0 USERSPACE VERIFIED - PID 1`, success is **`SD SECTOR 0 MATCHES U-BOOT - READ ONLY`**. It must show `SD ERROR=0`, `TRANSPORT STAGE=5`, `READ WORDS=80` (hexadecimal: 128 words), equal reference/read CRCs and `MISMATCH BYTES=0`. `FIRST MISMATCH=FFFFFFFFFFFFFFFF` means no mismatch. A plausible MBR or matching CRC alone is not the success condition; the kernel compares every byte too.

On refusal or failure, it instead shows `SD REFUSED OR FAILED - NO RETRY` or `SD SECTOR 0 MISMATCH - NO RETRY`, with the inherited state and error. Photograph the **whole final screen**, including all SD lines. The attempt is bounded and does not retry or reset the controller. Hold **POWER for at least 20 seconds** to exit. A successful native SD read is still unverified; [storage contracts and remaining work](native-storage.md) distinguish this one-sector test from a FAT reader and root mount.

## Established v3 userspace baseline

The entry remains **More Configs → IOSwitch Native XNU**. V3 adds a small RAM disk with an own signed test process and the original AMFI/signing components needed to execute it. It runs directly after Hekate → ATF → U-Boot → EL1, without Linux or an emulator. This process is a userspace bring-up test; Apple launchd and iPadOS apps are not included.

**V3 installation:** V3 was installed, all nine files match their hashes, all 100 protected files are unchanged, and the SD is safely ejected. The prior v1/v2 payloads remain on SD; the previous menu entry and receipt are backed up locally. [V3 installation receipt](../out/switch/native-xnu-v3-install.json). The user subsequently reported `EL0 USERSPACE VERIFIED - PID 1` on the physical OLED. [Physical userspace result](../out/switch/native-xnu-v3-hardware-result.json). No photograph has been supplied; the successful banner is the reported evidence. V2's native `XNU VM INITIALIZED` and `BSD_INIT ENTERED` result remains the established hardware baseline.

## V3 result to capture

Close USB storage and choose the same entry. A complete success is **`EL0 USERSPACE VERIFIED - PID 1`**, with `COMPLETED TEST SYSCALLS=3`, `GETPID RESULT=1`, `SYSCTL RESULT=0` and `USER PAGE SIZE=1000` (hexadecimal: 4096 bytes). The first two calls must return to userspace, which stores their results before the third call; the kernel then checks those actual user buffers before showing success.

Earlier `EL0 ENTERED` or `EL0 SYSCTL COMPLETED` messages are partial progress. The older `LAUNCHD EXEC ATTEMPT - EL0 NOT ESTABLISHED` stage line remains on screen; the later verified banner and completed results supersede it. Photograph the entire final screen, including any panic or early exception. The success screen remains visible while the test process loops. To exit, hold **POWER for at least 20 seconds**, then return to Hekate normally.

## V3 package and local evidence

The [V3 manifest](../out/native-userspace-v1-fat32/manifest.json) contains nine files totaling 68,746,496 bytes. Its largest file is the 67,944,448-byte kernel window, including the 33,568-byte signed test process and 56-byte trust cache. The new prefix is `switchroot/ioswitch-native-xnu-v3/`; the menu title and entry path are unchanged.

The [paired CPU-model runs](../out/native-a57/userspace-v1/model-pair-review.json) pass 60 checks: the intact program reaches EL0 and completes three system calls; changing only one unused signed padding byte causes an explicit execution security-policy rejection before the first call. Both runs use identical device-tree bytes, seed, kernel, arguments, trust cache and placements. This is a Mac Cortex-A57/GICv2 model result, with the native GIC addresses adapted and native loader gates disabled. The later user-reported OLED success is recorded separately from these model tests.

The prelinked kernel's minimum virtual address requires physical base `0xB207C000`, preserving its 2 MiB congruence. Its 768 MiB window ends at `0xE207C000`. The conservative reservation envelope remains exactly `[0xB2000000, 0xE2200000)`, as in v2. The native package retains all runtime bank, reservation, carveout and stack checks. The original AMFI provider and narrow PMGR ABI bridge passed [350 independent binary checks](../out/native-a57/userspace-v1/pmgr-independent-review-prelink-base.json); the bridge supplies required constant/service behavior, not a Tegra power driver.

For the full iPadOS boot, remaining work includes native SD access, a bounded FAT32/split-file reader, XNU block storage and root-volume validation/mounting, followed by Apple userspace and display/input integration. The [read-only SD transport](native-storage.md) passes 33 register-model cases and is linked to physical MMIO in v4. It has not yet read the actual SD card.

## V2 hardware baseline and historical validation

The seven previous IOSwitch test entries and their payloads were backed up locally and removed. All 84 tracked Hekate, Ubuntu and Android files retained their hashes. [Cleanup receipt and backup](../out/switch/retired-smoke-entries-20261028/receipt.json).

## Installation

V1 was copied and verified, and the user ran it on the OLED. Its check/copy stage took about two minutes, then it remained at `IMAGE COPIED AND VERIFIED - ENTERING KERNEL`. That establishes completed loader checks, not native XNU execution. [User result](../out/switch/native-xnu-v1-user-result.json).

V2 is installed under a separate payload directory using the same single menu entry. All nine files match their package hashes, all 92 protected files (84 existing files plus eight v1 payloads) are unchanged, and the SD was safely ejected. V1 payloads and a local copy of the prior entry are retained for recovery. [V2 installation receipt](../out/switch/native-xnu-v2-install.json). The user subsequently reported `XNU VM INITIALIZED` and `BSD_INIT ENTERED` on the physical OLED. This establishes native XNU execution and entry to BSD initialization. No photograph or timing values have been supplied for this run. [Physical result](../out/switch/native-xnu-v2-hardware-result.json).

## V2 diagnostic screens

Close Hekate USB storage and select the entry. The loader now shows source-check, copy and destination-check percentages plus milliseconds. Physical timing has not been measured; the previous 30-second estimate was withdrawn. Photograph the whole final screen, including the progress rows, green checkpoint squares, and any refusal or exception registers. Hold **POWER for at least 20 seconds** to exit, then start Hekate normally.

- Green squares below the loader banner are written from successive XNU entry checkpoints. Their count narrows an early stop.
- `EARLY XNU EXCEPTION - HALTED` includes the last stage, vector, ESR, FAR, ELR and SCTLR. This is kernel-owned output.
- `IOSWITCH NATIVE XNU DIAGNOSTICS` followed by `XNU VM INITIALIZED` is output drawn by XNU after it enabled its own memory mappings.
- `BSD_INIT ENTERED` is a later kernel checkpoint. The package has no root disk and cannot launch iPadOS.
- `XNU PANIC` with text/registers is a useful kernel result. The diagnostic halts after rendering it.
- `IMAGE COPIED AND VERIFIED — ENTERING KERNEL` alone is still loader output and does not establish XNU execution.
- A loader refusal or an earlier U-Boot stage is also useful. Do not infer kernel execution from it.

## V2 package and validation

[Package manifest](../out/native-xnu-v2-fat32/manifest.json): nine files totaling 21,019,904 bytes. Largest file is the 20,217,856-byte prelaid kernel window, well below FAT32 limits. The package uses the same pinned OLED BL31/BL33 and DTB as the successful handoff diagnostic. Its existing menu entry selects the new v2 prefix. Ubuntu, Android and the Hekate default remain independent.

The kernel is a separate diagnostic derivative, SHA-256 `1c008af1b210918676228be29e2cdcc518deaa03f8d6534cc5176e79e1d153d6`. The previous A57 kernel is unchanged. [Build receipt](../out/native-a57/ioswitch-diag-v2/build-receipt.json). The native [Apple DeviceTree](native-oled-adt.md) contains CPU0, the observed 19.2 MHz counter, actual GICv2 ranges, and required volatile bootstrap data. It contains no model device addresses, storage driver, RAM disk, trust cache or hypervisor marker.

**V2 local verification:** the exact hardware CRC helper matches zlib for 208 alignment/length fixtures and the same fixtures in incremental chunks. Nineteen loader-builder and memory-gate tests pass; the existing layout tests also pass. The new updater passes 20 temporary-filesystem cases, and the packaged boot script passes 49 inert control-flow cases.

The final v2 kernel reaches its own VM/BSD screen in a generic Cortex-A57 model. Two separate fault-injected copies render the correct exception registers before and after MMU enable; the released kernel is not modified by these injections. Early vectors now remain at their physical address until translation is enabled, the CPACR enable has an ISB, and the temporary framebuffer mapping is non-executable Device memory retired before TTBR0 is discarded. [V2 model results](../out/native-a57/ioswitch-diag-v2/model-results.json). These correct architectural/diagnostic gaps; they do not establish the cause of v1's native stop.

**Earlier local verification:** 165 unit tests passed. A generic Cortex-A57/EL3 model exercised copying, boot arguments, XNU translation and kernel entry. A model using the native ADT with only its GIC addresses adapted reached CPU registration, GIC setup and BSD initialization. A separate RAM-disk fixture demonstrated readable kernel panic output and `panic_spin_forever`. The exact native adapter rejected an unexpected entry stack before touching the destination. [Model receipt](../out/research-native/native-xnu-model-review.json).

The tests above are model observations. Native XNU execution is separately established by the user-reported physical screen; Tegra interrupt delivery, BSD completion and complete firmware memory ownership remain unverified.

## V2 conditional memory contract

XNU receives `[0xB2004000, 0xE2004000)`, a 768 MiB candidate window. Checks cover the conservative 2 MiB-rounded envelope `[0xB2000000, 0xE2200000)`. The allocation ledger excludes the original and working FDTs, script, entry adapter/stacks, wrapper, staging buffer, original BL33 image, ramoops, and the entire upper 256 MiB containing framebuffer, U-Boot relocation/heap/stack/DMA/tables and firmware/carveouts. The inactive alternate Hekate payload slot is not used by the selected L4T path; the active BPMP firmware resides in the preserved high carveout.

Before destination writes, the native adapter checks the measured RAM-bank shape, complete FDT reserve table and `/reserved-memory` nodes, required reservations, photographed MC carveout values, retained stack interval, EL1/cache/timer state, source CRC and boot contract. It checks the destination CRC after copying. An unexpected active reservation or changed carveout causes a refusal, without trying to reclaim it. The checks support this bounded experiment; they are not a firmware-certified free-memory allocator.

No authored command writes an OS partition, saves firmware variables, changes the default boot target or resets the console. U-Boot's ordinary handoff still quiesces hardware, and XNU programs its accessible interrupt controller and framebuffer. Hardware faults can stop before readable diagnostics; recovery remains the power-button procedure above.
