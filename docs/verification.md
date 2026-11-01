# Verification — 29 October 2026

**V9 native APFS access succeeded by owner report.** The owner reported `apfs files verified - read only`; the installed path requires both read-only mounts and complete 574-byte and 49,152-byte files with matching hashes. No photograph or timing was supplied. [Physical V9 result](../out/switch/native-xnu-v9-hardware-result.json). Root remains the own diagnostic RAM disk; Apple launchd, writable Data and the iPadOS UI are not yet established.

## Current native status

**V9 is installed and verified; physical APFS success is recorded above.** The [installation receipt](../out/switch/native-xnu-v9-install.json) records ten boot files totaling 69,098,050 bytes, all 154 tracked existing files preserved, and 9,380,726,977 retained media bytes fully SHA-256 verified before writes. New files were rehashed after sync before safe ejection. The final build passes 56,479 checks across five model runs and the preserved evidence inventory. Original APFS discovers both persistent block devices, and the own signed PID 1 mounts both volumes read-only, reads complete 574-byte and 49,152-byte files, checks EOF/close and matches both hashes. The other four runs establish bounded unavailable-volume failure after a transport error, rejection of changed file contents, exact native-kernel EL0 with storage disabled, and rejection of a modified signed fixture before storage. [Qualification](../out/native-a57/sd-block-v1/qualification.json), [independent raw-run review](../out/research-native/native-xnu-v9-model-independent-review.json).

The enabled-storage model uses a separate compiled adapter and a host bridge backed by the actual images; it does not emulate Tegra hardware. The native build retains SDMMC1 PIO and contains no model bridge. Independent checks bind the C/C++ objects, original module bindings, 83 boot-script scenarios, 59 host reader/backend tests and 35 updater/eject cases to the final package. [Install qualification](../out/research-native/native-xnu-v9-install-review.json), [contracts and run details](native-block.md). Authenticated root-volume boot and Apple userspace remain unverified.

## Earlier physical and local evidence

**V8 physical FAT32/split-file success is established by owner report.** The owner wrote `samples verified` and `fat files successfully verified`, then reconnected the card. This reports the success path after the complete fixture hash and all ten selected disk-range hashes. No photograph, elapsed time or exact displayed counters were provided. It establishes finite native file reading, not a persistent block-device service, APFS mount, full native disk hash or iPadOS boot. [Source-bound hardware record](../out/switch/native-xnu-v8-hardware-result.json).

**V8 local qualification passes:** 106 native host tests, 83 boot-script scenarios, 27 updater tests plus five independent file-change cases, and ten CPU-model runs. Standard and compact GPT tables both verify the 131,777-byte fixture and all ten selected disk ranges. Invalid table shape, damaged media and invalid card records still refuse. The 29,442 qualification checks include preservation hashes for all 27,865 frozen V7 files. [Qualification](../out/native-a57/sd-files-v2/qualification.json), [independent saved-run review](../out/research-native/native-xnu-v8-model-review.json), [host tests](../out/research-native/native-files-v8-host-tests.log), [final install qualification](../out/research-native/native-xnu-v8-install-review.json).

**V7 physical result:** the supplied photograph establishes accepted SDHC metadata, native reads of sectors 0 and 1, and retained EL0 PID 1. It stopped at stage `3`, error `212`, GPT error `7`, with no file bytes verified. The former 16 KiB declared-array minimum rejects Hekate's compact entry counts; a matching fixture reproduces the refusal. V8 requires that minimum for reserved space while CRC-checking exactly the declared bytes. The actual card count remains unobserved. [Photograph and interpretation](../out/switch/native-xnu-v7-hardware-result.json), [independent GPT review](../out/research-native/native-gpt-v8-compatibility-review.json). V8 is installed with all ten new files verified, 145 protected files preserved and safe ejection confirmed. Its 9,380,726,977 retained media bytes were fully hashed before writes and not recopied; metadata identity was checked before publication, after sync and before eject. Physical V8 file access subsequently succeeded by owner report, as recorded above. [Installation receipt](../out/switch/native-xnu-v8-install.json). [Current run instructions](native-files.md).

