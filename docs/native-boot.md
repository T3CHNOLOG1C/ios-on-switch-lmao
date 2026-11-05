# Native OLED boot contract and first probe

Research date: 28 October 2026. The deployment target is native AArch64 execution through the existing Hekate setup, with files on the existing FAT32 partition. Linux and QEMU are not part of that target runtime. The native iPadOS port is **not bootable yet**. The artifact built here is a small returning CPU probe, not XNU or iPadOS.

## What the installed firmware establishes

The existing FAT32 boot directory was initially inspected through a temporary read-only mount. Its original files were copied into `out/switch/boot-reference/` and the mount was removed. A separate reviewed probe entry was subsequently installed; see the [run guide and installation evidence](native-probe-run.md). The original Noble entry remains unchanged and uses `l4t=1`, `boot_prefixes=/switchroot/ubuntu-noble/`, `id=SWR-NOB`, and `r2p_action=self`.

| Existing artifact | Size | SHA-256 |
| --- | ---: | --- |
| `bl31.bin` | 45,655 | `dea0ecd406cd110b8d475641900ed23223d24d32dd5cef4d8a9ea2459c34e2b4` |
| `bl33.bin` | 390,884 | `b492a4017580ca11c9cd49075a899a65f62069d3750615217fc10308954c9faa` |
| `boot.scr` | 16,393 | `715bca03ca33a711d0666f07080da5b83aec1cd96395a6856a2530d3e3dd4032` |
| `nx-plat.dtimg` | 668,726 | `adfafc99ef03d690ac4ba3f852f53a8ba9274399df78be45ef59ee003c4ac40a` |

