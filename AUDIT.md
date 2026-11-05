# IOSwitch audit — 29 October 2026

This audit covers the work left by the previous agent. Paths are relative to `out/native-a57/root-v1/tree/` unless they start with `scripts/`, `boot/` or `out/`. In the source column:

- **V**: verified by reading the code in this session.
- **A**: reported by a sub-audit and not independently re-verified.

## What changed today

1. **New SD block driver.** `drivers/NativeIOSwitchBlock/NativeIOSwitchBlock.cpp` is rewritten as one self-contained driver.
   - It owns SDMMC1 with no probe theater. It reads the GPT, finds the single APFS partition and publishes it as a whole-disk `IOBlockStorageDevice`.
   - IOKit and APFS take it from there: the whole-disk container scheme, then the container, then the volumes, then the root matched by UUID.
   - I/O runs on a worker thread, so callers no longer block or recurse.
   - Each command moves up to 256 KiB with SDMA (CMD18/CMD25 plus Auto-CMD12). Timeouts are wall-clock.
   - Every failure is logged as `IOSwitchSD: … step= status= present=`, followed by a SDHCI reset, a CMD12 abort and one retry. The device is never latched offline.
   - There is no CMD13 after writes, no write-cache "qualification", no HUD redraws and no hashing.
   - Progress lines print every 5 s for the first 3 minutes after boot. The root-wait timeout prints the IOMedia list.
2. **Root boot hook.** `ioswitch_root_boot_prepare` no longer runs the SD "probe" (the sector-0 vs U-Boot comparison, card-reference records and so on). It publishes the block device directly. The storage profile is parsed lazily (`IOSwitchFiles.c`).
3. **Raw mount size check.** `IOSwitchRawMount.inc` no longer requires the container to be exactly 32 GiB.
4. **Build and package scripts.**
   - `scripts/build_kernel.py`: about 7 s; no receipts or hash gates.
   - `scripts/package_raw_boot.py`: produces a self-contained boot folder plus a Hekate ini, with the command line baked in instead of patched in at a fixed address.
5. **Deployed.** New Hekate entry **More Configs → "IOSwitch ioswitch-rawsd-v29"**. The change is additive; V24 and V19 are untouched. `armcpu-diagnostics=1` was removed from the command line; it only added panics.
6. **macOS had auto-mounted the Switch's APFS partition**, with the Data volume read-write; Spotlight was writing to it. I unmounted it and ejected the card.

## Root-mount failure: what is now known

- **The sparse install is not the cause** (sub-audit A, using an independent APFS walker).
  - 61,205 metadata blocks were checked; 0 are missing from the sparse plan.
  - 0 file-data or allocated blocks are missing, and 0 bytes differ between the SD parts and the .cdr.
  - macOS also mounted all three volumes straight from the card.
- **Most likely cause: the old SD transport** (V, and A independently).
  - A single timeout or error latched the whole disk offline for the rest of the boot (`online=false`, `host->blocked=1`), with no log line.
  - Its timeouts counted register polls (1,000,000) rather than time, and CMD13 busy-polling followed every write.
  - The FAT profile mounted the OS read-only. The raw profile publishes a *writable* container, so APFS writes during mount.
  - V23 on FAT also failed root mount, after DMA and full-RAM support landed. That points at the transport, not the partition.
- The new driver logs any SD error, so the next boot either mounts root or shows exactly which command failed.

## Findings, ranked

