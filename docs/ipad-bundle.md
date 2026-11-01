# Preparing a real iPadOS bundle

The pinned source-built image has reached iPadOS's lock and Home screens under both Mac HVF and the exported portable TCG runner. The bundle is in `bundles/ipados-17.7.11-tcg`. Touch launched Settings and Home returned to SpringBoard in both tests; Settings contents remained white under HVF and black under TCG. This is partial Mac UI proof, not a usable app experience or Switch boot. See [verification](verification.md).

The TCG test reached the lock screen by 602 seconds and Home after input by 682 seconds. It exited cleanly after 894.1 seconds, peaked at 3,562,048 KiB sampled QEMU RSS, and verified all 11 bundle files again after use. The guest's disk driver authenticated both virtual disks and 906,715 block reads with no integrity or I/O failures. [Runtime and post-use receipts](../out/build/ipados-tcg-result.json).

For a rebuild, use a development volume with room for the IPSW, source/build trees, extracted volumes and exported raw copies. The full IPSW plus just one set of raw OS/SystemOS disks already exceeds 15 GiB; the planning reserve is 50 GiB free for the complete workflow.

Start with [UniXNU's host requirements](https://unixnu.com/guide/requirements) and the [QEMU port's build instructions](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/README.md). Use the compatible framework/device pair in [build-readiness.md](build-readiness.md); newer framework revisions changed object paths expected by this QEMU port. A fresh QEMU iPad userspace build involves the open kernel, UEFI package, matching original OS components, verified root policies and a writable Data seed.

Important changes to the upstream Mac example for a Linux-bound bundle:

1. Build and stage for **TCG**, including the final root payload. Use `UNIXNU_QEMU_ACCEL=tcg` instead of the example's `hvf`, then stage with `run-qemu.py --os-root --accel tcg`. The exporter checks the device tree's 62.5 MHz timer and rejects an HVF payload.
2. Keep the default 6 GiB guest and 4 GiB DarwinBoot window. Use the CPU renderer and omit the Mac Metal addon. Do not package a Metal Data seed or a Metal owner-trust payload as a portable image.
3. Preserve the Data seed's volume UUID, matching kernel policies, trust cache, original disk bytes and staged `unixnu.toml`. The build includes a local owner key; it is **not** needed on the Switch and must stay in the build environment.
4. Use the staged run's `esp/`, not the unexpanded `payload.img`. The staged ESP contains `BOOTAA64.EFI`, DarwinBoot and the QemuRamfb driver with its configuration.
5. The local bundle uses upstream's `--skip-setup` seed preparation, because normal Setup waits on an unsupported service. The original Data seed is retained in the build tree. No Apple account was entered, and the prepared guest has no network device.

## Getting raw OS bytes

The upstream runner attaches `044-76040-038.dmg` read-only with macOS `hdiutil` and hands QEMU the raw **whole** GPT device. Its root policy authenticates precisely that byte stream. To make a portable file, copy that read-only whole image device into `OS.raw`, with the verified image's exact whole-disk length. Never select a physical disk or a single APFS partition. An exporter accepting a random `/dev/diskN` would be too easy to misuse, so this project accepts regular files only.

The SystemOS input, `044-76034-038.dmg`, is already a raw APFS container in this upstream profile. Its extension alone does not describe its format. The exporter accepts it when the APFS signature and full block geometry match, and stores the byte-for-byte copy as `disks/systemos.raw`. Compressed UDIF DMGs are rejected.

Verify that the raw export matches the root policy produced by that same build before using it. IOSwitch checks structural signatures and file hashes; it does not independently verify UniXNU's signed block policy. A structurally valid but mismatched image will fail in the guest.

`scripts/export_os_disk.py` performs the Mac-side whole-disk copy. Supply the exact source image and its completed `build/verified-root-os/verification.json` report. It derives and validates the read-only virtual device from that image, compares the complete copied bytes with the authenticated report, and detaches its own image mapping. It refuses an already attached source or an existing output:

```sh
python3 scripts/export_os_disk.py \
  --image /path/to/044-76040-038.dmg \
  --verification /path/to/build/verified-root-os/verification.json \
  --output /path/to/OS.raw
```

