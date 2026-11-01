# IOSwitch

**The four-core native APFS kernel and separate boot package are built locally; they are not installed or hardware-qualified.** All four A57s are requested, with bounded entry/GIC/timer/IPI/scheduler checks. The latest raw boot still fails before root mounting. Read [HANDOFF.md](HANDOFF.md) for the exact artifacts, evidence, constraints and next steps.

The latest physical raw boot opened the 32 GiB APFS container and selected checkpoint XID 441, then timed out waiting for the OS UUID and panicked `Failed to mount root device`. No cause has been established. The new kernel adds a raw-only, bounded media/container diagnostic at that existing failure. [Photographed result](out/switch/native-raw-v24-hardware-result.json). The installed V28 installer and existing single-core entries remain; updating the kernel does not require rerunning the destructive image installer.

V23 fixes overlapping kernel virtual reservations introduced by the full-RAM layout. The actual old enumeration reproduces a fixed-range rejection before the stock console enables; V22's photograph reached stage 21, consistent with that failure. The corrected enumeration preserves the complete reserved union without overlaps or a RAM reduction. Full RAM, 4 KiB pages, required DMA and early fault/panic diagnostics remain. The owner later reported a root-mount failure; the exact preceding output was not captured. [Correction and evidence](docs/native-virtual-regions.md).

V22 corrects the early diagnostic gate for the actual full-RAM/stock-video profile and makes early C panics visible. V21 stopped on COPY COMPLETE by owner report; its disabled diagnostics hid whether XNU entered or faulted. The underlying stop is still unidentified. V22 retains full RAM and required DMA; multicore startup remains pending. [Entry correction and next-screen guide](docs/native-entry.md).

V21 removes the **768 MiB OS memory cap**. XNU now uses both measured RAM banks across the 4 GiB aperture, with live firmware reservations, the framebuffer, kernel and VM metadata kept out of the free queues. The remaining 768 MiB envelope protects only initial placement and temporary mappings. The native 4 KiB page policy remains active at every RAM size. [RAM implementation and checks](docs/native-ram.md), [new loader artifact review](out/research-native/full-ram-v21/v21-artifact-review.json).

V20's first hardware run stopped at the MC guard before any DMA transfer. V21 corrects the guard: TrustZone-only global registers are diagnostic, while readable MC memory identity and SDMMC1's own bypass state are required. Native data phases still require **64-bit DMA with no PIO fallback**. DMA correctness, speed, upper-bank allocation and graphical startup need the next hardware boot. [DMA evidence](docs/native-dma.md).

V19's latest photograph still shows the console, with completed APFS transactions through XID 550 and flushes around 17–55 ms. It has no visible panic or wrapped iterator warning, but does not establish a successful UI boot. The class-2 protection and indexing warnings also occur in the retained Mac run that reached Home. Actual native swap validation/copy/retirement state is still needed. [V19 hardware record](out/switch/native-xnu-v19-hardware-result.json), [display comparison](out/research-native/sol-v19-display-startup-review.md).

V21 retains the V19 fractional-counter correction and idle-cache fix, and the V18 log, vnode, Data and copy fixes. Display type zero means internal display. Inspected 4 KiB contracts pass, with bounded private-driver coverage. [Clock correction](docs/native-timebase.md), [audit](docs/native-audit.md), [4 KiB review](out/research-native/sol-v18-4k-audit.md).

The separate local raw candidate now has a standard **32 GiB APFS container with System, Data and Preboot roles sharing space**. Complete original Data contents, including staged apps, passed verification: 10,995 entries and 467,599,350 bytes. The unchanged sealed cryptex images are files in Preboot. The image is detached; no Android partition changed. V24 supplies the separate installer and explicit raw boot profile described above. [Candidate](out/native-single-apfs-trial-v1/candidate.json), [Data verification](out/native-single-apfs-trial-v1/data-population.json).

The V10 and earlier sections below are historical.