### Blockers and bugs
| # | Area | Finding | Src |
|---|---|---|---|
| 1 | Storage | Old driver: permanent offline latch, poll-count timeouts, CMD13 after every write, synchronous and re-entrant completion, three copies per read. **Replaced.** | V |
| 2 | Display | V19 reached userland but never drew a frame. The display driver (`drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`) never sends vblank or completion events. In `kickOnce`/`finishTransfer` (~:149, :256) a failed swap leaves `transferred=true`, which permanently skips later frames. Rejections are only stored in RAM, never logged. | A |
| 3 | SpringBoard | Known dependency chain (UniXNU-qemu, same 21H461 build): SpringBoard → chronod → liveactivitiesd. liveactivitiesd needs an AppleKeyStore stand-in that answers selectors 0/7/17/35/101. The render server also needs vsync. Also required: `/chosen/display-scale`, an iPad `/product` identity, the disabled.plist daemon set and the setup-skip preferences. | A |
| 4 | Input | Nothing for Switch hardware: no I2C, GPIO or PMIC driver, no FTM4 touch, no Joy-Con. `ArmCpu` masks every SPI and nothing calls `armcpu_spi_attach`. | A |
| 5 | Kernel | Physical layout is hard-coded in `arm_vm_init.c` (holes 0xa0000000/0xf5a00000, kernel start 0xb207c000) and `pmap.c:2685`. If the loader moves, memory corrupts silently. | A |
| 6 | Kernel | `kern_exec.c:7429-7763` rewrites launchd's disabled.plist and disabled.migrated on every boot, with fsync and readback, and panics on any mismatch. It also panics if `/private/var` ownership isn't 0:0 or 501:501. This should be done once, in the image. | A |
| 7 | Kernel | `kern_priv.c:91` grants PRIV_WORK_INTERVAL to every process. `work_interval.c:1118` (`|| 1`) silently drops Core Animation's second interval, which breaks render pacing. | A |
| 8 | Kernel | `DINetBootHook.cpp:117-124` (raw branch): waits 30 s for IOResources to go quiet, then can exit before DiskImages is ready, so the cryptex attach fails. | A |
| 9 | Platform | No PSCI reboot or power-off from iPadOS (`ArmDTPlatform.cpp:112-121` uses PSCI only in diagnostic stages). | A |
| 10 | Installer | `boot/installer/main.c:82,89` refuses to run if an APFS GPT entry exists, but the installer itself creates that entry. Reinstalling needs a hand edit of the GPT. | A |

### Performance
| # | Finding | Src |
|---|---|---|
| 11 | The SD runs at U-Boot's 48 MHz HS, 3.3 V (about 20 MB/s at best). SDR104 (about 90 MB/s) needs PLLC4, LDO2 at 1.8 V, pad calibration and tuning, as in hekate `bdk/storage/sdmmc_driver.c`. The new driver logs the actual clock registers at boot. | A+V |
| 12 | Console: the framebuffer is mapped uncached, and the rotated console scrolls pixel by pixel with preemption off. With `-v debug=0x14e serial=0`, every printf and kprintf goes there. Likely the largest boot-time cost; drop `-v`/DB_KPRT once diagnosis is done. | A |
| 13 | Frame copy is pixel-by-pixel into an uncached mapping. The IOSurface is remapped and boot-args re-parsed on every frame. | A |
| 14 | Loader: U-Boot loads a 67 MiB window (about 14.6 MiB of it padding) to 0xA2000000. The loader then copies it again with the I-cache off, after walking 2×67 MiB of cache-invalidate loops. `boot/handoff/probe.c` clears the framebuffer and runs a bit-serial CRC32 of the DTB on every boot. | A |
| 15 | `pa_valid` now calls out of line on every pmap operation. Boot runs three scans of 1M pages, each nested over the reservation list. | A |
| 16 | Single core: `cpus=1`, so three A57s sit idle. The SMP code panics the boot if any core misses its budget, instead of degrading. | A |
| 17 | Installer: bounce-copies 9.4 GB that is already aligned, sends CMD13 after every read, and writes in 1 MiB chunks. The Mac has raw access to `disk4s9` and could write the extents directly. | A |