The installed BL33 identifies itself as `U-Boot 2024.NX02.b201801 (Jun 01 2024 - 18:37:55 +0000)`. BL31 and BL33 match the Noble 5.1.2 hashes independently recorded by [Scarlet's bootstack manifest](https://github.com/petitstrawberry/scarlet-project-switch/blob/main/projects/aarch64-switch-l4t-console/bootstack.json), which points to [Switchroot U-Boot revision `722e2b86`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/tree/722e2b86be9ab1ae073335585c5997093e05f4f2). That source revision was fetched into `out/research-native/switch-uboot/` for inspection. We have not rebuilt the firmware and reproduced its binary hash; source matching is therefore an informed reference, not a reproducible-build claim. The installed device-tree image differs from Scarlet's; the existing OLED image remains the board reference.

The copied `boot.scr` has valid legacy-image header and payload CRCs. Its exact decoded text is `out/research-native/actual-boot.cmd`, with a receipt in `actual-boot-script-receipt.json`. It maps SKU 3 to `FRIG`, loads the matching `nx-plat.dtimg` entry, checks the selected tree's identity/revision, then uses `bootm`. Relevant addresses are:

| Purpose | Address |
| --- | --- |
| Kernel image staging | `0xA0000000` |
| Initramfs staging | `0x92000000` |
| Combined device-tree staging | `0xA8000000` |
| Selected device tree | `0x8D000000` |
| Script staging, U-Boot default | `0x8FE00000` |

These are existing firmware conventions, not a complete free-RAM map. Native code must preserve firmware, framebuffer and hardware carveouts and the runtime U-Boot relocation region. A generic QEMU memory map or Linux's available-memory count cannot substitute for that map.

## Two different Hekate entry contracts

The [pinned Hekate `payload=` implementation](https://github.com/CTCaer/hekate/blob/45cd81ed08e30a26ba380958c7e20b55c342834b/bootloader/main.c#L154) rejects files larger than `0x30000` bytes (192 KiB), adds its relocator, and enters at `0x40010000` on the BPMP. Its [build target](https://github.com/CTCaer/hekate/blob/45cd81ed08e30a26ba380958c7e20b55c342834b/Makefile#L78) is ARMv4T/ARM7TDMI. Renaming an ARM64 kernel or this probe to `payload.bin` does not satisfy that entry contract.

The existing **L4T firmware route is native**:

```text
Hekate on BPMP
  -> existing Mariko memory/power firmware and ARM Trusted Firmware (BL31)
  -> existing AArch64 U-Boot (BL33)
  -> proposed native probe / future native XNU loader
```

The [Switchroot bootstack documentation](https://gitlab.com/switchroot/bootstack/documentation/-/blob/main/README.md) identifies these firmware components. Linux is a later payload of U-Boot, not a prerequisite for U-Boot to execute another native payload.

At [Hekate revision `45cd81ed`](https://github.com/CTCaer/hekate/blob/45cd81ed08e30a26ba380958c7e20b55c342834b/bootloader/l4t/l4t.c), the L4T loader places BL33 at `0xAA000000`, imports configuration through the environment block at `0xA9FC0000` with magic at `0xA9FBFFFC`, and requests a nonsecure `EL2t` entry (`SPSR=8`) through BL31. The Mariko branch boots CPU0 with `ccplex_boot_cpu0(..., true)` and supplies the B01 BPMP firmware and memory-training tables. Secure memory placement depends on the build's carveout configuration; it must not be flattened into ordinary XNU RAM.

Do not replace BL33 with an arbitrary flat stub yet: Hekate also interprets a word at BL33 offset `0x10` as its embedded device-tree offset and may patch UART properties there. Keeping the already working BL31/BL33 pair avoids inventing a second firmware ABI during the first CPU check.

## What U-Boot can launch

The [matching Switch defconfig](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/configs/nintendo-switch_defconfig) enables `BOOTM` and explicitly disables `BOOTI` and `EFI_LOADER`. `go`, memory display, and script execution default on in that tree; the installed binary contains the `go` and memory-display command descriptions. The board source includes T210B01 handling and assigns OLED/FRIC SKU 3. Therefore the current UniXNU `BOOTAA64.EFI` cannot simply be handed to this installed U-Boot build.

There are two useful successive contracts:

1. **Returning `go` application:** [the implementation](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/cmd/boot.c) calls an AArch64 function with `x0=argc`, `x1=argv`, preserving the current U-Boot execution environment. It prints the returned status. This is suitable for reading CPU facts without taking hardware ownership. It does not pass a kernel FDT in `x0` or prove the post-firmware kernel handoff.
2. **Native kernel handoff via `bootm`:** [ARM64 boot code](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/arch/arm/lib/bootm.c) performs cleanup and passes the selected FDT in `x0`, zeros `x1`–`x3`, and uses its EL2 handoff helper for an ARM64 image. A later tiny `uImage` probe must confirm the actual resulting EL, cache/MMU state, reservations, timer and framebuffer before XNU takes over. Using this boot-image ABI does not require a Linux kernel.

A native XNU loader must either implement the needed DarwinBoot/UniXNU loading and boot-argument contract directly or use a separately validated OLED UEFI implementation. Enabling U-Boot EFI, or reusing an older Erista/coreboot UEFI port, is not by itself proof that UniXNU's protocols, memory map and filesystem access work on this console.

## Locally built first probe

Source: `boot/probe/registers.S` and `boot/probe/link.ld`.

The probe is a position-independent AArch64 ARMv8-A leaf function. It preserves callee-saved registers, `x18`, the incoming stack, and the return address. It reads ID/EL/timer/MMU registers, writes a 256-byte record at its own load address plus `0x200`, then returns status 0. Unsupported ELs return 2 without the completion marker. It contains no system-register writes, MMIO access, storage access, SMC/HVC calls, cache changes, interrupt-controller changes, or exception-level transitions. The only stores target its own in-image result buffer.

Build commands, run from the workspace root with the installed LLVM tools:

```sh
clang --target=aarch64-none-elf -march=armv8-a -c boot/probe/registers.S -o out/research-native/registers.o
ld.lld -T boot/probe/link.ld -o out/research-native/registers.elf out/research-native/registers.o
/opt/homebrew/opt/llvm/bin/llvm-objcopy -O binary out/research-native/registers.elf out/research-native/registers.bin
/opt/homebrew/opt/llvm/bin/llvm-readelf -h -r -s out/research-native/registers.elf
/opt/homebrew/opt/llvm/bin/llvm-objdump -d out/research-native/registers.elf
```

Observed build: 284 bytes of instructions; complete raw file **768 bytes**; AArch64 little-endian ELF; entry offset 0; result offset `0x200`; no ELF relocations. Raw binary SHA-256: `0cf22b9ebc368a7084e3b9346accf2692df7659e773eedaa2c6a38b08088ff22`. Disassembly is saved in `out/research-native/registers-disassembly.txt`. **V3 executed successfully on the OLED**, returned zero, and produced the full [photographed register record](../out/switch/native-probe-v3-hardware-result.json): EL2, Cortex-A57 r1p1, enabled MMU/caches, firmware-reported 19.2 MHz frequency.

The original package in `out/native-probe-reviewed-fat32/` used `switchroot/ioswitch-probe/` and pinned copies of the existing BL31/BL33. The successful v3 revision uses `switchroot/ioswitch-probe-v3/`. `scripts/package_native_probe.py` reproducibly wraps the 768-byte probe in an 832-byte, uncompressed U-Boot standalone image. The current script checks OLED identity and exact firmware helper instructions, requires a FAT directory size of 832 bytes, then reads exactly 832 and verifies the checksum. Its earlier 833-byte read-count test was incorrect for this firmware. It stages the image at `0xA0100000`; `bootm start` validates its header and `bootm loados` copies its payload to `0xA0000000` and publishes the destination data cache. These explicit states do not perform OS preparation or handoff.

The script then calls the opcode-guarded original BL33 tails at `0xAA001920` (`IC IALLUIS; ISB; RET`) and `0xAA001A64` (`DSB SY; ISB; RET`). Together with the load stage, they provide the required data-cache clean, DSB, instruction-cache invalidate, DSB and ISB sequence before `go 0xA0000000`. The helper calls deliberately return status 1 because they preserve `go`'s incoming argc; that status does not skip the following operation. Only a successful probe return and matching record marker permit `md.l 0xA0000200 40` (64 32-bit words, 256 bytes).

Every authored success or refusal branch reaches a sleep loop with no timed reboot or OS fallback. Recovery is physical POWER held for at least 20 seconds. This is a script hold, not a guarantee against a firmware fault or manual console interruption. Static review and mocked command tests establish authored control flow; the owner's subsequent v3 photograph separately establishes the successful native run and visible result. This probe measures U-Boot's current EL/MMU state, not the separate `bootm` OS cleanup/handoff state required for a kernel.

The result consists of little-endian 64-bit fields:

| Offset | Field |
| --- | --- |
| `0x00` | Completion marker, bytes `IOSWPRB1` |
| `0x08`, `0x10`, `0x18` | Schema 1, record size 256, raw `CurrentEL` |
| `0x20`, `0x28` | `MIDR_EL1`, `MPIDR_EL1` |
| `0x30`, `0x38`, `0x40`, `0x48` | `ID_AA64PFR0_EL1`, `ID_AA64MMFR0_EL1`, `ID_AA64MMFR1_EL1`, `ID_AA64ISAR0_EL1` |
| `0x50`, `0x58`, `0x60`, `0x68`, `0x70` | `CNTFRQ_EL0`, incoming SP, argc, argv, DAIF |
| `0x78`, `0x80`, `0x88`, `0x90`, `0x98` | EL1 SCTLR, TCR, TTBR0, TTBR1, VBAR |
| `0xA0`, `0xA8`, `0xB0`, `0xB8`, `0xC0`, `0xC8` | EL2 SCTLR, TCR, TTBR0, VBAR, HCR, CNTHCTL; zero when entered at EL1 |
| `0xD0`–`0xFF` | Reserved, zero |

## FAT32 parts and native storage

FAT32's maximum individual file size is `2^32-1` bytes. Keeping every part below 4 GiB solves file storage, but does not make the native kernel, boot firmware, or block driver consume those parts. Hekate's inspected firmware loader reads single whole BL31/BL33 files; it does not concatenate arbitrary `.bin` segments. U-Boot's initial file-loading services also do not remain a runtime iPadOS storage driver after the kernel owns the machine.

The existing authenticated OS image is 6,077,580,800 bytes and must preserve its **whole-disk** byte layout, including GPT. SystemOS is 3,303,014,400 bytes. A future native read-only SD/FAT32-backed block provider can map logical byte offsets onto bounded files using a manifest with exact sizes, offsets and hashes; cross-part sector reads and final partial parts must be tested. Native SDMMC, cache/DMA coherency, FAT access and failure handling are required before this replaces the QEMU virtio disks. Do not preload the multi-gigabyte OS image into the OLED's 4 GiB RAM. The paired owner policy, trust cache, kernel and Data identity must remain consistent.

## Gates before a native iPadOS claim

1. **Done:** capture the returning probe's marker, status and actual CPU/EL/timer-register facts on the OLED while preserving the working boot setup.
2. Capture a separate `bootm` probe after the firmware handoff, including the exact OLED FDT, reserved-memory ranges, framebuffer and EL/cache state. No inherited firmware address becomes free RAM without evidence.
3. Bring up the A57/PAN-compatible XNU path, native Tegra GICv2 timer/interrupt backend, and a bounded RAM map. A development-model boot proves those modeled contracts only; Tegra interrupt security views and board MMIO still need device proof.
4. Reach an XNU serial/framebuffer checkpoint and a native user fixture on the console before original iPadOS userland.
5. Verify read-only authenticated SD-backed storage from the FAT32 parts, then the original userland's scheduler, services and screen. Native framebuffer/input and a CPU renderer are separate from a Maxwell GPU driver.

[Scarlet's project](https://github.com/petitstrawberry/scarlet-project-switch) is useful evidence that a non-Linux native OS can use the Hekate/Noble firmware route. Its [boot script](https://github.com/petitstrawberry/scarlet-project-switch/blob/main/projects/aarch64-switch-l4t-console/bootloader/boot.cmd) explicitly rejects `t210b01 != 0` or SKU other than 0, and its hardware contract is Erista/ODIN. Its addresses, panel assumptions, device-tree changes and GPU support must not be transplanted as OLED proof. Its optional Switchvisor path also introduces an EL2 monitor and is outside this native deployment target.