**V6 physical sector-zero success is now established by user report.** The owner replied `matches!` to the expected success banner. Its source requires a completed native 128-word read and equality of all 512 bytes plus CRC against the same-boot firmware reference. No photograph or exact register values were supplied. [Physical result](../out/switch/native-xnu-v6-hardware-result.json). V7 subsequently establishes accepted capacity metadata and sector-1 addressing. Sustained file reads, FAT32/root mounting and Apple userspace remain unverified.

The [V5 photograph](../out/switch/IMG_5517.png) confirms signed EL0 PID 1 and a pre-command SD refusal: error 3, stage 1, mask 1, version `0303`. Only the SDHCI version check failed; the other seven checks passed. [Decoded physical result](../out/switch/native-xnu-v5-hardware-result.json). V6 accepts specification bytes 2 and 3 based on the original SD host specification, preserving the other predicates and entire command path. The focused transport test passes 550 C scenarios, independently rerun. [Compatibility review](../out/research-native/native-sd-v6-compatibility-review.json), [source and test review](../out/research-native/native-sd-v6-gate-review.json). The final [V6 qualification](../out/native-a57/sd-read-v3/qualification.json) passes 1,006 checks across 13 bounded runs and preserves all 211 V4 plus 171 V5 frozen evidence files. The exact native kernel retains EL0 with SD disabled; enabled tests use a separately compiled fake-register variant. V6 is installed with all nine readbacks verified and 124 protected files unchanged, then safely ejected. [Installation receipt](../out/switch/native-xnu-v6-install.json). The owner subsequently reported the successful sector-zero comparison above.

V5's [qualification](../out/native-a57/sd-read-v2/qualification.json) passed 566 checks across six bounded runs and preserved 211 frozen V4 evidence files. Its [installation receipt](../out/switch/native-xnu-v5-install.json) records all nine readbacks verified and 116 protected files unchanged. The physical result above supersedes its earlier pending status.

The physical OLED has reached XNU VM initialization and BSD entry with v2, by [user report](../out/switch/native-xnu-v2-hardware-result.json). V3 adds an own signed RAM-disk process: the Mac Cortex-A57 model completes three real system calls and reaches EL0, while an otherwise identical payload with one changed signed padding byte is explicitly rejected by execution policy. [60-check paired review](../out/native-a57/userspace-v1/model-pair-review.json). Physical v3 userspace is now established by the [user-reported success banner](../out/switch/native-xnu-v3-hardware-result.json); native sector-zero reading is now separately established by user report; see the [current run guide](native-xnu-run.md).

The complete v3 suite of 169 unit tests passed across the main run and the isolated rerun of its one sandbox-blocked Unix-socket case. The main run passed 168 tests; the socket case failed at `bind` under the sandbox and passed outside it. [Main output](../out/research-native/native-userspace-tests.log), [socket rerun](../out/research-native/native-userspace-socket-test.log). The SD transport test contains 33 C register-model scenarios; the updater has 20 additional temporary-filesystem cases. These results establish host behavior, not native hardware operation.

V4 links the one-sector transport to native SDMMC1 MMIO after verified EL0. The new reference parser, ADT and package tests pass 31 targeted cases (including the test containing 33 transport scenarios); all 12 loader-builder cases pass after the final actual-bootargs guard. [Targeted output](../out/research-native/native-sd-v1-targeted-tests.log), [builder output](../out/research-native/native-sd-v1-builder-tests.log). Independent reviews pass 66 inert boot-script cases, 20 updater cases, 350 PMGR checks and final native-object binding. [V4 script](../out/research-native/native-xnu-v4-script-review.json), [updater](../out/research-native/native-xnu-v4-updater-review.json), [native object binding](../out/native-a57/sd-read-v1/native-objects-independent-review.json). The [runtime/package qualification](../out/native-a57/sd-read-v1/qualification.json) passes 434 checks across ten bounded runs. Enabled SD runs use fake registers; the exact native kernel reaches EL0 with SD disabled. V4 is installed, readback verified, and safely ejected, with 108 protected files unchanged. [Installation receipt](../out/switch/native-xnu-v4-install.json). Physical SD verification remains pending.