**V10 is installed, verified and safely ejected for the first native original-root boot attempt.** All 12 new files passed readback, all 163 tracked existing files were preserved, and all 9.38 GB of retained images passed full hash verification. [Installation receipt](out/switch/native-xnu-v10-install.json). Close USB storage and choose **More Configs → IOSwitch Native XNU**; photograph the whole final screen. It selects the original iPadOS root and Apple startup binaries, with disposable writable Data in RAM. The Mac CPU model establishes original dyld at EL0, 498 PID 1 syscalls and the expected root/cache/Data mounts and ownership. Original launchd main, Data writes, physical V10 execution and the iPadOS UI remain unverified. [V10 evidence and limits](docs/native-root.md), [independent model review](out/research-native/native-root-final-model-review.json), [run instructions](docs/native-xnu-run.md).

**V9 native APFS access succeeded by owner report.** The owner reported `apfs files verified - read only`; that path requires both read-only mounts and complete 574-byte and 49,152-byte files with matching hashes. No photograph or timing was supplied. [Physical V9 result](out/switch/native-xnu-v9-hardware-result.json). V9 used the own diagnostic RAM disk; Apple launchd, writable Data and the iPadOS UI were not established by that test.

Experimental **native iPadOS on a Switch OLED through Hekate**, using the existing FAT32 SD layout and `.bin` disk parts below 4 GB.

**Native XNU now reads FAT32 files and both split iPadOS disk images on the Switch OLED.** The owner reported `samples verified`, then `fat files successfully verified`. In the installed V8 code, that success requires the complete 131,777-byte fixture hash and all ten disk sample hashes, including the split boundary and offsets above 4 GiB. No photograph or exact timing was supplied. [Physical V8 result](out/switch/native-xnu-v8-hardware-result.json).

**V9 remains preserved as the working storage baseline.** Its installation verified ten boot files, preserved 154 tracked existing files, and fully verified the retained image files. [V9 installation receipt](out/switch/native-xnu-v9-install.json). It exposes both split disk images through persistent read-only XNU block devices. In the Mac CPU model, the original APFS module mounts both volumes read-only, and the signed test process reads two complete files with matching hashes. All five success/failure runs pass. [Qualification](out/native-a57/sd-block-v1/qualification.json), [block-device test and evidence limits](docs/native-block.md). Its **`APFS FILES VERIFIED - READ ONLY`** result is recorded by owner report above; the current menu entry now selects V10.

## Native handoff established

The memory check is complete: all three pages were photographed and independently decoded. They establish the runtime RAM banks, inherited framebuffer and active carveouts. [Physical memory result](docs/native-memory-result.md).

The [handoff v2 photograph and full decode](out/switch/native-handoff-v2-hardware-result.json) establish EL1 execution, masked exceptions, MMU/caches off, native display output and counter progress. Its final FAILED banner came from an incorrect expectation for a reserved HCR bit. The Cortex-A57 manual and an EL3-present CPU model confirm that the physical readback was correct. The source check is fixed; **no additional diagnostic run is needed to establish these milestones**. [Handoff report](docs/native-handoff-run.md).

The [earlier CPU-probe photograph and full decode](out/switch/native-probe-v3-hardware-result.json) establish native execution before U-Boot's OS cleanup. [Earlier probe guide and installation history](docs/native-probe-run.md). The new loader uses a bounded candidate allocation window, runtime exclusion checks and a native OLED device tree.

The intended native chain is:

```text
Hekate → existing OLED ATF / U-Boot → native XNU loader → Tegra XNU → iPadOS
```

The existing firmware can execute ARM64 code without Linux. The loader is implemented as an early kernel experiment; a complete Tegra/iPadOS port remains unfinished. The [boot contract](docs/native-boot.md) distinguishes Hekate's BPMP payload ABI from the AArch64 firmware route.

## Current evidence

