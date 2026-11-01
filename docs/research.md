# Source investigation — 28 October 2026

Current requirement: native Switch OLED execution through Hekate with FAT32-sized disk parts, preserving the existing SD layout. Linux/QEMU deployment was explicitly rejected after the initial laboratory work. Generic QEMU results below are source-build baselines, not a Tegra emulator or native port validation.

## What HackiOS actually does

The repository now calls itself HakiOS. Its loader places a patched open-source XNU kernel in a reserved physical memory window, builds Apple's device-tree/boot-argument structures, provides a framebuffer and RAM disks, and enters XNU. Custom drivers and userspace compatibility libraries bridge missing phone facilities. The closed iOS system files are supplied separately from Apple's IPSW.

The inspected revision is `88513176b1e96ecf0b764cd62c6f4e6ac71345e3`. The loader has QEMU virt and Qualcomm Redwood platform definitions. Its phone drivers are for Snapdragon/Adreno/Goodix/UFS hardware. It has no Nintendo Switch platform. Its QEMU path emulates a Cortex-A76 and GICv3 with TCG. The kernel patch explicitly enables PAN and targets newer ARM cores; Tegra X1's Cortex-A57 requires separate CPU work before native execution is a defensible claim.

Primary sources:

- [HakiOS README at the inspected revision](https://github.com/oosexgang/hakios-xnu-kernel/blob/88513176b1e96ecf0b764cd62c6f4e6ac71345e3/README.md)
- [Loader/platform definitions](https://github.com/oosexgang/hakios-xnu-kernel/blob/88513176b1e96ecf0b764cd62c6f4e6ac71345e3/xnuloader/loader.c)
- [Kernel adaptation](https://github.com/oosexgang/hakios-xnu-kernel/blob/88513176b1e96ecf0b764cd62c6f4e6ac71345e3/xnu/hakios.patch)
- [QEMU invocation](https://github.com/oosexgang/hakios-xnu-kernel/blob/88513176b1e96ecf0b764cd62c6f4e6ac71345e3/qemu/run.sh)

The verified smoke path reuses its kernel, EFI loader and raw-syscall test process. IOSwitch independently serializes the documented XNUP payload; **an XNUP payload is not a Hekate payload**. The upstream iOS image is associated with an iPhone identity, so it does not establish the requested iPad behavior.

## Why the iPad donor is UniXNU

UniXNU has an iPadOS 17.7.11 / iPad7,1 profile. Its QEMU device port supplies UEFI/DarwinBoot, VirtIO disks and input, and a CPU framebuffer renderer. The inspected QEMU revision is `97b75b1f63c6b7e895cd7646029db741d5fe5488`; the main UniXNU checkout was `33da81310819b86d3ea62cb704f156c56407764a`.

The portable interface in IOSwitch preserves its one-core Cortex-A710 TCG machine, GICv3, 6 GiB guest RAM, 4 GiB kernel window and 62.5 MHz timer. The ESP is a PCI block device; the two root images are modern VirtIO MMIO disks. The OS disk is the decompressed whole GPT disk, while SystemOS is a raw APFS container. The guest verifies block hashes against policies linked into the matching kernel. Arbitrarily rebuilding a filesystem breaks this pairing.

The upstream README reports setup/UI bring-up with CPU rendering. Its working Settings path uses a Mac-hosted Metal bridge, which this project cannot transfer to Linux by changing a command line. That is evidence of a remaining graphics problem, not evidence that every iPad app runs.

- [UniXNU OS profile](https://github.com/UniXNU/UniXNU/blob/33da81310819b86d3ea62cb704f156c56407764a/profiles/os/ipados-17.7.11-21H461/profile.toml)
- [QEMU port, build instructions and reported state](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/README.md)
- [QEMU runtime and disk topology](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/scripts/run-qemu.py)
- [Root/policy construction](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/scripts/build-os-root.py)

## Hekate's role

Hekate can launch the Switchroot L4T bootstack: ATF, U-Boot and Linux. Linux supplies existing Switch hardware drivers; QEMU can then present the virtual hardware these XNU ports expect. Hekate does not provide a general UEFI environment for either project's EFI application.

- [Hekate project](https://github.com/CTCaer/hekate)
- [Switchroot bootstack](https://wiki.switchroot.org/wiki/linux/linux-bootstack-documentation)
- [Switchroot boot configuration](https://wiki.switchroot.org/wiki/linux/linux-boot-configuration)

The entry generator preserves the working install's boot prefix and root identifiers. A label saying iOS would not make the payload boot iOS, so the generated label explicitly says Linux.

## Native port milestones

1. Identify the exact Switch revision, available memory, boot handoff and serial/log retrieval route. Prove a tiny CPU payload returning observable evidence from Hekate.
2. Establish the ARM64 handoff and physical memory map; implement the required UEFI services or replace the current UEFI-dependent loader.
3. Bring up ARMv8.0-compatible XNU CPU code, Tegra GICv2 and timers. Reach the open userspace fixture before attempting Apple's userspace.
4. Add Tegra framebuffer/storage/input support and fit the kernel plus root/Data layout into physical RAM. Preserve boot diagnostics.
5. Boot the matched iPadOS donor, then implement graphics and missing services. Prove useful app rendering, input, persistence, sleep/resume and thermals on the actual device.

None of those native milestones is implemented by this QEMU integration. The source establishes a research route, not a ready-to-install iPad replacement.