The remainder records earlier experiments in chronological context.

## Earlier native hardware milestones

The OLED CPU probe v3 returned successfully through the real Hekate → ATF → U-Boot path. The photographed result establishes native Cortex-A57 execution at EL2 and the firmware's 19.2 MHz counter frequency. [Full register decode and photograph hashes](../out/switch/native-probe-v3-hardware-result.json).

The next three photographed pages establish the actual U-Boot RAM banks, relocated firmware pointers, inherited framebuffer and memory-controller carveouts. The runtime banks total 4,093 MiB, with the final 3 MiB of the lower bank excluded. The static control device tree's 6 GiB memory template is not a free-memory map. [Decoded inventory and limits](native-memory-result.md).

The subsequent native handoff diagnostic is prepared for a physical test. It uses normal firmware kernel cleanup, then its own framebuffer renderer and an explicit EL2 → EL1 transition. Independent review caught an unaligned FDT load in the first compilation; the replacement uses `-mstrict-align`. The exact replacement ELF reproduces its packaged raw binary, passes four bounded Cortex-A57 CPU-model cases (successful EL1, two malformed-FDT refusals and an injected EL1 exception), and passes 41 boot-script control-flow cases. The model enables EL2 alignment checking. [CPU-model receipt](../out/research-native/handoff-cpu-model-review.json), [script review](../out/research-native/handoff-script-aligned-review.json), [run instructions](native-handoff-run.md).

The owner's next photograph confirms native execution **after U-Boot cleanup** and readable output drawn by the diagnostic itself. Entry is EL2 with the MMU/cache enable bits clear. The v1 validator refused with code `0xD` before attempting EL1: it expected a 32-byte memory property, while this pinned firmware serializes three configured bank slots, including an unused zero-filled third slot. The original local fixture omitted that third slot. [Photograph and decoded observation](../out/switch/native-handoff-v1-hardware-result.json).

These native milestones do not establish native XNU or iPadOS execution. The subsequent v2 photograph establishes physical EL1 execution as described below. The correction accepts the firmware's trailing zero slot while still requiring exactly the two measured nonzero RAM banks; it does not broaden the accepted memory map.

## Observed locally

- Host: macOS / ARM64; QEMU 11.1.2 installed for this task.
- Python unittest suite: the earlier complete run passed **125 tests** with `python3 -m unittest discover -s tests -v` in 4.025 seconds. The local QMP socket test required permission outside the filesystem sandbox. [Final test output](../out/build/tests-native-final-unsandboxed.log).
- HakiOS upstream revision: `88513176b1e96ecf0b764cd62c6f4e6ac71345e3`.
- The three prebuilt input artifacts matched the hardcoded SHA-256 values in `ioswitch.py`. They were downloaded from upstream, not compiled locally.
- A QEMU TCG Cortex-A76 / GICv3 guest with 4096 MiB virtual RAM ran the real XNU kernel and its raw-syscall open test program as PID 1.
- First successful smoke run: **6.10 seconds**. A second run during diagnosis also passed in **6.09 seconds**. This is Mac performance, not a Switch benchmark.

Evidence retained in this workspace:

- [First successful serial log](../out/runs/20261028-152256-o4t_cerq/serial.log), lines 115–116 and 189–190.
- [Result metadata](../out/runs/20261028-152256-o4t_cerq/result.json).
- [Exact QEMU arguments](../out/runs/20261028-152256-o4t_cerq/command.json).
- [Input provenance](../out/runs/20261028-152256-o4t_cerq/provenance.json).