### Over-engineering, unnecessary hashing, waste
| # | Finding | Src |
|---|---|---|
| 18 | The whole SD path was built as a "diagnostic probe": register allow-lists on every access, a U-Boot sector-0 reference CRC compare, SHA-256 known-answer self-tests and a card "cache-off qualification" via CMD48. A firmware "exporter" scrapes U-Boot's SD state from RAM (`boot.cmd`). | V |
| 19 | `IOSwitchCryptex.cpp` "authorizes" a disk-image attach by reading private DiskImages object offsets (0xa8/0x98), class sizes, the kext version and an eight-class registry chain. The real work is `di_root_image_ext()` plus a mount. | V |
| 20 | "Owner boot manifest": a SHA-384 over fixed constants published as `boot-manifest-hash` (`IOKitBSDInit.cpp:569-657`), plus `native_verified_block_*` "authorization" that walks every IOMedia three or four times per mount. | V |
| 21 | `boot.cmd`: each file gets a size query, a size compare, a load, another size compare and a CRC32, then `bootm` verify, then another CRC. A U-Boot instruction fingerprint refuses any rebuilt U-Boot. Files come from three different version folders (v10, v23, raw-v24). | V |
| 22 | The loader requires exact values in 25 memory-controller registers and exact RAM banks, so any Hekate or L4T update refuses to boot. | A |
| 23 | HUD repaints: full-region pixel fills with interrupts off, triggered on every megabyte of I/O progress and every root event (`IOSwitchRootDiag.inc`). | V |
| 24 | `bsd_init.c` kext-load "probes" store results in write-only globals; a failed APFS load only shows up as the 60 s root wait. There are dozens of empty "checkpoint" functions. | V |
| 25 | Dead security bypasses: `IOUserClient.cpp:4334-4545` (name-matched MAC skip, three getProperty calls per open), `bind_vfsops.c`, and the `OSKext.cpp:8461` plist-address call. | A |
| 26 | Host scripts: receipts that check receipts, hard-coded SHAs, "readback verification" that reads the page cache, a dense 32 GiB staging image hashed three ways, and TAIL.BIN of zeros. | A |
| 27 | Disk: `out/` was 94 GB on a disk with 4 GiB free; the obsolete outputs are now deleted (see Cleanup). | V |

### Correct, keep
- The 19.2 MHz timebase and the fractional microtime fix (`rtclock.c:475`).
- `ptoa()` in `pmap_sync_page_attributes_phys`.
- The 4K user ABI changes, which the A57 needs.
- GICv2 setup.
- Skipping the idle cache flush on A57; make it unconditional rather than gated on diagnostics.
- `armcpu-self-ipi`, which enables cross-CPU signalling.

## Cleanup (done 29 October 2026: about 54 GB freed, `out/` now 31 GB; build and packaging re-verified)
| Path | Size | Why |
|---|---|---|
| out/native-data-v17-16g | 16 GB | FAT-file Data, superseded by the container |
| out/native-media | 8.7 GB | Split FAT OS/SystemOS parts |
| out/build/os.raw | 5.7 GB | Regenerable from the IPSW |
| out/native-a57/root-v1/data-seed (`.cdr`, attempt-*) | about 6 GB | RAM-Data experiments |
| out/native-a57/{sd-read-v1..v3, sd-files-v1/v2, userspace-v1, tree} | about 21 GB (du; clones) | Old trees and runs |
| out/native-a57/root-v1/passes (all but smp, persistent-data, vmregions, memory, dma, rawsd), root-runs | about 3.8 GB | Model/QEMU runs |
| about 35 `*-fat32`, `*-delta`, `native-xnu-v*` dirs | about 2 GB | FAT images |

Keep: `out/native-a57/root-v1/tree`, `out/native-single-apfs-trial-v1/ipad-candidate.cdr`, the IPSW, `out/research-native/sol-hekate-source`.

## Next steps
1. Boot **IOSwitch ioswitch-rawsd-v29** and photograph the screen, especially the `IOSwitchSD:` lines. They give the SD clock registers, the partition, I/O counters and any failing command.
2. Once root mounts: simplify the cryptex and Preboot path, then work through the SpringBoard chain (AKS stand-in, vsync, display scale, daemon set, setup skip) and fix the display driver's swap stall.
3. Performance: quiet the console, enable SMP with a fallback to fewer cores, and add SDR104.
4. Cleanup: drop the FAT code paths, the probe and receipt scaffolding, and the fixed-address loader gates.