## Ubuntu Noble dependencies

The [actual Switch baseline](../out/switch/baseline.json) confirms Ubuntu Noble, kernel `4.9.140-l4t`, about 3,992 MiB total RAM and 2,714 MiB available at collection time. QEMU was absent in that baseline. The official ARM64 catalog provides Python 3.12 and QEMU 8.2.2, which includes the Cortex-A710 model; confirm the installed runtime before launching. [Ubuntu Python package](https://packages.ubuntu.com/en/noble/python3), [Ubuntu QEMU package](https://packages.ubuntu.com/noble/arm64/qemu-system-arm), [QEMU 8.2.2 model definition](https://github.com/qemu/qemu/blob/v8.2.2/target/arm/tcg/cpu64.c#L1195).

The existing 3 GiB lz4 zram swap device uses the same physical RAM; it is not 3 GiB of additional memory. The Mac's 3.40 GiB sampled QEMU peak cannot establish that the full guest fits on this console. The baseline has no KVM device and reports virtualization disabled in the installed kernel. No console boot result is implied by this inventory.

When ready to install missing runtime dependencies on the Switch's Linux desktop:

```sh
sudo apt update
sudo apt install python3 qemu-system-arm qemu-efi-aarch64 qemu-system-gui
```

The GUI package supplies GTK/SDL display modules; the firmware package includes `/usr/share/AAVMF/AAVMF_CODE.fd`. [GUI package](https://packages.ubuntu.com/noble/qemu-system-gui), [firmware file list](https://packages.ubuntu.com/noble/all/qemu-efi-aarch64/filelist). The exported bundle also carries the exact firmware used in the Mac test.

Optional diagnostic tools are `vulkan-tools` (`vulkaninfo`) and `mesa-utils` (`glxinfo`), available in Noble's `universe` component. The initial collector works without them and reports them as unavailable. Their installation is not required to send the first report. [Vulkan tools](https://packages.ubuntu.com/search?keywords=vulkan-tools), [Mesa utilities](https://packages.ubuntu.com/noble/mesa-utils).

## Export and launch

From the IOSwitch directory:

```sh
python3 scripts/export_unixnu.py \
  --esp /absolute/path/to/prepared-run/esp \
  --firmware /absolute/path/to/edk2-aarch64-code.fd \
  --os-raw /absolute/path/to/OS.raw \
  --systemos-raw /absolute/path/to/044-76034-038.dmg \
  --source-revision ACTUAL_40_CHARACTER_UNIXNU_QEMU_COMMIT \
  --output bundles/ipad
```

Obtain the revision from `git -C /path/to/UniXNU-qemu rev-parse HEAD`. It records provenance supplied by the builder; it is not a cryptographic build attestation. Default guest resolution is upstream's `1376x1024`. `--screen 1280x720` is an optional bring-up experiment for the Switch panel, not verified iPad layout support.

Copy the complete bundle and IOSwitch project to Switchroot's Linux filesystem. The raw OS image exceeds FAT32's single-file limit, so it does not belong on the Hekate FAT32 boot partition. The generated Hekate INI is the small file that belongs there.

Run `python3 ioswitch.py doctor`, then the smoke test, then:

```sh
python3 ioswitch.py run --bundle /path/to/bundles/ipad --dry-run
python3 ioswitch.py run --bundle /path/to/bundles/ipad --display gtk --fullscreen
```

Keep run output outside the bundle. Files are authenticated by SHA-256 before launch and both root disks are attached read-only. Data changes remain in guest RAM and are discarded. `result.json` records the process outcome but never calls an unobserved iPad UI or Switch boot successful.

The iPadOS guest's kernel log is not on the serial port in this UniXNU port. The upstream `qemu-ps.py` can inspect it with its own runtime. IOSwitch records serial/emulator output, sampled host memory, and guest screenshots through a private local QMP socket. UI inspection and an input-response check are still required; a successful process launch is not a boot verdict. For a black screen, inspect the guest kernel log and processes through the upstream tools before concluding it is only a display problem.
