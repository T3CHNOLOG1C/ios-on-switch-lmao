# Existing-firmware card metadata exporter

This standalone `go` payload reads the already initialized SD card objects in
one exact retained U-Boot/BL33 build. It issues no firmware calls, card commands,
MMIO accesses, reset, recovery, power change or media writes. It is specific to
the IOSwitch native OLED profile and must refuse any layout or state mismatch.

The pinned artifact is `out/switch/boot-reference/bl33.bin`, 390,884 bytes,
SHA-256 `b492a4017580ca11c9cd49075a899a65f62069d3750615217fc10308954c9faa`.
The matching source snapshot is `out/research-native/switch-uboot` at the
recorded revision `722e2b86`. Offset evidence below comes from disassembling
the pinned binary, rather than assuming the source's C struct packing.

## ABI evidence

All binary offsets below are relative to the relocated image base. The exporter
reads 277 exact immutable instruction words from 17 function ranges, recorded
in `signatures.h` and `out/research-native/firmware-export-v1/abi-evidence.json`.
It does not execute those instructions. The build verifies every signature
against the pinned input artifact.

| Object / field | Byte offset / value | Pinned binary evidence |
|---|---:|---|
| Global data pointer | x18 | `_main` at `0x1c24..0x1c74` |
| gd relocation base / offset | `0x58` / `0x80` | loads at `0x1c44..0x1c4c` |
| gd active DM root / uclass list | `0x90` / `0xa0` | `uclass_find` at `0x1c4b8` |
| uclass driver / device list / sibling list | `8` / `0x10` / `0x20` | `uclass_find`, `uclass_get`, block lookup |
| BLK class driver | base + `0x523f0`, ID 13 | initialized table plus lookup at `0x1ef68` |
| MMC block driver | base + `0x51c10` | initialized driver table named `mmc_blk` |
| Tegra MMC controller driver | base + `0x52048`, ID 34 | initialized table named `tegra_mmc` |
| device platform / uclass platform / parent | `0x10` / `0x20` / `0x38` | accessor loads `0x1baaC`, `0x1bb2c`, `0x1bc58` |
| device private / class / uclass private | `0x40` / `0x48` / `0x50` | accessors at `0x1bb6c`, `0x1bbac`; device-list code |
| device list node / active flags | `0x60` / `0x90`, bit 0 | block lookup and `mmc_get_mmc_dev` at `0x20df0` |
| descriptor interface / device number | `0` = MMC 6 / `4` = SD device 1 | lookup at `0x1efb4..0x1efc8` |
| descriptor hardware partition | `0xb` = 0 | `mmc_select_hwpart` at `0x20eb0` |
| descriptor LBA count / block size / log2 | `0x10` / `0x18` / `0x20` | stores at `0x20720..0x2072c` |
| descriptor device backlink | `0x80` | block allocation; BLK private allocation is `0x88` bytes |
| Tegra platform configuration / mmc | `0` / `0x20` | bind at `0x216bc..0x216e8` |
| Tegra private controller address | `0` = `0x700b0000` | probe stores device address at `0x217a4` |
| MMC cfg / priv / device backlinks | `0` / `0x10` / `0xe0` | mmc bind stores `0x21170..0x21190` |
| MMC initialized / high capacity | `0x18` / `0x1c` | success at `0x20c44`; HC print at `0xa924` |
| MMC card version / OCR | `8` / `0x2c` | SD flag at `0x1fdb0`; ACMD41 store `0x20a18` |
| OCR high capacity interpretation | bit 30 | extraction `0x20a1c` then store `0x20a20` |
| MMC CSD / CID | `0x40` / `0x50`, four u32 each | R2 response stores `0x1fdf4..0x1fe00`; CID copy `0x1fd6c..0x1fd78` |
| MMC RCA | `0x60`, u16 | CMD3 R6 upper-half store at `0x1fdbc` |
| MMC bus width / clock / read block size | `0x20` / `0x24` / `0x6c` | info and startup code |
| MMC capacity / user capacity | `0x88` / `0x90`, u64 bytes | info `0xa958`; CSD capacity store `0x1fed8` |
| MMC pending init / init in progress / DDR | `0xd8` / `0xd9` / `0xdc` | init and info code |
| Firmware malloc start / end globals | base + `0x66cb0` / `0x66ca8` | `mem_malloc_init` at `0x1364c` |
| Firmware heap allocation | `[base-0x402000, base)` | `initr_malloc` at `0xfe68` |

All object reads are confined to the independently matched active firmware heap.
The initial global data and relocated image bounds are restricted to the native
profile's excluded `0xf0000000..0xffd00000` firmware region. List walks check
backlinks and have fixed 128-item limits. The selected block device must be
unique, active, device 1, MMC interface, user hardware partition and the exact
MMC block driver. Its parent must be the exact Tegra MMC driver with cached
controller base `0x700b0000`. Platform, MMC and descriptor backlinks must agree.

The cached card must have completed initialization, report SD version and OCR
ready+CCS, use CSD version 2, have a nonzero RCA/CID, 4-bit SDR operation and
512-byte blocks. Its CSD-derived sector count must equal both MMC capacity
fields and the block descriptor. The supported limit is 2^32 sectors. This is
cached same-boot firmware evidence; it does not actively re-query a card or
prove hot-swap identity. The later native controller-readiness gate and sector
zero comparison remain required before any filesystem read.

## Publication and output

`entry.S` runs at `0xa7000000`, preserves x18 and the AAPCS callee-saved registers,
switches to its own 16KiB stack, and returns to the caller's original SP/LR. The
complete current image is under 64KiB, enforced by linker and build checks.
It occupies part of the already excluded staged-kernel allocation; the package
must separately enforce that the kernel payload ends before `0xa7000000`.

The boot script must publish the exporter's executable bytes with the reviewed
firmware cache sequence before `go`, and run it before `bootm go` removes the
firmware execution environment. A file load alone is not I-cache publication.
The native handoff's subsequent cache clean provides physical publication of
the record. This payload uses DSB ordering but does not configure cache state.

The record is exactly 256 bytes at `0xa001f400`. Integers are little-endian.
It follows the root-owned `sd_reference.h` card metadata contract. Two complete
validated captures must agree before publication. Bytes 4..255 are written
first, then the magic at byte 0 after a barrier. Failure clears only the magic
word and returns a nonzero error code; the script must reject that return.

The SHA-256 inside the record identifies the pinned ABI artifact. It is a static
source identity, not a measured hash of the live relocated whole image. Runtime
layout qualification is supplied by the instruction signatures, pointer
relationships and cached-state checks above; packaging separately pins BL33.

## Verification boundaries

`test_export.py` exercises the C reader using synthetic cached objects, every
instruction-signature mutation, every read failure, invalid pointers, list
corruption, duplicate devices, card-state mismatches, capacity bounds and
snapshot changes. It checks that failed captures leave their output unchanged.

`model.py` executes the exact AArch64 image under generic Cortex-A57 QEMU with
synthetic firmware RAM. Positive and four refusal scenarios preserve x18,
x19..x30 and SP, leave all firmware RAM byte-identical and confine record writes
to the expected bytes. This does not execute real U-Boot or establish Tegra
hardware, physical card, firmware cache or live-pointer behavior.

The current reviewed candidate is `out/native-firmware-export-v1c`; its model
report is `out/native-firmware-export-v1c-model-run/report.json`. The actual
modeled record also passes the root-owned C card-reference validator; see
`out/research-native/firmware-export-v1/validator-compatibility.json`.