| Component | What is established | What remains |
| --- | --- | --- |
| Native handoff and loader | Actual OLED entered EL1 after firmware cleanup; user reports v3 signed PID 1 at EL0 after real system calls | Apple userspace, native interrupt delivery and stronger memory-ownership proof |
| A57 kernel and GICv2 | V3 Mac model executes own signed PID 1 with real syscalls and rejects a corrupted signed page; earlier modeled self-IPI/timer interrupts observed | Native Tegra interrupt/security proof, sustained userspace, SMP |
| OLED inventory | Physical U-Boot pages establish both runtime RAM banks, framebuffer and active carveouts; live Linux tree supplies GICv2 register ranges | Native display driver independent of inherited scanout and a complete allocation map for XNU |
| Native SD transport | V8 owner report establishes sustained native reads through the bounded FAT32/split-file test | Physical qualification of persistent requests, recovery and broader controller support |
| FAT32 disk parts | V8 physical fixture and ten split-disk sample checks succeeded by owner report; all parts were fully hash-verified on the host | Full native disk authentication and root boot |
| XNU block devices and APFS | V9 file verification succeeded by owner report; V12 photograph shows original SystemOS and writable RAM Data mount completion | Sustained Apple userspace and observed Data writes; V13 skips custom media authentication |
| Stock video console | V12 photograph establishes full-screen XNU text output without the custom HUD | CPU compositor presentation and physical input |
| Software graphics | Native ARM64 iOS Mesa softpipe/Core Image fixture builds; static interface/import checks pass | Fixture execution, verified integration, usable apps and native display/input |

See [CPU/GICv2 evidence](docs/native-a57.md), [physical platform facts](docs/native-platform.md), and [graphics investigation](docs/linux-graphics.md). The earlier source-only CPU test stopped at an absent-AMFI-provider fault. V3 supplies the original provider and reaches the own signed test process in the model. Timer observations and userspace observations are recorded separately.

## FAT32 image packaging

`scripts/split_volume.py` packages a raw disk into ordered `.bin` parts with a default maximum of **4,000,000,000 bytes**, below FAT32's individual-file limit. Its manifest records sizes, offsets, per-part hashes and the whole-disk hash. Verification streams the original byte order without joining the files.

```sh
python3 scripts/split_volume.py pack --source /path/to/os.raw --output /path/to/new/os-parts
python3 scripts/split_volume.py verify /path/to/new/os-parts
```

Prepared local media:

| Volume | Parts |
| --- | --- |
| OS | 4,000,000,000 + 2,077,580,800 bytes |
| SystemOS | 3,303,014,400 bytes |

These remain under `out/native-media/` and are copied to `IOSWITCH/OS/` and `IOSWITCH/SYSTEMOS/` on the SD card, with complete per-part SHA-256 readback checks. Their hashes match the paired raw images. V9 adds the continuous read-only disk adapter, locally qualified with original APFS. Its native path and root authentication still need qualification. [Media verification receipt](out/build/native-media-verification.json).

## Upstream research and earlier laboratory baseline

HakiOS supplies examples of patched XNU, firmware loading and platform adapters. UniXNU supplies the selected iPad donor profile: **iPad7,1 / iPadOS 17.7.11 (21H461)**. Neither inspected project has a complete Switch OLED port.

The earlier prototype used QEMU's generic ARM machine, not a Tegra emulator. It reached iPadOS Home on the Mac with incomplete app rendering. A small XNU test also ran through Linux/QEMU on the actual Switch. The full iPadOS guest was never started there, and its unfinished transfer was removed when the native-only requirement was clarified.

The historical exporter, emulator launcher and Linux menu generator remain for laboratory use. They are not the native deployment:

- [Source pins and research](docs/research.md)
- [iPadOS build and provenance](docs/build-readiness.md)
- [Historical bundle preparation](docs/ipad-bundle.md)
- [Observed results and limits](docs/verification.md)
- [Read-only Switch inventory tutorial](docs/switch-checks.md)
- [Superseded Linux/QEMU trial procedure](docs/switch-run.md)

## Tests

```sh
python3 -m unittest discover -s tests -v
```

The earlier **v3 suite of 169 tests passed** across the main Mac run and its isolated rerun of one sandbox-blocked socket case. These cover packaging, hashes, device-tree contracts, native layout and reservation checks, guarded laboratory runs and failure handling. Separate probe/installer reviews also exercise mocked success and failure paths. They do not establish native hardware boot. Upstream components retain their own licenses; see [NOTICE](NOTICE).
