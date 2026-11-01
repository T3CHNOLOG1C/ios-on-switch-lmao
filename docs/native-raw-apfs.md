# Standard raw APFS on the Android userdata partition

**Current result:** the owner started installation and subsequently booted the raw entry. APFS discovered the 32 GiB container and checkpoint XID 441, then root UUID publication/matching timed out. Root mounting, cryptex attachment and iPadOS startup remain unresolved. The new four-core kernel/package is local only; see [HANDOFF.md](../HANDOFF.md) and the [photographed hardware record](../out/switch/native-raw-v24-hardware-result.json). Updating boot files does not require rerunning the image installer.

**V28 fast sparse installer is installed and the SD is safely ejected.** V28 replaces only the 77,168-byte installer and starts with one fresh VOL UP press. Its payload passed readback and ejection. V27 previously installed the 1,744-byte sparse manifest and new 16 KiB tail; retained large files were checked by stat only. Four existing 2 GiB files were kept, the fifth was trimmed, and eleven obsolete dense parts were deleted. Retained image storage is **9,620,553,728 bytes (8.96 GiB)**. [Installation](../out/switch/native-installer-button-v28-install.json), [sparse plan](../out/native-sparse-v27-plan/plan.json).

The APFS container still has **32 GiB capacity**. Its current allocation map permits **9,418,506,240 bytes (8.77 GiB)** of DMA copying and **24,941,232,128 bytes (23.23 GiB)** of omitted free ranges. Allocated zero blocks and the complete checkpoint/internal-pool metadata areas remain included. Neither the source image nor the installed image is hashed or fully read back. GPT metadata still gets its small CRC/readback checks. [Allocation proof](../out/research-native/raw-profile-v24/sparse-apfs-research.md), [native artifact review](../out/research-native/installer-fast-v27/final-artifact-review.json).

The host did not overwrite Android userdata; image writes happen only through the on-device selected-target VOL UP action. The installed raw boot entry remains V24 and reuses V23's single-core kernel. The separate four-core package has not been copied to the SD. [Boot installation](../out/switch/native-raw-v24-boot-install.json), [new local package](../out/native-smp-four-boot-v1/manifest.json).

## Layout

The candidate contains one standard APFS container with System, Data and Preboot volumes sharing its 32 GiB. The System and App cryptex images remain ordinary files in Preboot. The complete original Data tree, including staged applications, was copied and verified: 10,995 entries and 467,599,350 regular-file bytes. This is a prepared filesystem, not proof of successful native startup. [Candidate](../out/native-single-apfs-trial-v1/candidate.json), [Data verification](../out/native-single-apfs-trial-v1/data-population.json).

| Item | Identity |
| --- | --- |
| Selected Android partition | `userdata`, 32,768 MiB, unique GUID `5703F8E9-0032-DF6B-2674-B1A569FFC4EE` |
| Partition start | Byte 926,908,678,144; sector 1,810,368,512 |
| APFS container | `50F8A67F-AFC1-4A1B-BCBB-82162674C67F` |
| Original OS volume | `04769D26-2CA0-4825-B443-E2E5F88300F2` |
| Data volume | `D1B1D053-D0A6-496C-8625-A947A3DA863E` |
| Preboot volume | `4AA4A761-5138-439D-9A30-D4138F6887FA` |

V24 originally staged sixteen 2 GiB parts. V27 reuses their verified data, trims/deletes unused ranges, and copies only a new 16 KiB tail. No large file was recopied, rehashed, or read back for this update. Games and personal files were not scanned, copied, or hashed. The separately saved GPT backup contains only 4,608 bytes of declared partition metadata. A 20 KB partition-layout description is also retained. [GPT backup](../out/switch/native-gpt-before-raw-v24.json), [layout description](../out/switch/native-partition-layout-before-raw-v24.json).

## Image installation procedure, only when an installation is needed

1. Close Hekate USB storage and choose **More Configs → IOSwitch APFS Installer**.
2. Select **RAW27.IWI**, then **userdata — 32768 MiB**. Volume buttons select; POWER opens.
3. Check the displayed target name, start sector and capacity. Press **VOL UP once** to erase that Android userdata partition and install APFS. VOL DOWN cancels.
4. Keep power connected until **Installation complete. DMA writes and GPT update complete.** The installer then powers off when POWER is pressed.
5. Return to Hekate and select **More Configs → IOSwitch Native APFS**.

The Android installation loses its userdata and should not be booted after erasure. The installer leaves the FAT partition, Nintendo content, Linux partition, emuMMC and other GPT entries unchanged. It updates the selected partition's type to standard Apple APFS and its name to `IOSWITCH` only after all declared image ranges finish programming successfully, preserving its unique GUID and extent. [Installer contract](../native/installer/README.md).

GPT publication writes backup then primary; power loss between copies is not atomic and can require external recovery using the saved table metadata. An uncertain transfer stops without a blind retry. The payload displays failures and does not claim a successful install from a partial write.

## Boot contract and evidence

The raw boot entry reuses V23's actual kernel and loader, including the 4 GiB RAM aperture, 4 KiB pages, required SDMMC DMA, corrected virtual reservations and early fault/panic output. A new 1,024-byte command sidecar appends exactly `ioswitch-storage=raw` in staged RAM. No old payload is rewritten or rehashed, and no new boot-time checksum scan is added. Its firmware directory contains the required BL31 and BL33 files. [Boot package](../out/native-raw-v24-boot/manifest.json), [independent package review](../out/research-native/raw-profile-v24/boot-helper-independent-review.json).

The native raw driver discovers the unique standard APFS GPT partition and binds the expected container UUID before publication. The physical store is writable. System mounts read-only; Data and Preboot mount read-write with ownership. Original DiskImages opens the two unchanged cryptex files read-only, and the bridge requires their real read-only media, exact source vnode and expected inner volumes. Dyld receives the mounted System cryptex cache path. There is no raw-to-FAT fallback on failure. [Actual compiled integration review](../out/research-native/raw-profile-v24/sol-raw-integration-review.md).

The separate **IOSwitch Native FAT V19** entry retains the earlier native text-console profile and its old storage behavior. Its OS/SystemOS files and eight persistent Data parts remain on the SD for fallback. It does not inherit V24's full-RAM/DMA configuration. [Fallback package](../out/native-fat-v19-fallback/manifest.json).

Nineteen obsolete IOSwitch payload directories and seventeen unused payload, policy, fixture and metadata files were deleted: 1,373,722,498 bytes total. Their contents were not backed up from the SD. V10 shared assets, the wrapped V19/V23 loaders and kernel windows, and FAT recovery images were preserved. [Directory cleanup](../out/switch/obsolete-ioswitch-v24-cleanup.json), [remaining-file cleanup](../out/switch/obsolete-ioswitch-v24-leaves-cleanup.json). The generated AppleDouble companion of RAW24.IWI was also removed so the installer lists only the real image.

The native sparse decoder/stream passed 35 focused ASan/UBSan checks, including stale Android bytes left untouched in omitted gaps. The generated real manifest passed the actual decoder; source batching, planner mapping and deployment failure checks also passed. Cross-compilation and static artifact review establish the prepared installer and boot contracts. The photographed native raw boot establishes container/checkpoint reads; complete installation correctness, root/volume mounts, original file-backed cryptex attachment, reboot persistence, performance and the iPadOS UI remain unverified.