The serial log contains the test program's greeting and its report of PID 1, uid 0. The loader replays output, so looking only at the end of the file can misleadingly show an earlier kernel-init phase. The test intentionally exits PID 1; the resulting panic is expected for this fixture. An initial QEMU launch failed because the read-only FAT drive lacked `readonly=on`; that was corrected before the successful runs.

## What the automated tests establish

The tests exercise malformed/incompatible device-tree rejection; iPad identity and TCG timer/memory validation; compressed-DMG rejection; bundle hashes and path containment; symlink/special-file refusal; no-overwrite behavior; preservation of actual Linux Hekate entries; QEMU device order and read-only disks; dry-run immutability; rejection of run output inside the bundle; memory preflight; and refusing to count a kernel banner as userspace success. They also cover source-copy revalidation, bounded QMP replies, lossless screenshot conversion, observation failure reporting, filtered Switch inventories and read-only disk-image export cleanup.

The iPad test fixtures are synthetic. They verify packaging and runtime contracts, not iPadOS execution. Independent reviews corrected missing USB keyboard support for UEFI, writable-run-directory overlap with a bundle, screen-size contract disagreements, symlink/special-file holes, and export cleanup/provenance issues.

## Locally compiled UniXNU

The pinned framework `1dba43325b8242c3c55001999cda841c25e9ca68` and QEMU board `97b75b1f63c6b7e895cd7646029db741d5fe5488` compiled with the installed Xcode/LLVM tools. The open native-user fixture booted under macOS HVF and reported:

```text
PC 00000001000003d4 (checkpoint 1000003d4), EL0,
getpid 1, sysctl status 0, hw.pagesize 4096
```

This is evidence from the newly built source kernel, separate from the earlier downloaded HakiOS smoke binaries. It still does not establish iPadOS UI or Switch compatibility.

- [Native-user build/run log](../out/build/native-user.log) and [run artifacts](../vendor/unixnu/out/qemu/run/20261028-155527/).
- [Exact command/environment recipe](../out/build/commands.txt) and [source/tool provenance](../out/build/provenance.json).
- [Storage ABI probe](../out/build/storage-abi.log): live original sizes, 4 KiB layout, and 190 ordinary storage slots passed. Its annotation object was not executed; this alone does not qualify block I/O.
- [Display ABI probe](../out/build/display-abi.log) and [display/HID archive build](../out/build/display-build.log).
- [Root verifier tests](../out/build/root-verifier.log): seven groups passed.

The complete Apple IPSW downloaded from the board's URL, and extracted OS/SystemOS members matched the upstream SHA-256 pins and ZIP CRCs. The policies verified all 1,483,785 OS blocks and 806,400 SystemOS blocks; the root kernel, trust bundle and Data payload were built together. The download's local SHA-256 is recorded in [download provenance](../out/build/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw.json); that whole-file hash is not an independently provided Apple authenticity pin.

## Actual iPadOS UI under Mac HVF

The source-built **iPad7,1 / iPadOS 17.7.11 (21H461)** CPU-rendered guest booted with upstream's setup-skip seed. It displayed the lock screen by the 60-second capture. A Home-button event unlocked to SpringBoard; tapping the visible Settings icon opened Preferences; Home returned to SpringBoard.

The result is visibly incomplete: several Home tiles are white, and Settings remained white after further waiting. A snapshot found 233 processes, including SpringBoard, backboardd and Preferences. The final kernel log repeatedly records iconservicesagent failures. The display reported stage 10, 32 diagnostic frame scans and 291 swap-validation calls at the snapshot. The content scan stops after the first 32 frames, so it is not a total frame count or frame-rate measurement.

