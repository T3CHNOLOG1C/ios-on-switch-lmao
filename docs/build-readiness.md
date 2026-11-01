# iPadOS build research and reproducibility

Research date: 28 October 2026. This document covers the Mac build and portable QEMU image. It does not establish Switch hardware compatibility.

## Selected source pair

| Input | Exact revision | Why |
| --- | --- | --- |
| [UniXNU](https://github.com/UniXNU/UniXNU/tree/1dba43325b8242c3c55001999cda841c25e9ca68) | `1dba43325b8242c3c55001999cda841c25e9ca68` | Framework main immediately before the QEMU author documented compatibility on 21 October; includes the fresh-kernel bootstrap fix |
| [UniXNU-qemu](https://github.com/xVanTuring/UniXNU-qemu/tree/97b75b1f63c6b7e895cd7646029db741d5fe5488) | `97b75b1f63c6b7e895cd7646029db741d5fe5488` | Inspected QEMU port with iPadOS root, CPU display, input, and Mac-only Metal bridge |
| [Panic font](https://github.com/UniXNU/uefi-sheng/blob/3aa95a72d469234755e2bcf2841ec6c0e83863e9/Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib/Font.h) | `3aa95a72d469234755e2bcf2841ec6c0e83863e9` | Only `Font.h` and the license are needed from this physical-device firmware tree; QEMU supplies its own firmware |

This pair was selected from upstream compatibility history and has now been built here through a visible iPadOS Home screen with working Home/touch input under both Mac HVF and the exported portable TCG runner. It is not an upstream published release. The QEMU author's [21 October compatibility change](https://github.com/xVanTuring/UniXNU-qemu/commit/56a98014ace4f42b3031f9ed82a757f6c871a312) removes the requirement for the bootstrap-fix branch after it merged. Both [UniXNU releases](https://github.com/UniXNU/UniXNU/releases) and [QEMU-port releases](https://github.com/xVanTuring/UniXNU-qemu/releases) were empty when checked. There is no released, prebuilt iPadOS bundle to substitute for a build.

Blindly pairing today's framework main with the QEMU port is unsafe. Framework `33da813` renamed the AES builder to `build-software-aes-bindings.py`, defaulting to `build/software-aes/NativeAES.o`. The QEMU repository's copied builder still writes `build/native-aes/NativeAES.o`. Compatibility aliases for archive files do not supply that object. The selected historical framework still uses the original paths. Pins must include framework submodules and `vendor/unixnu-qemu/unixnu.lock`, not just the two top-level commits.

## Build checkpoints observed locally

The selected pair has now compiled successfully with the stated Xcode/SDK. The second kernel pass, UniXNU EFI package, DarwinBoot EFI driver and QEMU framebuffer driver built. A 60-second HVF run reached the native user fixture at EL0, `getpid == 1`, successful `sysctl`, and `hw.pagesize == 4096`, then QEMU exited with status 0. This establishes the newly compiled XNU boot chain, not the original iPadOS userland.

The live TCG storage audit passed 190 original storage slots and 176 kernel bindings; the display probe observed the original framebuffer metaclass and built the Legacy display, HID input and key-store archives. The host verifier passed seven groups of real RSA signature and block-integrity checks. The full firmware/root stage subsequently passed: all 1,483,785 OS blocks and 806,400 SystemOS blocks were verified; the raw OS export matched its paired policy; the full root kernel and payload compiled. The first Mac HVF CPU-rendered guest reached the lock screen by the 60-second screenshot, accepted Home, opened Settings after a tap, and returned Home. Settings remained white and widgets had white placeholders, matching upstream's missing CPU-only graphics functionality. A process snapshot recorded 233 guest processes. This HVF result does not test Switch execution.

The exported TCG bundle then completed a separate Mac run through the IOSwitch launcher. Its lock screen appeared by 602 seconds, Home appeared after input by 682 seconds, and a Settings tap launched Preferences PID 344. Settings remained black after about 74 seconds; Home returned to SpringBoard. The run ended cleanly at 894.1 seconds with 44 automatic screenshots and no observation errors. It peaked at 3,562,048 KiB sampled QEMU RSS. The final guest counters showed two authenticated disks, 906,715 successful block checks, no integrity or I/O failures, and 3,462 accepted swaps/CPU frame copies. All 11 bundle files were rehashed after shutdown and remained unchanged. This is partial UI and storage proof on the Mac; it does not establish usable apps or console performance.

Exact invocation/environment notes: `out/build/commands.txt`. Source and tool provenance: `out/build/provenance.json`. Initial native-user evidence: `out/build/native-user.log` and `vendor/unixnu/out/qemu/run/20261028-155527/`.

## Tools and scratch space

The host is ARM64 macOS 27.0.1 with Xcode 27.0 (`27A5194q`), iPhoneOS 27.0 SDK, and QEMU 11.1.2. The existing Xcode/SDK is newer than the one referenced by upstream's current CI; compatibility must be demonstrated by building.

Required tools come from [UniXNU's requirements](https://unixnu.com/guide/requirements), `scripts/unixnu/util.py`, and the QEMU builders: Python 3.11+, git, make, cmake, ninja, unifdef, LLVM and lld, mtools, dtc, QEMU with its AArch64 EDK2 firmware, Xcode's iPhoneOS SDK, and macOS `hdiutil`/`diskutil`. The root-policy verifier additionally needs OpenSSL 3 headers and libcrypto. `mtools` and `lld` were missing and were installed. Homebrew also upgraded the linker's dependencies to LLVM 23.1.4 and z3 5.3.2.

`scripts/build_preflight.py` reads tool paths, SDK availability, the source revisions, submodule status, font, and the free space on the selected filesystem. It does not download, install, mount, write an image, or build. A successful result means the inventory checks passed; it does not certify the build or guest.

```sh
python3 scripts/build_preflight.py \
  --scratch . --unixnu vendor/unixnu \
  --device vendor/unixnu-qemu --uefi vendor/uefi-font
```

Apple's ZIP64 central directory was inspected with bounded HTTP range reads, without downloading the image during research:

| Item | Bytes | GiB |
| --- | ---: | ---: |
| Complete IPSW | 6,835,124,840 | 6.37 |
| Extracted OS UDIF member, `044-76040-038.dmg` | 5,157,406,069 | 4.80 |
| Extracted SystemOS raw APFS member, `044-76034-038.dmg` | 3,303,014,400 | 3.08 |
| Expanded whole OS disk, covered by its policy | 6,077,580,800 | 5.66 |
| Portable bundle's additional copies of OS + SystemOS | 9,380,595,200 | 8.74 |

These alone total about **28.65 GiB** if all originals and export copies are retained. Add source checkouts, compile objects, a 1 GiB Data seed while preparing it, payloads, run screenshots, logs, and temporary files. **50 GiB free is the planning reserve**, not a measured maximum. The download checks free space and stops before it falls below 10 GiB. The user freed storage during research; the original 10–12 GiB blocker was no longer present and the inventory reported about 98 GiB free before dependencies and downloads.

## Firmware identity and integrity

The port targets exactly **iPad7,1 / j120ap, iPadOS 17.7.11, build 21H461**, chip `0x8011`, board `12`. The [board file](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/board.toml) selects the Apple CDN restore image and pins:

- OS compressed member SHA-256: `3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d`.
- SystemOS raw member SHA-256: `b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f`.
- Decompressed kernelcache SHA-256: `f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7`.

The framework's `ipsw fetch` supports selective range downloads. The QEMU port's `build-os-root.py` calls an older `extract-target-volume.py` using `zipfile.ZipFile` and needs a **local complete IPSW**; a URL cannot be passed to that script as a drop-in substitute. Extraction checks BuildManifest identity and ZIP CRC. The OS and SystemOS policy builders subsequently check the exact pinned SHA-256 values and verify every authenticated block. The whole-IPSW digest is saved locally after download; it is provenance, not an independently supplied Apple authenticity pin.

## Ordered build and proof gates

1. Select the pinned repositories, their recursive submodules, and existing feed lock. Run the read-only inventory. Keep provenance and build logs under `out/build/`.
2. Build `qemu_defconfig`: first compile records, then the small driver archives, the source XNU kernel and original-kernelcache prelink, then the UniXNU EFI package. A first PMGR pass intentionally uses a no-op because it has no reference kernel yet; the subsequent kernel build links the bridge.
3. Build a payload with a persistent local platform seed and the matching accelerator's timer frequency. Boot the native-user fixture and check EL0, `getpid == 1`, and page size. This is an XNU checkpoint, not iPadOS proof.
4. Run `build-os-root.py`. It gathers live QEMU storage/display ABI evidence, extracts OS/SystemOS, creates a local owner key, authenticates their policies, builds verified block drivers, creates a 1 GiB Data seed, builds the framebuffer/input code, and packages the original trust caches.
5. Set `XNU_OWNER_DATA_VOLUME_UUID` from this exact Data seed; select `qemu_root_defconfig`; rebuild kernel and payload with the corresponding original trust cache and packed seed. Keep the key, signed policies, kernel and images paired.
6. First test the CPU-rendered guest on the Mac and retain serial output, kernel log, process list, screenshots and peak resident memory. A visible SpringBoard home screen and working tap/Home interaction are the next proof gate. Setup cannot currently finish normally; upstream's `--skip-setup` modifies the local seed to bypass that blocked flow.
7. Build a **TCG** payload before exporting for Linux. Mac HVF and TCG payloads have different timer frequencies; upstream rejects mismatches. The Mac-only Metal bridge and its shared-memory window are not portable Switch GPU acceleration.
8. Export whole-disk OS bytes and the raw SystemOS file, verify them against the exact root-policy reports, then pass them into `scripts/export_unixnu.py`. Boot the exported bundle on the Mac under TCG before trying the Switch.

`build-os-root.py` skips outputs that already exist. That is a convenience, not a dependency-aware cache: after changing source pins, owner keys, kernel configuration or ABI evidence, stale output must not be silently reused. A new build directory is preferred for a different pairing.

## Safe raw-image export contract

The OS member is UDIF and cannot simply be renamed to `.raw`. The OS policy covers the complete **6,077,580,800-byte virtual disk** exposed by `hdiutil`, including its GPT, not just the APFS partition. SystemOS is already raw APFS despite its `.dmg` suffix.

An export helper must accept an image path, never a manually selected `/dev/diskN`. Attach the exact source `-readonly -nomount -plist`; parse the returned GUID whole-disk entity; correlate that entity back to the same image in `hdiutil info -plist`; check `diskutil info -plist` for `BusProtocol == "Disk Image"`, `Writable == false`, exact device identity and expected byte size. Read only the derived raw character device into an exclusively created regular output file. Compare the streamed SHA-256 and size with `build/verified-root-os/verification.json`, and detach only the mapping created by the helper in `finally`. An existing unrelated disk or ambiguous attachment must fail closed.

The same image/source checks are already present in upstream's [OS policy builder](https://github.com/UniXNU/UniXNU/blob/1dba43325b8242c3c55001999cda841c25e9ca68/scripts/build-owner-os-policy.py). Its policy report also records the compressed-source digest and owner-key digest. `export_unixnu.py` alone does not prove that an arbitrary kernel is linked to those policies; retain their build provenance and verify the exported guest boots.

## Limits that remain after a successful Mac build

The QEMU port's own [state table](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/README.md#state) documents incomplete keyboard input, Setup blocked by an unavailable FairPlay service, Settings/icon rendering problems without Mac Metal, and Safari failure because AppOS is not mounted. The guest uses 6 GiB virtual RAM and a 4 GiB kernel-managed window; reducing `-m` alone does not repair the device tree and boot memory layout. The actual [Switch baseline](../out/switch/baseline.json) reports about 3,992 MiB total RAM, 2,714 MiB available at collection time, and existing 3 GiB lz4 zram swap. That compressed swap uses the same physical RAM. The Mac success and its 3.40 GiB sampled QEMU peak do not establish console memory fit.

The first Mac TCG run used the launcher's 128 MiB translation cache and reached the lock screen in about ten minutes. QEMU reported 206 full translation-cache flushes during that run. The [JIT snapshots](../out/build/tcg-jit-observation.json) and [bounded UI-thread snapshot](../out/build/tcg-ui-thread-summary.json) show that services continued running during the delay rather than establishing a deadlock.

A subsequent [256 MiB comparison](../out/tcg-cache-comparison/20261028-163914-a65ih3ac/comparison-result.json) used the same immutable bundle and normalized QEMU command; only `tb-size` changed. The first lock-screen capture moved from 602 to 502 seconds, with preceding loader captures at 582 and 482 seconds respectively. Near eight minutes, the original run had 82 cache flushes at 471.2 seconds, versus 14 at 477.15 seconds in the larger-cache run. Home input worked and the same blank widgets remained. The comparison exited cleanly through QMP at 790.98 seconds, with 39 automatic screenshots, no observation errors, two authenticated disks, 920,271 verified blocks and zero integrity/read failures. All 11 bundle files and the manifest remained unchanged after use.

This is one ordered comparison, not a repeated benchmark or proof that cache size alone caused the earlier screen. Host filesystem cache and concurrent work may differ. The 256 MiB run sampled a 2,005,584 KiB QEMU RSS peak at 107.86 seconds (389 samples); that is lower than the first run, but does not show that larger caches save memory or establish Switch memory fit. Manual Home timing and later UI actions differed, so total runtime and cumulative final counters are not comparable. Settings was not retested. `--tcg-cache-mib` exposes 16–1024 MiB for controlled experiments; the default remains 128 MiB until console measurements justify changing it.

## Full guest artifacts

- Portable TCG bundle: `bundles/ipados-17.7.11-tcg/` (11 hashed files; OS/SystemOS plus EFI/kernel/Data).
- Full HVF run: `vendor/unixnu/out/qemu/run/20261028-160544/`; see `home-after-key.png`, `settings-after-tap.png`, `settings-later.png` and `home-after-settings.png`.
- Process/kernel/counter evidence: `out/build/hvf-processes.txt`, `hvf-kernel-final.log` and `hvf-display-counters.txt`.
- Paired kernel/archive/policy/Data provenance: `out/build/ipados-provenance.json`.
- Completed portable run: `out/runs/20261028-161317-lh5nn82q/`; `home-final.png` is the last Home image before QMP quit.
- Portable UI observations and screenshot hashes: `out/build/tcg-ui-verification.json`; runtime summary: `out/build/ipados-tcg-result.json`.
- Portable final processes, kernel log and counters: `out/build/tcg-processes-final.txt`, `tcg-kernel-final.log`, `tcg-counters-final.txt`.
- Post-use verification of every bundled file and the unchanged manifest: `out/build/tcg-post-run-integrity.json`.
- Safe raw disk receipt: `out/build/os-export.json`; OS digest `e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2`.
- Bundle export: `out/build/bundle-export.log`; TCG packaging ESP `vendor/unixnu/out/qemu/run/20261028-160745/esp`. Its two-second staging run was not a boot test.

The exported-bundle Mac TCG portability gate is complete, with the rendering limitations above. The bundle manifest conservatively labels root-policy and OS-version validation as unverified because the generic exporter cannot derive those; independent policy receipts, runtime disk counters and build provenance provide this build's additional evidence. The embedded generation-time reports retain their original scope flags. The Mac build/test did not access a physical Switch disk; the separate console baseline is an inventory, not a boot result.
