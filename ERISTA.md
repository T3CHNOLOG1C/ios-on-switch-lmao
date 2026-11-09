# Erista (the 2017 Switch, T210)

Work in progress on the `erista` branch. Upstream supports the OLED (Aula, T210B01) only.

`--config=erista` (`--//bazel:board=icosa`) now picks the board for what was hardcoded to the OLED: the board ADT's
model/compatible/arm-io SoC (`tools/package/native_oled_adt.py` BOARDS), U-Boot's board test
(`boot/u-boot-script.txt`, `tools/package/boot_script.py` BOARD_TESTS: hekate's sku 0 on a T210), the emulator's
fuse page (`tools/emu/hekate.py`), and the kernel's idle-board match (`osfmk/arm64/cpu.c` accepts `nintendo,icosa`).

Decided at run time from APB_MISC_GP_HIDREV (`kits/SwitchChip`): TegraSE and the AppleKeyStore probe use SE1 only
on a T210; the loader's seed takes its DRBG output from SE1 (AHB master 14) there; the loader's EMC raise reports
`LOADER_EMC_NOT_T210B01` and leaves the clock where hekate's Minerva put it, and the kernel's TegraBPMP finds no
mailbox and says so.

Still OLED/T210B01-specific, in the order they will stop an Erista boot:

| Where | What |
|---|---|
| the fixed framebuffer in the boot script | hekate's 720x1280 framebuffer at `0xF5A00000` is assumed on both consoles; `--config=erista` packages switchroot's Icosa DTB (`//bazel/pinned:firmware/icosa.dtb`, the same L4T release as the pinned U-Boot) |
| `drivers/TegraBPMP`, `SwitchPower/SwitchPMIC.cpp` | EMC rate changes go through the B01 BPMP firmware; on a T210 the rate stays at hekate's (the loader and the kernel both skip the raise), the EMC DVFS governor has nothing to drive |
| `NativeMobileDisplay/TegraDisplayPower` | the SC7 cold display init now follows hekate's T210 branches (PLLD SDM/MISC1, DSI init-seq index, no B01 pad trimmers, T210 pad/MIPI calibration) and panel branches (JDI level-2 table, InnoLux/AUO power control, 5.5" settle, AVDD and WLED pins). Written from hekate's di.c/di.inl, never run on a T210; the LCDs' per-panel power-down register tables (`_di_dsi_panel_deinit_config_*`) are not sent before sleep in |
| `drivers/SwitchUSB` pad setup | the falcon firmware is now picked by chip (`tegra21x_xusb_firmware` on a T210); the pad controller sequence (`VREG_FIX18`, the B01 UPHY lane register file in `XusbLanes.h`, no SS clock scaling in the mailbox) is still the B01's and untested on a T210 |
| `drivers/SwitchPower/SwitchThermal.cpp` PLL ladder | on a T210 SwitchThermal now raises the CPU on the DFLL only (MAX77621 rail writes keep bit 7, no M4_CFG/ES2 steps, the T210 DFLL trips 15/66/86 C); its PLL ladder keeps the T210B01's CVB coefficients, so with `switch-no-dfll`, or if the DFLL fails, a T210 stays at hekate's clock |
| `drivers/SwitchPower/SwitchSensors.cpp`, `kits/SwitchIdle` | B01 SOC_THERM sensor set and C7 latencies |
| `drivers/TegraNVDEC`, `TegraGM20BMetal/precompile` | B01 DVFS point; shader cache keyed on chipset 0x12e (Erista reports 0x12b, which `gpu.c` already accepts) |

Same parts on both consoles, expected to carry over: RT5639 audio, BCM4356 Wi-Fi/BT, FTM4 touch, Joy-Con rails,
IMU, MAX77620 PMIC/RTC, MAX17050, BQ24193.

Not Erista-specific: the image builder needs macOS APFS tools.

## Building without a Mac

Upstream's workflows run on GitHub's hosted `xcode-27` image, so a push to `erista` runs CI and a tag equal to
`MODULE.bazel`'s version (`0.1.0-erista.1`) builds this fork's assets: the kernel, loader and overlay for both IPSW
kinds (`image-inputs-21H461.tar`, `image-inputs-24B119.tar`), `sd.zip` with the installer, and the host tools for
arm64 and Intel Macs (universal `apfs_tool`, `lz4` and the `arm64e` translator, macOS 12 up). They stay a run
artifact, `release-assets`, for 90 days; nothing is published as a GitHub release.

```sh
gh run download --repo T3CHNOLOG1C/ios-on-switch-lmao -n release-assets -D ~/lmao-assets <run id>
```

On a Mac, `bazelisk run --repo_env=SWITCH_RELEASE_DIR=$HOME/lmao-assets --//install:prebuilt=release ... //:image`
then needs no Xcode: `install/release.bzl` takes the assets and `SHA256SUMS` from that directory. `@switch_release`
selects the image inputs by `--//ipsw:kind`, so `--config=ios27` works from them too. iOS 27's kext prelink runs on
the Mac with the prebuilt translator, blacktop/ipsw for the Mac's CPU and `xcrun nm` / `c++filt` from whatever Xcode
or Command Line Tools `xcode-select` names; the ptrauth string discriminators it needs are computed in Python
(`translate/arm64e/xnu/prelink26/ptrauth_disc.py`, checked against clang by `ptrauth_disc_test` on CI) instead of by
compiling and running arm64e code. The pretranslation of the OS and the cryptexes runs the same prebuilt translator;
on a 2014 Intel Mac expect hours. The iOS 27 kernel is the debug profile's DEVELOPMENT build: xnu-12377 has no RELEASE
records yet (`kernel/xnu-12377.121.6/config/selected.bzl`). An Intel Mac gets no shader toolset, so the shader seed
still has to come from an Apple silicon Mac.

### The shader toolset on an Intel Mac

The seed's tools (`gpu/seed/tools.bzl`) run no GPU: `precompile` takes its Metal device from the project's own
bridge (`native_bridge_metal_open`), which feeds NVK's host twin. They are CPU code throughout, so an x86_64 build
is a cross-compile on the arm64 CI Mac, but everything in the chain is pinned to arm64 and to macOS 27:

| Piece | Today | For a 2014 Air on Monterey |
|---|---|---|
| `bazel/platforms`, `bazel/toolchains/apple` | `macos_user_arm64`, triple `arm64-apple-macosx27.2`, `target_cpu` arm64 | a `macos_user_x86_64` platform and toolchain, triple `x86_64-apple-macosx12.0` |
| NVK twin (`third_party/mesa/bazel`) | `MESA_PLATFORMS` macos → arm64, stub triple `arm64-apple-macos27.0`, meson flags captured from an arm64 native build | an x86_64 Mesa platform; the captured flags carry nothing arm-specific, SSE paths stay off |
| NVK's Rust (`@nvk_rust`, 1.96.1) | rustc, cargo and `rust-std` for aarch64-apple-darwin and -ios only; toolchains `exec`/`target` aarch64 | add `rust-std-1.96.1-x86_64-apple-darwin` and a toolchain for that triple |
| Metal bridge (`bazel/rust`, 1.98.0) | `extra_target_triples` aarch64 only, `MACOSX_DEPLOYMENT_TARGET=14.0`, kind `tool` = this Mac | an x86_64 triple and a kind that transitions to it with deployment target 12.0 |
| `tools/gpu/seed.py` mock/tool | `xcrun clang` with no `-target`: the runner's arm64, macOS 27 SDK default | `-arch x86_64 -mmacosx-version-min=12.0` |
| `drivers/NativeMetal/NativeBridgeMetalFrontend.mm` | built with a macOS 27 floor; `API_AVAILABLE(macos(15.0))` guards only the residency-set paths | every Metal API newer than 12 it reaches must be guarded or avoided; the count is unknown until a 12.0-floor compile runs `-Wunguarded-availability` |
| `tools/gpu/nvk_twin.py`, `tools/gpu/seed.py layout` | arch-agnostic (`ARCHES` already names x86_64) | nothing |

The last row is the risk: the toolset's floor is macOS 27 today and nothing has run it on anything older, and the
only Monterey Intel machine to test on is the Air itself.

So the seed is not built on the Intel Mac at all: `//gpu/seed:tree` and its `tree.size.json` come from a Mac
that can build them (they are derived from the IPSW, which is why `release/` refuses them as release assets,
`ipsw_free.bzl`, and why CI never builds them). `install/local_seed.bzl` wraps a directory holding the two:

```sh
bazelisk run //:image -- --//install:prebuilt=release \
  --repo_env=SWITCH_SHADER_SEED_DIR=/path/to/unpacked \
  --//install:shader_seed=@switch_shader_seed//:tree \
  --//install:shader_seed_size=@switch_shader_seed//:size
```

## An image from a friend's Mac

Any Apple silicon Mac with [Bazelisk](https://github.com/bazelbuild/bazelisk) builds the Erista image from the
asset folder above (220 MB, sent along or downloaded from the tag's run), with no Xcode and nothing as root. iOS 27's kext prelink runs `xcrun nm` and `c++filt`, so that Mac needs the
Command Line Tools (`xcode-select --install`); iOS 17 does not. Allow 30 GB free and a few hours: the IPSW is
downloaded from Apple, the shader seed is built with the release's toolset, and the arm64e volumes and cryptexes are
translated on that Mac.

```sh
git clone -b erista https://github.com/T3CHNOLOG1C/ios-on-switch-lmao
cd ios-on-switch-lmao
bazelisk run --repo_env=SWITCH_RELEASE_DIR=$HOME/lmao-assets --//install:prebuilt=release --config=ios27 --config=erista //:image
```

`out/sd/switchroot/ios/IOS27.SII.*` is the install image (`install/README.md`), split for FAT32. Zip the folder and
send it; it holds Apple's iOS, so not to a public link. On the card, from any OS:

1. Copy `switchroot/ios/` onto the FAT partition.
2. From the asset folder's `sd.zip`, copy `bootloader/payloads/install-ios.bin`, `bootloader/ini/ios.ini` and
   `bootloader/res/icon_ios_hue.bmp` next to hekate's.
3. In hekate, Payloads, run `install-ios.bin`, pick the image, and POWER writes it onto the card's Linux or Android
   partition (16 GiB at least) and makes its boot files hekate's iOS entry.

Without `--config=erista` the ADT, the boot script's board test and the DTB are the OLED's and the image does not boot
a 2017 Switch.

### A new kernel without a reinstall

hekate's iOS entry boots `switchroot/ios/bl33.bin` from the FAT: U-Boot with the boot script, the loader, the DTB and
the kernel window behind it. A kernel or loader change is a new `bl33.bin`, which needs no Mac of note (a newer tag's asset folder brings the new kernel):

```sh
bazelisk build --repo_env=SWITCH_RELEASE_DIR=$HOME/lmao-assets --//install:prebuilt=release --config=ios27 --config=erista //boot:package
```

runs on the Intel Air too, and `bazel-bin/boot/ios/switchroot/ios/bl33.bin` replaces the card's. The installer only
checks boot files against the image's manifest when it installs, and the image on the FAT stays as it was for the next
reinstall. What lives on the APFS volume (the overlay's drivers and daemons, the GPU bundle, the seed) needs
`//:image` and the installer again.