- [Observed results and screenshot hashes](../out/build/hvf-ui-verification.json).
- [Lock screen](../vendor/unixnu/out/qemu/run/20261028-160544/screen-060s.png), [Home screen after input](../vendor/unixnu/out/qemu/run/20261028-160544/home-after-key.png), [white Settings](../vendor/unixnu/out/qemu/run/20261028-160544/settings-later.png), [return to Home](../vendor/unixnu/out/qemu/run/20261028-160544/home-after-settings.png).
- [Guest processes](../out/build/hvf-processes.txt), [kernel log](../out/build/hvf-kernel-final.log), [display counters](../out/build/hvf-display-counters.txt).
- [RSS summary](../vendor/unixnu/out/qemu/run/20261028-160544/host-memory-summary.json): 86 samples at two-second intervals after startup, maximum **3,682,368 KiB (3.51 GiB)**. Sampling missed the beginning of boot, so this is not the boot peak or a Switch memory measurement. It also excludes the host OS.

QEMU exited normally after the test was stopped through QMP at about 200 seconds. No guest network device or Apple account was used. This establishes a real iPadOS UI and input path on the Mac, not usable applications or Switch performance.

## Portable bundle

`bundles/ipados-17.7.11-tcg` contains 11 hash-recorded files, totaling 9,574,855,336 bytes (8.92 GiB). The OS raw export matched the exact fully verified root-policy bytes; SystemOS is the pinned raw member. It uses the TCG timer and the same root kernel/trust/Data pairing. [Build pairing](../out/build/ipados-provenance.json), [export log](../out/build/bundle-export.log).

The exported bundle was booted through `ioswitch.py run` on the Mac using Cortex-A710 TCG, one virtual CPU, 6 GiB virtual RAM, the CPU renderer and a 128 MiB translation cache. The original OS and SystemOS were read-only raw files; no Mac HVF or Metal bridge was involved.

| Observation | Evidence |
| --- | --- |
| Lock screen visible by 602 seconds; frame 582 still showed loader diagnostics | [602-second frame](../out/runs/20261028-161317-lh5nn82q/frame-000602.png) |
| Home event at approximately 675.8 seconds; Home visible by 682 seconds | [Home frame](../out/runs/20261028-161317-lh5nn82q/frame-000682.png) |
| Settings tap at approximately 720.4 seconds launched Preferences, PID 344 | [Process snapshot](../out/build/tcg-processes-ui.txt), [input events](../out/runs/20261028-161317-lh5nn82q/input-events.jsonl) |
| Settings remained black after about 74 seconds; no usable contents | [Settings after waiting](../out/runs/20261028-161317-lh5nn82q/settings-after-wait.png) |
| A second Home event returned to SpringBoard by frame 823 | [Return to Home](../out/runs/20261028-161317-lh5nn82q/frame-000823.png), [final Home](../out/runs/20261028-161317-lh5nn82q/home-final.png) |

The test ended with QMP `quit` after **894.1 seconds**, QEMU exit 0, 44 automatic screenshots and no observation errors. The final process snapshot listed 206 entries, including Preferences. UI conclusions come from the images and input response; the generic runner deliberately leaves its automatic `ipad_ui_verified` field false. [Explicit UI receipt and screenshot hashes](../out/build/tcg-ui-verification.json), [final runtime result](../out/build/ipados-tcg-result.json).

The guest disk driver reported **two authenticated virtual disks, 906,715 successful block-verification operations, zero integrity failures, zero I/O failures and no ambiguous policy selection**. These are actual guest reads against the build's locally signed owner policy; repeated reads are included and this is not a claim of Apple Secure Boot. The display recorded 3,462 accepted swaps and CPU copies, with zero rejected swaps. [Final counters](../out/build/tcg-counters-final.txt), [kernel log](../out/build/tcg-kernel-final.log), [processes](../out/build/tcg-processes-final.txt).

