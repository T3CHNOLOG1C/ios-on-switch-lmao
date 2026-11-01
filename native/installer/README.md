# Standalone BPMP APFS installer

**V28 is installed and the SD is safely ejected.** Select **More Configs → IOSwitch APFS Installer → RAW27.IWI → userdata (32768 MiB)**. Press **VOL UP once** on the erase screen to start. VOL DOWN cancels. After **Installation complete**, return to Hekate and select **IOSwitch Native APFS**. [Current procedure](../../docs/native-raw-apfs.md), [installation receipt](../../out/switch/native-installer-button-v28-install.json).

The prepared container retains its full **32 GiB capacity**, including System, Data and Preboot. Its SD image files now occupy **8.96 GiB**. The installer reads and writes **8.77 GiB**, skipping **23.23 GiB** proved free by the current APFS allocation map. Allocated zero blocks are included. The update reused existing files, trimmed/deleted unused dense parts, and copied only a new **16 KiB** tail. [Plan](../../out/native-sparse-v27-plan/plan.json), [allocation proof](../../out/research-native/raw-profile-v24/sparse-apfs-research.md).

## Transfer and target checks

The payload uses pinned official Hekate hardware, FatFs, display and SD DMA code. It checks source file lengths and the first APFS NXSB block's magic, checksum and exact container geometry. It does **no full-image hash, preliminary image scan, full target readback or zero scan**. Sparse extents select the actual source and target ranges; omitted target bytes remain untouched. Free Android bytes are not securely erased.

FatFs builds a bounded cluster-link map and batches contiguous clusters into requests up to 1 MiB. Unaligned reads use a small prefix and suffix around the batched middle. Aligned source buffers receive DMA directly; target writes stage each complete request in a private DMA buffer. The existing BPMP cache initialization remains active, with Hekate's supported clock boost. Every transfer checks card identity/errors and real CMD13 READY/TRAN completion. Unknown or enabled write-cache configurations refuse writes. There is no PIO path or retry/reinitialization after an uncertain transfer. Ordinary linked BDK/FatFs writes are wrapped to refusal.

Both GPT headers and entry arrays must agree and pass their CRCs. Used entries must have valid bounds, unique GUIDs and no overlap. Only Android `userdata`/`UDA` partitions outside the source FAT partition are selectable; an existing APFS GPT entry is refused. The screen shows the selected target's exact name, first LBA, sectors, capacity and bytes to copy. VOL DOWN cancels. A fresh VOL UP press after the buttons have been released permits writes to the selected image bounds. POWER is not required, and VOL DOWN takes priority if both volume buttons are pressed.

After all declared ranges finish programming, the installer changes only the selected GPT entry's type to Apple APFS and name to `IOSWITCH`. Its unique GUID, extent and attributes remain unchanged. Backup entries/header are written and read back before primary entries/header, with recalculated CRCs. GPT publication is not atomic across power loss. A failed transfer stops without publishing GPT; interrupted installation leaves Android userdata unusable. The host update did not write Android or GPT.

## External sparse manifest

The external `.IWI` is stored on FAT, never inside the APFS partition. Version 2 has a 64-byte header:

| Offset | Field |
| --- | --- |
| 0 | `IOSWIMG2` |
| 8 | LE32 version = 2 |
| 12 | LE32 ordered source part count, 1..128 |
| 16 | LE64 logical APFS bytes |
| 24 | LE32 APFS block size, 4096..65536, power of two |
| 28 | LE32 copy extent count, 1..8192 |
| 32 | LE64 concatenated source bytes |
| 40 | 24 zero reserved bytes |

Each following 256-byte source record contains a zero-padded, NUL-terminated 240-byte FAT-relative ASCII path, LE64 file size and eight zero reserved bytes. Source files have positive, sector-aligned sizes below 4 GiB and sum to the declared source bytes. The first contains the NXSB block.

After the source records, each 24-byte extent contains LE64 **image offset, source offset, byte count**. Source offsets address the concatenated declared parts. All ranges are APFS-block aligned, positive, sorted, disjoint and bounded independently in source and target. Source gaps are allowed. The first extent starts at image/source zero and covers the NXSB block. The whole table is validated before writing. The decoder cannot prove that omitted blocks are APFS-free; the host's allocation proof supplies that property.

`scripts/prepare_native_sparse_install.py` plans retained prefixes and bounded new slices from the qualified allocation map, and encodes this manifest. The current plan has six parts and six extents. The native decoder also accepts dense version-1 `IOSWIMG1` manifests; their legacy SHA field is ignored. Hidden metadata files are excluded from the image menu.

## Build and evidence

Official Hekate is pinned to `45cd81ed08e30a26ba380958c7e20b55c342834b`. The GPL-2.0 source/license remains under `out/research-native/sol-hekate-source`. The existing Arm GNU Toolchain is used; no toolchain was installed. `native/installer/ffconf.h` enables upstream fast FatFs access consistently across the reduced build.

```
python3 scripts/build_native_installer.py --output out/new-apfs-installer
```

Build output must be a new directory below `out`. The builder checks ARM ELF32, entry `0x40008000`, IRAM bounds and relocation alignment. Current output is `out/native-apfs-installer-v28`, **77,168 bytes**. [Artifact review](../../out/research-native/installer-fast-v27/final-artifact-review.json).

The real decoder/stream passed 35 ASan/UBSan cases, including malformed late extents refused before any write and omitted gaps retaining stale target bytes. The generated real manifest passed the actual decoder. Source batching, DMA staging, planner mapping and deployment success/failure paths passed focused checks. No VM, full suite, image-content hash or full image readback was run for this update. [Sparse native checks](../../out/research-native/installer-sparse-v1/README.md), [planner](../../out/research-native/raw-profile-v24/sparse-v27-plan-result.json), [deployer](../../out/research-native/raw-profile-v24/sparse-v27-deployer-result.json).

The actual confirmation function passed 12 focused ASan/UBSan cases for fresh presses, initially held buttons, cancellation, card removal and POWER alone. The updater passed five inert deployment cases. [Button checks](../../out/research-native/installer-confirm-v28/result.json), [updater checks](../../out/research-native/raw-profile-v24/button-v28-updater-result.json).

On-device installation, raw APFS mounts, original cryptex attachment, reboot persistence, throughput and the iPadOS UI still need physical verification.