QEMU RSS was sampled throughout at two-second intervals. Its observed peak was **3,562,048 KiB (3.40 GiB)** at 77.32 seconds, before the first UI. This excludes the host OS, can miss brief peaks, and is not a Switch measurement. The 128 MiB translation cache flushed 206 times over the run; two earlier snapshots recorded 41 flushes in 143 seconds. That shows repeated translation work, but no comparison run establishes how much boot time a larger cache would save or its memory cost. [JIT observation](../out/build/tcg-jit-observation.json), [final JIT statistics](../out/build/tcg-info-jit-final.txt).

After shutdown, `ioswitch.py run --bundle bundles/ipados-17.7.11-tcg --dry-run` successfully rehashed every file and checked all sizes. The bundle manifest also matched the copy saved before boot. [Post-use integrity receipt](../out/build/tcg-post-run-integrity.json). This completes the Mac portability check while leaving app usability and console behavior unresolved.

## Real disk-image and observation checks

The OS export helper was also tested on a freshly created **32 MiB compressed GPT/HFS+ disk image on this Mac**. It copied the exact expanded bytes, checked size/hash/EOF, detached its mapping, and removed partial output on an intentional hash failure. [Integration result](../out/research-export/macos-integration.json). This checks the actual `hdiutil`/`diskutil` workflow; it is not a substitute for exporting the full paired iPadOS image.

The QMP observer captured an actual firmware screen in a headless QEMU check. [Captured image](../out/observation-checks/20261028-155420-ookkdjw0/firmware.png). That image proves screenshot plumbing only.

## Older CPU experiment

HakiOS's unchanged kernel failed under Cortex-A57 emulation on `msr PAN,#1`. An isolated diagnostic copy with immediate PAN instructions removed progressed further, then failed on `mrs x8,PAN`. The original input was unchanged. See [the bounded traces and architectural constraints](oled-cpu-memory.md#measured-first-a57-failure). These results rule out simply changing the CPU flag as a native/KVM solution.

## Not verified

- Usable app rendering and the Mac Metal bridge.
- An iPadOS boot on the Switch. The console inventory below establishes its baseline only; the completed Mac TCG run is not console execution evidence.
- Switch CPU performance, available memory under load, touchscreen forwarding, Joy-Cons, GPU acceleration, suspend/resume or persistence.
- Any native Hekate → XNU/Tegra boot.

The Mac build/test did not access a physical Switch disk. No partitioning, eMMC write or Hekate configuration change is part of these tests.

## First actual Switch execution

The owner provided SSH access to the running OLED. The [filtered baseline](../out/switch/baseline.json) confirms Ubuntu 24.04.1, kernel `4.9.140-l4t`, 4 GiB-class physical memory, approximately 2.65 GiB initially available, an existing 3 GiB LZ4 zram device, and no KVM. QEMU 8.2.2 and its GUI/firmware packages were then installed; [the setup receipt](../out/switch/setup-receipt.json) records the package transaction. No kernel, GPU driver, swap, clocks, Hekate or disk partition settings were changed.

The unchanged, hash-checked HakiOS smoke guest ran under TCG **on the Switch**, inside a transient service limited to 3 GiB resident cgroup memory and 180 seconds. It reached the real open userspace program, reported **PID 1 / uid 0**, and finished in **11.36 seconds**. Sampled peak QEMU RSS was **278,340 KiB (272 MiB)**; sampled swap use stayed zero. [Observed result](../out/switch/smoke-verification.json), [run evidence](../out/switch/smoke-runs/). This establishes XNU userspace on the console through Linux/QEMU, not iPadOS or native Tegra boot.

Transfer of the larger iPadOS bundle was interrupted by sleep/network loss, resumed after the owner restored connectivity, and then deliberately cancelled when the owner clarified that emulated deployment is unacceptable. The full iPadOS guest has **not** started on the Switch. The earlier planned trial's [host watchdog](../scripts/guard_run.py) passed its live cgroup-v1 and sensor acceptance test with a harmless three-second child command. That is wrapper evidence only; the deployment direction is now native XNU on Tegra.

## Storage and next on-device check

The initial 12 GiB free-space blocker was resolved by the owner; the volume had about **98 GiB available** before the full build. The 6,835,124,840-byte IPSW and approximately 8.74 GiB of raw OS/SystemOS data are now budgeted with build and export copies. No user files were deleted to make room.

The collector has now run on the actual Switch OLED: [baseline report](../out/switch/baseline.json). It identifies Ubuntu 24.04, kernel `4.9.140-l4t`, four Cortex-A57 cores, 3,991.8 MiB total RAM, 2,713.9 MiB available RAM, and about 24 GiB free in the inspected filesystem. Its existing 3 GiB lz4 zram swap had no used swap at collection time. Zram consumes physical RAM and is not additional memory capacity. QEMU was absent in this initial report, `/dev/kvm` was absent, and the kernel configuration reported virtualization disabled.

That baseline explicitly records `boot_test_performed: false`. It does not demonstrate a console XNU or iPadOS boot, input response, or memory fit. The [Switch checks tutorial](switch-checks.md) and [small ZIP](../out/ioswitch-checks.zip) remain available for repeat inventories; subsequent console tests have their own artifacts under `out/switch/`.

The memory-property correction was tested against a fixture produced with the pinned U-Boot memory helpers. The old binary reproduced refusal13; v2 passed to EL1 on that same fixture. Five further negative/exception checks passed, and all41 script cases passed for the exact v2 package. V2 was copied to eight new SD files, all readbacks matched, all49 prior tracked files remained unchanged, and the disk was safely ejected. [Model receipt](../out/research-native/handoff-cpu-model-v2-review.json), [installation](../out/switch/native-handoff-v2-install.json). Physical EL1 was subsequently established in the v2 photograph below.

## Physical EL1 handoff result

The owner's v2 photo confirms native AArch64 EL1 (`CurrentEL=4`), MMU/data/instruction caches off, DAIF masks set, direct framebuffer output and a positive physical-counter delta. Its48-byte memory property and two active banks also confirm the corrected shape check. [Full23-field transcription](../out/switch/native-handoff-v2-hardware-result.json).

The photographed FAILED label remains the historical automatic result. It expected HCR.HCD bit29, which Cortex-A57 defines as reserved zero because it implements EL3. The observedHCR0x80000000 is correct. Source now writes/requires RW only; an EL3-present model reproduces v2's false failure then passes the correction. The corrected package remains local and no repeat device probe is needed for the observed milestones. [Architectural and exact-model review](../out/research-native/handoff-hcr-review.json). XNU/iPadOS and interrupt delivery remain unverified.

## Native XNU loader implementation

The fixed-layout packer and freestanding loader are implemented. They validate Mach-O segments, zero-fill and gaps, boot arguments, Apple DeviceTree resources, source/destination CRCs and native FDT/MC exclusion checks. The native package contains a diagnostic XNU derivative and a minimal OLED Apple DeviceTree; it has no root disk or iPadOS userspace. [Run guide](native-xnu-run.md).

All **165 unit tests** passed. Independent artifact review rebuilt the adapter byte-identically and checked every kernel segment, zero-filled byte, layout gap and boot-argument field. The script passed 49 inert success/failure scenarios. [Artifact review](../out/research-native/native-xnu-artifact-review.json), [script review](../out/research-native/native-xnu-script-review.json).

The exact native adapter refused the model's unexpected stack before modifying its destination. A separate model adapter and native ADT with only GIC addresses adapted reached XNU VM initialization, CPU registration, GIC setup and BSD initialization. The earlier RAM-disk fixture rendered the known AMFI-related panic in XNU's framebuffer and reached `panic_spin_forever`. These bounded CPU-model runs do not establish native Tegra execution. [Model receipt](../out/research-native/native-xnu-model-review.json).
