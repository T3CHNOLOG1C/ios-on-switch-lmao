# ios-on-switch-lmao

iPadOS 17.7.11 (21H461, iPad7,1 / J120) booting natively on a Nintendo Switch OLED. The chain is:

```text
hekate → switchroot ATF/U-Boot → native loader → patched XNU + our drivers + Apple's kexts → iPadOS from the SD card's APFS
```

Everything is built with **Bazel**: the kernel (XNU and every driver), the boot entry, the TegraGM20B Metal bundle, the
SD-card installer and the deploy tools. The build is hermetic and reproducible: every input is pinned by version and
sha256, every action runs sandboxed, and two clean builds give the same bytes. The few inputs that can't be downloaded
are declared and checked (see [Host inputs](#host-inputs)). Older notes ([HANDOFF.md](HANDOFF.md), [AUDIT.md](AUDIT.md),
`docs/`) are history from earlier agents and are not reliable.

## Contents

- [Prerequisites](#prerequisites)
- [First build](#first-build)
- [Everyday commands](#everyday-commands)
- [Targets](#targets)
- [Watch the cache](#watch-the-cache)
- [Caches and disk use](#caches-and-disk-use)
- [Adding a driver](#adding-a-driver)
- [Changing XNU](#changing-xnu)
- [Bumping a pin](#bumping-a-pin)
- [Reproducibility and the old CMake flow](#reproducibility-and-the-old-cmake-flow)
- [Host inputs](#host-inputs)
- [Format and lint](#format-and-lint)
- [Tools and scripts](#tools-and-scripts)
- [Coming from the CMake flow](#coming-from-the-cmake-flow)
- [Troubleshooting](#troubleshooting)

## Prerequisites

- **A Mac on Apple silicon** with about 25 GB free. The build takes 10–13 GB; the IPSW members the installer
  needs take another 6.5 GB, fetched when you install, except the 5.2 GB OS image, which `//:gpu`'s first build fetches
  once for its Metal libraries.
- **Xcode-beta 27.0 (27A5194q)**, selected with `sudo xcode-select -s /Applications/Xcode-beta.app`, or named by
  exporting `DEVELOPER_DIR`.
  - The kernel is compiled with Apple clang, and its code generation depends on clang's version. So the build refuses
    any other Xcode and tells you how to re-pin.
  - Apple's licence doesn't allow downloading Xcode or its SDKs automatically, so this is the one input you install
    yourself.
- **bazelisk**: `brew install bazelisk`. `.bazelversion` pins Bazel 9.3.1, and bazelisk fetches it on first use.
  Nothing else comes from Homebrew: lz4 and the LLVM tools are pinned downloads, and the unit tests get them too.
- **For the GPU bundle only**: `~/Library/Application Support/switch-ios/gpu-device-data/m2v-device-<sha>.tar` once per machine, the data only the Switch
  makes (its driver records and Core Image's compiled kernels: `harvest.py --pack`, pinned in `gpu/deps.bzl`
  `DEVICE_DATA`). The first build copies it into the repository cache, and no checkout reads it after that. The Apple
  Metal libraries the seed and the raw pack read (QuartzCore's `default.metallib` among them) come from the IPSW, below.

## First build

```sh
bazel build //:boot          # kernel + boot entry, with the Metal bundle's trust cache: bazel-bin/boot/ios
bazel build //:gpu           # the Metal bundle, seed, raw pack and trust cache: bazel-bin/gpu/bundle
bazel test //...             # unit tests and pinned-input checks
```

The first build downloads, all by sha256 into the shared repository cache:
- XNU 10063.141.1 and the other Apple open-source tarballs (libplatform, libdispatch, AvailabilityVersions, libpthread,
  IOStorageFamily);
- corecrypto 2023;
- LLVM 23.1.4 (only 8 binaries are unpacked);
- Python 3.13;
- Rust 1.98.0;
- m2v and its crates;
- lz4 1.10.0;
- a few MB of the IPSW (kernelcache, trust caches, device tree), by HTTP range request;
- for `//:gpu`, the IPSW's 5.2 GB OS image (the member the installer also needs), once per machine: Bazel attaches it
  read-only and copies out the 152 Metal libraries `gpu/apple-libs.sha256` pins (231 MB), each into the repository
  cache by its sha256. Later fetches take them from the cache and read neither the image nor the network.

It then compiles XNU (about 80 s), about 300 driver objects, corecrypto and libpthread, links, prelinks and packages.
The Rust bridges add about 4 minutes the first time (each crate is cached on its own after that).

`bazel-bin/boot/ios` is what `out/boot/ios` used to be:
- `switchroot/ios/`: `bl33.bin` (U-Boot with the LZ4 kernel window staged after it), `loader.uboot.bin`,
  `boot.scr`, `boot.cmd`, `bl31.bin`, `oled-sku3.dtb`, `exporter.uimg`;
- `bootloader/ini/ios.ini`;
- `overlay/`: the userland overlay that deploy copies to Data.

**The quick check for other agents**: `bazel build //:kernel` (does the kernel link?) is a few seconds when nothing
changed, and recompiles only what you touched.

## Everyday commands

| What | Command |
|---|---|
| Build everything | `bazel build //:boot //:gpu` |
| Does the kernel still link? | `bazel build //:kernel` |
| Deploy to the card (dry run first) | `bazel run //deploy:sd -- disk4` then `bazel run //deploy:sd -- disk4 --yes` |
| Deploy with the Metal bundle | `bazel run //deploy:sd -- disk4 --yes --stage-gpu` |
| Remove the Metal bundle from the card | `bazel run //deploy:sd -- disk4 --yes --remove-gpu` |
| Pull logs only (+ logarchive + shader harvest) | `bazel run //deploy:pull_logs -- disk4` |
| Read the logs | `/usr/bin/log show --archive "$HOME/Library/Application Support/switch-ios/device-logs/boot-MMDD-HHMM/boot.logarchive" --style compact --info --debug` |
| Keep this build (rollback) | `bazel run //boot:snapshot -- 1930-r9-mything` → `~/Library/Application Support/switch-ios/lkg/1930-r9-mything/` |
| Deploy a kept build | `LKG="$HOME/Library/Application Support/switch-ios/lkg/NAME"; bazel run //deploy:sd -- disk4 --yes --stage-gpu --boot-package="$LKG" --gpu-bundle="$LKG/gpu"` |
| Install iPadOS on a fresh card | `bazel run //install:sd -- disk4` (dry run), then `... --yes` (see below) |
| Change XNU | Edit `kernel/xnu-10063.141.1/` and commit (see [Changing XNU](#changing-xnu)) |
| Format everything | `bazel run //:format` (or `bazel run //:format -- PATH...`) |
| Check format / lint | `bazel test //:format_test //:lint_test //:clang_tidy_test` |
| compile_commands.json for clangd | `bazel run //tools/lint:compdb` |
| Tests | `bazel test //...` |

**Which disk?** `diskutil list external` shows the card, usually `disk4`. Every card command takes the whole disk and:
- refuses anything that isn't a removable card with the Switch layout: the `hos_data` FAT32 first, and one
  32 GiB `userdata`/`iOS` partition;
- never touches the eMMC, emuMMC, Android/Linux partitions, the MBR or other files on the FAT;
- is a dry run unless you add `--yes` and retype the disk name.

**Deploy** (`tools/deploy/deploy.py`, no sudo):
1. pulls the last boot's logs (as below);
2. writes only `switchroot/ios/*` and `bootloader/ini/ios.ini` on the FAT (changed files only, AppleDouble
   files removed);
3. replaces Data's `Switch/overlay`;
4. with `--stage-gpu`, installs the bundle into Data's `ngpu/addons` with the backboardd prefs;
5. unmounts the iPad volumes and ejects.

Other flags: `--no-pull-logs`, `--no-csstore`, `--no-harvest`, `--no-launchd-log`, `--launchd-check LABEL...`, `--no-eject`.

**Pull logs** (`tools/deploy/pull_logs.py`):
- Copies the unified log (`db/diagnostics`, `db/uuidtext`), launchd.log, crash reports (WebKit's at top level),
  mobileactivationd logs, migration state, the boot I/O census, the LaunchServices database and every process's m2v
  shader cache.
- Builds `boot.logarchive`.
- Merges the shader journals into `drivers/TegraGM20BMetal/precompile/corpus.log`, a
  source file: commit it. The next `//:gpu` build seeds from it.
- Then unmounts and ejects (`--no-eject` keeps the card).

**Install** (`tools/install/sd.py`; read its docstring first):
- The kernel needs:
  - exactly one APFS-type partition of exactly 32 GiB (the card's `userdata`);
  - the System volume from the IPSW's OS image;
  - Data (`D1B1D053-…`) and Preboot (`4AA4A761-…`) with the UUIDs compiled into the kernel;
  - the two cryptex images in `Preboot/Switch-Staging`.
- `//install:sd`:
  1. builds the container in a scratch image (the 5 GB OS image is fetched by range request on first use);
  2. sets those UUIDs (rewrites the volume superblocks, checked with `fsck_apfs`);
  3. writes only the regions in use to the partition;
  4. retypes that one GPT entry;
  5. seeds Data from the OS volume's `/private/var`;
  6. stages the cryptexes, the overlay and the boot entry.
- Steps that are already done are skipped.
- It needs sudo for the raw writes and asks for it after you confirm.
- On a real card it ejects halfway: reinsert the card and rerun the same command.
- `bazel build //install:artifacts` shows what it would write.

## Targets

| Target | What |
|---|---|
| `//:boot` (= `//boot:package`) | The boot entry (tree artifact `bazel-bin/boot/ios`), one action per step (`boot/defs.bzl`). Boot-args live in `tools/package/boot_args.py`; the ADT edits, `USB_HOST` and `DISPLAY_GAMUT` in `tools/package/adt_edit.py` |
| `//:kernel` | `kernel.macho`: XNU + drivers, linked and prelinked with Apple's kexts |
| `//:gpu` (= `//gpu:bundle`) | `bazel-bin/gpu/bundle`: `ngpu/addons/{TegraGM20BMetal.bundle, .m2v-seed, .raw/pack.bin}`, `TegraGM20BMetal.trustcache`, `trust.json` |
| `//gpu:trustcache` | The bundle's trust cache module. `//:boot` appends it to the kernel's static trust cache |
| `//gpu/nvk:ios`, `//gpu/nvk:host` | NVK (`libvulkan_nouveau.dylib` + `.source`) and its macOS host twin: Mesa vendored in `third_party/mesa` (77452b90 + ios-on-switch-lmao's commits), built natively, one action per compile and generator |
| `//drivers/<Driver>` | One driver's package: its objects (e.g. `//drivers/SwitchUSB`), headers (`:headers`, `:hdrs`) and tests. XNU's objects are `//kernel/xnu-10063.141.1/<component>`; `//kernel:kernel_unprelinked` is the link |
| `//ipsw:kernelcache`, `//ipsw:static_system_trust` | The original kernelcache (decompressed) and the root+OS-cryptex trust bundle, from the IPSW |
| `@ipsw//:os`, `:system_cryptex`, `:app_cryptex`, … | The members of the IPSW kind `--//ipsw:kind` selects (`ipsw/pins.bzl`, one table of kinds). Deflated ones are inflated by a build action (`//ipsw:<kind>_<role>`) |
| `//install:sd`, `//install:artifacts` | SD-card install and what it writes |
| `//deploy:sd`, `//deploy:pull_logs` | Deploy and log pull |
| `//boot:snapshot` | Copy this build to `~/Library/Application Support/switch-ios/lkg/<name>` |
| `//kernel/xnu-10063.141.1:update_tables` | Re-record XNU's header/MIG/config tables from its make (after a makefile change) |
| `//:format`, `//:format_test`, `//:lint_test`, `//:clang_tidy_test` | Format and lint every file (see [Format and lint](#format-and-lint)) |
| `//tools/lint:py_owners`, `//tools/lint:compdb` | Every tracked `.py` file has an owning target; compile_commands.json for clangd |
| `//bazel/python:requirements.update`, `:requirements_test` | Re-lock the Python packages with uv; check the lock is current |
| `//tests/...`, the driver packages' `*_test`, `//boot:firmware_export_test`, `//install:*_test`, `//tools/deploy:deploy_test`, `//bazel/pinned:sha256` | Tests |
| `//tools/repro:check` | Reproducibility |
| `//boot:firmware_export`, `//tools/package:progressui_logo`, `//tools/package:wallpaper_1x` | Regenerate a pinned input: the RAM exporter, the ProgressUI logo, the 1x wallpaper plists |
| `//drivers/TegraGM20BMetal:<tool>` | GPU developer tools: the shader corpus (`make_corpus`, `run_corpus`, `report`, …), `ordercheck`, `check_dyld_cache_imports` |

Flags:
- `--//boot:smp=false`: package for one core.
- `--//boot:gpu_trustcache=//boot:no_gpu`: package without the GPU trust module.
- `--//kernel/rules:path_root=…`, `build_date=…`, `builder=…`: what paths and the XNU version string show; they're for
  equivalence checks.
- `--config=release`: stop at the first error.
- `--config=reproducible-check`: no caches, with an execution log.
- `--config=remote`: a shared remote cache (set the URL in `user.bazelrc`).

## Watch the cache

Bazel caches every action by a hash of exactly its inputs. Each build's last line says what ran and what was reused.
Measured on this Mac (16 cores, shared with other agents, so wall times vary):

| Scenario | Wall time | What Bazel printed |
|---|---|---|
| **Cold**: fresh output base, no disk cache (`//:kernel //:boot //:gpu`) | 380.8 s (critical path 318.5 s) | `INFO: 428 processes: 82 internal, 346 darwin-sandbox.` |
| **No-op** rebuild | 0.29 s | `INFO: 1 process: 1 internal.` |
| **Comment added** to `drivers/SwitchButtons/SwitchButtons.cpp` | 2.3 s | `INFO: 2 processes: 1 action cache hit, 1 internal, 1 darwin-sandbox.` |
| **Code changed** in that driver (a function added) | 11.7 s | `INFO: 6 processes: 1 internal, 5 darwin-sandbox.` |
| Code change reverted | 0.53 s | `INFO: 6 processes: 5 disk cache hit, 1 internal.` |
| **One m2v `.rs` edited** (a line in `third_party/m2v/crates/metal2vulkan/src/lower/arith/wide.rs`, debug profile: incremental rustc) | 15.6 s for both bridges; `//:gpu` after them 36 s (the seed 26.5 s) (load average 45-60 on 16 cores, `--config=agent`) | `INFO: 13 processes: 3 internal, 2 darwin-sandbox, 8 local.` |
| m2v edit reverted | 0.3 s (the bridges) | `INFO: 13 processes: 34 action cache hit, 10 disk cache hit, 3 internal.` |
| **Fresh output base**, disk cache warm (a new clone, or after `bazel clean --expunge`) | 78.5 s (critical path 2.2 s) | `INFO: 428 processes: 346 disk cache hit, 82 internal.` |
| **A driver header edited** (a comment in `drivers/TegraGPIO/TegraGPIO.h`) | 24.8 s (critical path 2.6 s) | `INFO: 282 processes: 24 action cache hit, 1 internal, 281 darwin-sandbox.` (every driver object: each declares all the tree's headers) |
| An XNU patch change, or a header XNU itself includes (`diagnostics/**`, `drivers/NativeSwitchBlock/*.h`, `kits/VerifiedRoot/include/**`) | ≈95–117 s | `INFO: 295 processes: … 294 darwin-sandbox.` (XNU plus every driver object, which include XNU's headers) |

What each scenario shows:
- **Comment added.** Only that object recompiled. It came out byte-identical, so nothing after it ran: Bazel keys on
  content, not timestamps.
- **Code changed.** Exactly five actions ran: that object, its archive, the XNU link, the prelink and the package.
  XNU, the other ~300 objects and the GPU bundle were untouched.
- **m2v `.rs` edited.** That crate (metal2vulkan) and its dependents only (metal2vulkan-runtime, the bridge), each
  its own rustc action, for the device's bridge and the precompile's host bridge; no LTO in the debug profile, and rustc
  compiles our crates incrementally there (sessions in `<output base>/rust_incremental`). Then the
  plugin objects (the raw stack key hashes m2v's sources and is compiled in), link, sign, seed, raw pack and bundle.
  The kernel didn't rebuild; only the package step reruns once the bundle's trust cache changes.
- **Fresh output base.** Nothing executed. The time is server start, analysis and copying 346 outputs out of the disk
  cache.

Run the scenarios yourself with `bazel run //tools/bench:cache` (add `-- --cold` for the cold build). Times were
measured while other agents loaded the machine; expect less on an idle one.

## Caches and disk use

| Where | What | Cap |
|---|---|---|
| `~/Library/Caches/switch-ios-bazel/repository` | Downloads by sha256, plus Bazel 9's unpacked-repository cache. Shared by every checkout | None; about 3.5 GB for the build, 5.4 GB more after `//:gpu`'s first build (the OS image and its 231 MB of Metal libraries), 1.3 GB more after an install |
| `~/Library/Caches/switch-ios-bazel/disk` | Action results (the disk cache) | `--experimental_disk_cache_gc_max_size=10G`, entries older than 30 days dropped, collected after 2 min idle |
| `~/Library/Caches/bazel/_bazel_$USER/<hash>` | The output base: this checkout's bazel-out and external repos | About 1.5–2.5 GB. `bazel clean` empties it; the disk cache refills it in seconds |

Notes:
- IPSW members are stored once, as fetched. On APFS, the copy Bazel makes from the repository cache is a clone.
- The OS image (5.16 GB) is fetched by `//install:sd` and, once per machine, by `@gpu_apple_libs` (`//:gpu`), which
  then keeps its Metal libraries in the repository cache file by file; neither builds nor fetches read the image again.
  The system cryptex is held compressed (1.3 GB) and inflated (3.3 GB) into the output base only for an install; that
  action stays out of the disk cache.
- To shrink everything: `bazel clean --expunge`; delete `~/Library/Caches/switch-ios-bazel/disk` (rebuilt on demand);
  delete `~/Library/Caches/switch-ios-bazel/repository/contents` (unpacked repositories, re-extracted on demand).

## Adding a driver

1. Put the sources in `drivers/SwitchFoo/`.
2. Give it a `drivers/SwitchFoo/BUILD.bazel` like `drivers/ArmGIC/BUILD.bazel` (its loads, `package`, `lint_files()`,
   `kernel_headers()`) with one call:

   ```python
   kernel_driver(name = "SwitchFoo")   # every drivers/SwitchFoo/*.cpp (sorted), then its firmware.S if it has one
   ```

3. Add `"//drivers/SwitchFoo"` to the `members` of the archive it rides in (`kernel/BUILD.bazel`). Most board drivers ride in
   `libNativeSwitchBlockDevice`; drivers that subclass original HID/display classes ride in `libNativeLegacyDisplay`.
   Member order is link order (`-force_load`).

The default is the IOKit C++ recipe (XNU's own compile flags) plus our warnings. More knobs, used by existing drivers:

| Knob | For |
|---|---|
| `srcs = [...]` | An explicit, ordered list. Names are relative to the driver's directory; `:path` and `//label` reach elsewhere. `.c` and `.S` work too |
| `copts`, `c_copts`, `src_copts = {"X.cpp": [...]}` | Extra flags. `{tree}`, `{build}`, `{xnu}` and `{sdk}` expand to the tree, this package's output directory, the patched XNU and the iPhoneOS SDK |
| `late = ["X.cpp"]` | A late object: subclasses an original class; the board code runs its static initializer |
| `abi_checks = {"bindings.S": "xudc X.cpp"}`, `abi_check = ":abi_check"` | SwitchUSB-style vtable and import checks against the original kernelcache (`abi_check` is a `py_binary`) |
| `prelink = ["com.apple.iokit.IOUSBHostFamily"]` | Original kexts to prelink |
| `prelink_cuts`, `prelink_deferred` | Dependencies cut and personalities deferred (see SwitchUSB) |
| `c_recipe = ":freestanding_c"`, `object_names = "path"` | Vendored Linux code (nvkm, BPMP, DFLL) |

`bazel build //:kernel` then compiles just the new objects, re-archives and relinks. The old CMake flow had the same
pieces; `kernel/rules/kernel.bzl` documents each.

## Changing XNU

XNU is vendored: `kernel/xnu-10063.141.1/` is Apple's xnu-10063.141.1 with our changes as ordinary commits on top
(`git log -- kernel/xnu-10063.141.1` shows each one). Edit it like any other source and commit:

```sh
$EDITOR kernel/xnu-10063.141.1/osfmk/kern/startup.c
bazel build //:kernel
```

Its headers, MIG stubs, iig, config and syscall files are Bazel actions, one per header or step
(`kernel/rules/xnu.bzl`), so a header edit recompiles only the drivers that include it. They replay tables recorded
from XNU's own make (`kernel/xnu-10063.141.1/tables/`): after changing a Makefile, a MASTER file, a `conf/files` list
or a script those run, `bazel run //kernel/xnu-10063.141.1:update_tables` and commit the tables. Code that differs per
XNU version tests `#if XNU_10063_141_1` (`//kernel:xnu_version`). XNU files keep Apple's style: `//:format` and the
linters leave them alone.

## Bumping a pin

| Pin | Where | How |
|---|---|---|
| IPSW | `ipsw/pins.bzl` | `bazel run //ipsw:pin -- --kind=<build> <ipsw url> iPad7,1 j120ap` (pins one kind in place; streams each member once to hash it; nothing stored) |
| XNU version | `kernel/xnu-<version>/` | Vendor the new tag next to the old one (pristine commit, then our commits replayed), `bazel run //kernel/xnu-<version>:update_tables`, add the version to `//kernel:xnu_version`. `MODULE.bazel` `@xnu_base` (lldb macros) follows |
| Apple OSS, corecrypto, lz4 | `MODULE.bazel` | Version in the URL, new sha256, `strip_prefix` |
| m2v | `third_party/m2v` | Vendored: the upstream tree as one commit, our changes as commits on it (`git log -- third_party/m2v`). Edit it in place; for a new upstream commit, import it and rebase ours onto it |
| Rust crates | `bazel/rust/crates.MODULE.bazel` | Edit a `crate.spec`, then `CARGO_BAZEL_REPIN=1 bazel mod deps --lockfile_mode=update` (rewrites `bazel/rust/Cargo.lock` and `crates.lock.json`) |
| NVK | `third_party/mesa` | Edit Mesa's sources and commit (one commit per change, as the series is); `bazel build //gpu/nvk:ios //gpu/nvk:host` rebuilds what the edit touches |
| Mesa | `third_party/mesa/README.ios-on-switch-lmao.md` | A new import, ios-on-switch-lmao's commits rebased onto it, the BUILD files updated where meson.build changed. If `nvk_query.cl`, `nvk_copy_indirect.cl` or their headers change, regenerate `third_party/mesa/bazel/pinned` with Mesa's mesa_clc/vtn_bindgen2 (LLVM 23.1.4) |
| The IPSW's Metal libraries | `gpu/apple-libs.sha256` | After an IPSW bump: attach the OS image read-only and regenerate with the command in its header (`libraries.py pins`) |
| Device data | `gpu/deps.bzl` `DEVICE_DATA` | `bazel run //deploy:pull_logs -- disk4` stages the device's caches, then `bazel run //drivers/TegraGM20BMetal:harvest -- --pack` writes `~/Library/Application Support/switch-ios/gpu-device-data/m2v-device-<sha>.tar` and prints the pin |
| Rust | `MODULE.bazel` | `rust.toolchain(versions = [...])` and the values of `//bazel/rust:toolchain_version` |
| LLVM | `bazel/llvm/llvm.bzl` and `gpu/deps.bzl` | URL and sha256 in both (one archive) |
| Python packages | not used yet | — |
| Xcode | `bazel/host/xcode.bzl` | Select the new Xcode, then `bazel run @xcode//:print_pins` and paste |
| Pinned files | `bazel/pinned` | Replace the file, update its line in `SHA256SUMS` (`shasum -a 256 <path>`, path relative to `bazel/pinned`), `bazel test //bazel/pinned:sha256` |

A few BUILD arguments name external repositories by their directory, which Bazel 9 derives from `MODULE.bazel`. For
example, `+http_archive+corecrypto`, `+http_archive+lz4` and
`+http_archive+iostoragefamily` appear in `kernel/BUILD.bazel` and `tools/BUILD.bazel`. If you
rename a repository, or change how it is declared, update those paths too.

## Reproducibility and the old CMake flow

```sh
bazel run //tools/repro:check                   # two from-scratch builds in fresh output bases, every file compared
```

Last run (`//:kernel //:boot //:gpu`, two fresh output bases, every cache off): each build ran
`428 processes: 82 internal, 346 darwin-sandbox`, and the comparison printed **`2829 output files, 0 differ`**. That
covers the kernel, the boot entry (window, loader, boot script, overlay binaries and their signatures), the Metal plugin
and NVK signatures, the seed and the raw pack.

What makes the outputs reproducible:
- Every compile maps the sandbox to a fixed path root with `-ffile-prefix-map` (`/switch-ios/...`). Apple clang's UBSan
  source locations ignore that flag, so they keep only their last three path components.
- XNU's version string has a fixed date and builder (`--//kernel/rules:build_date`, `builder`).
- XNU's make sorts symbol sets under `en_IE.UTF-8`, the locale the hardware-tested kernels were built under.
- Archives are written with `ZERO_AR_DATE=1`, and the kernel link with `-oso_prefix`.
- Code signatures are ad hoc, with no timestamp.
- The GPU plugin uses classic objc_msgSend call sites, because ld's selector stubs come out in a random order.

How `//:boot` differs from the old CMake flow's (the comparison tool is gone with that flow):
- `bl33.bin` (the staged window), `loader.uboot.bin`, `boot.cmd` and `boot.scr` follow from the linker's debug map:
  N_OSO object paths and modification times in `__LINKEDIT`, which name the CMake flow's absolute paths. They change
  the compressed window's length and stage ID, and the loader and boot script embed both.
- The default (`path_root=/switch-ios`) also changes `__FILE__` strings from your home directory to `/switch-ios/...`.
- No build can reproduce those bytes, not even a second CMake build: its debug map records each object's modification
  time.

For the GPU bundle, every file matches main's flow except the plugin. In the plugin, classic call sites replace
selector stubs, paths become `/switch-ios/...`, and 96 bytes of `llvm-as` version string differ (the official LLVM 23.1.4
build instead of Homebrew's). Because the plugin differs, the trust cache does too.

## Host inputs

The only things the build reads from the Mac, each declared and checked:
- **Xcode** (pinned version, clang, ld and SDK digests; `bazel/host/xcode.bzl`). It supplies the compiler for the kernel
  and the iOS binaries, the SDKs, `xcodebuild` (firehose), `codesign`, `xcrun ar`/`nm` and `unifdef`.
- **macOS base tools**: `/usr/bin/{make,perl,tar,patch,plutil,compression_tool}`; the installer and deploy also use
  `diskutil`, `hdiutil`, `fsck_apfs` and `ditto`.
- **The GPU bundle**:
  - NVK's three generated files Mesa's own CL/GLSL compilers made (`third_party/mesa/bazel/pinned`: nvkcl.spv/.cpp/.h and
    astc_spv.h, with the sha256 of every input they were generated from); Mesa, its tools and the dylibs are built;
  - the m2v library store;
  - QuartzCore's metallib.
  - Bazel copies each file in itself, through its download check against the pinned sha256 (`bazel/host/files.bzl`):
    a wrong file fails the fetch, and the build only ever sees the pinned bytes.
  - The seed and the raw pack run Metal on this Mac's GPU, so they never run on a remote executor. Their results are
    still identical across builds.

Pinned, unregenerable files (in `bazel/pinned`, with `SHA256SUMS`, checked the same way by `@pinned`):
- the kernelcache audit outputs (`original-bindings.S`, ABI headers);
- the owner key;
- the V20 prelink report;
- the switchroot ATF/U-Boot and the OLED DTB;
- `exporter.uimg` and the BT patch RAM.

`data-v10.dbr` and the ProgressUI logo contain Apple-derived content (the 1x wallpaper plists, Apple's metadata only): check before publishing the repository.

## Format and lint

Every file of ours has a formatter and, where one exists, a linter. They are Bazel rules (`bazel/lint`), the tools are
pinned (`bazel/lint/tools.bzl`, LLVM 23.1.4, the Rust toolchain), and each check is one cached action per file, so
re-checking an unchanged file costs nothing.

| Files | Format | Lint |
|---|---|---|
| C, C++, Objective-C(++) | clang-format (`.clang-format`) | clang-tidy (`.clang-tidy`), per translation unit through the build graph |
| Python | ruff format | ruff check (`ruff.toml`) |
| BUILD, `.bzl`, MODULE.bazel | buildifier fix | buildifier lint=warn, all warnings (`.buildifier.json`) |
| Shell | shfmt | shellcheck (`.shellcheckrc`) |
| Rust | rustfmt (`rustfmt.toml`) | clippy once the bridge crates are `rust_library` targets |
| Markdown | rumdl (`.rumdl.toml`, prose wrapped at 120) | rumdl |
| JSON, TOML | dprint (`dprint.json`) | (dprint fails on anything that does not parse) |

- `bazel run //:format` formats everything in place; `bazel run //:format -- PATH...` only those files and directories.
- `bazel test //:format_test` checks formatting, `//:lint_test` runs the linters, `//:clang_tidy_test` runs clang-tidy
  over every unit `//:boot` and `//:gpu` compile, warnings as errors.
- The file sets are `lint_files()` (`bazel/lint/defs.bzl`): one call in every BUILD file declares the package's files
  and chains its subpackages into `//:lint_files`. A new package needs the call; vendored and generated files go in its
  `exclude`. Code that is not ours is never reformatted or linted (and has `DisableFormat` for editors).
- Not linted: assembly (no maintained linter; the assembler at `-Werror` is the check), linker scripts, plists, device
  trees, shaders and patches.
- buildifier never reorders a list (`.buildifier-tables.json`): a driver's `srcs` are its link order, an archive's
  members its member order. `//:format` only formats; lint findings are fixed by hand.

Rebasing a patch made before a formatting change (for example the one-time format of the whole tree):

1. Check out the commit the patch was made on and apply it there (a patch from before the S3 layout first goes through
   `bazel run //tools/repath -- PATCH`).
2. Format what it touches and fold that into it: `bazel run //:format -- $(git diff --name-only HEAD~1)`, then
   `git commit --amend -a --no-edit`.
3. Rebase onto main. The formatting both sides now share applies cleanly; what conflicts is the patch's own change.
   Run `bazel run //:format -- FILE...` on anything you resolve by hand, and `bazel test //:format_test` before sending.

## Tools and scripts

Every build, package, install, deploy and log step is a Bazel target, and there are no loose scripts:
- Orchestration is Starlark: rules in `kernel/rules`, `//ipsw`, `gpu/defs.bzl` and `install/defs.bzl` declare each
  action's inputs and outputs; repository rules fetch and check what comes from outside.
- Real tools are Python, one `py_binary` per job on the hermetic rules_python 3.13 (`tools/kernel`, `tools/package`,
  `tools/prelink`, `tools/gpu`, `tools/install`, `tools/deploy`), called from those rules or by `bazel run`.
  Shared code is a `py_library`; host tests are `py_test`.
- `bazel run //tools/lint:py_owners` fails on any tracked `.py` file that no target owns. Run it before committing a new
  script; give it a target, or don't add it.

## Coming from the CMake flow

| Before | Now |
|---|---|
| `cmake -S . -B out/cmake -G Ninja -DSWITCH_SMP=ON && cmake --build out/cmake` | `bazel build //:boot` |
| `cmake --build out/cmake --target kernel` | `bazel build //:kernel` |
| `python3 drivers/TegraGM20BMetal/build-tegra-gm20b-metal.py ...` | `bazel build //:gpu` (and `//:boot` picks up its trust cache automatically) |
| `touch scripts/package_raw_boot.py` after a GPU rebuild | Not needed: `//:boot` depends on `//gpu:trustcache` |
| `cp -R out/boot/ios out/boot/lkg/NAME` (+ `out/gpu`) | `bazel run //boot:snapshot -- NAME` (into `~/Library/Application Support/switch-ios/lkg/NAME`) |
| `PKG=... [STAGE_GPU=1] zsh out/switch/deploy-sd.sh` | `bazel run //deploy:sd -- disk4 --yes [--stage-gpu] [--boot-package=... --gpu-bundle=...]` |
| Logs part of deploy-sd.sh + `mkarchive.py` + `log show` | `bazel run //deploy:pull_logs -- disk4`, then `log show --archive .../boot.logarchive` |
| `scripts/xnu-patch.sh export / check / apply` | Nothing: edit `kernel/xnu-10063.141.1/` and commit |
| `scripts/lint.py format / tidy / compdb` | `bazel run //:format`, `bazel test //:clang_tidy_test`, `bazel run //tools/lint:compdb` |
| `python3 -m unittest ...` | `bazel test //...` |
| Edit `cmake/Drivers.cmake` for a new driver | One `kernel_driver()` in the tree's `BUILD.bazel` |
| Boot-args in `scripts/package_raw_boot.py` | `tools/package/boot_args.py` (the ADT edits in `tools/package/adt_edit.py`) |
| `out/gpu/nvk/build-nvk.sh`, `relink-nvk.py`, the NVK patches | `bazel build //gpu/nvk:ios //gpu/nvk:host` (the patches are `third_party/mesa`'s commits; the relink is `third_party/mesa/bazel/link.bzl`) |
| `tree/scripts/*` (builders, audits, qemu-entry), `tree/package/Scripts/*` | Gone: their live parts are Bazel rules and tools; the rest was dead |

## Troubleshooting

- **"The selected Xcode does not match the pin"**: select Xcode-beta 27.0, or re-pin (see [Bumping a pin](#bumping-a-pin)).
  `SWITCH_ALLOW_UNPINNED_XCODE=1` builds anyway, with a warning; the output won't match.
- **`@gpu_device_data` fails to fetch (`m2v-device-….tar` in neither the repository cache nor
  `~/Library/Application Support/switch-ios/gpu-device-data/`)**: copy the tar named in `gpu/deps.bzl` `DEVICE_DATA` there
  (`$SWITCH_HOME/gpu-device-data/` if you set it; it stays out of the repository: it is Apple-derived), or make a new one from the device's caches: `bazel run //deploy:pull_logs -- disk4` stages them, then
  `bazel run //drivers/TegraGM20BMetal:harvest -- --pack` writes the tar and prints the pin.
  Without any device data (another machine), set `DEVICE_DATA = None`: the seed then lacks the device's records and Core
  Image's 2 device-compiled kernels, which the device compiles on first use.
- **`hdiutil attach` fails while fetching `@gpu_apple_libs`**: the first fetch on a machine attaches the IPSW's OS image
  read-only (no root needed). A fetch interrupted while it was attached leaves the mount behind: the next fetch detaches it,
  or run `hdiutil detach` on the `_volume` directory in `$(bazel info output_base)/external/+gpu+gpu_apple_libs`.
- **`//third_party/mesa/bazel/pinned:inputs_test` fails**: a Mesa change touched what Mesa's CL kernels (or the ASTC
  shader) read; regenerate those files with the real tools and update `third_party/mesa/bazel/pinned/inputs.txt`.
- **"no file for o/..." or "no step writes ..." from an XNU target**: a makefile change outran the tables; run
  `bazel run //kernel/xnu-10063.141.1:update_tables`.
- **Another Bazel command is running**: one server per checkout. Agents working in the same checkout at the same time
  should pass `--output_base=<own dir> ... --experimental_convenience_symlinks=ignore` (or wait).
- **A pinned download moved** (`Checksum was ...`): the pin is doing its job. Fix the URL, never the sha256, unless you
  meant to bump.
- **Something odd after editing `.bzl` files**: `bazel clean`. The disk cache brings everything else back quickly.
- **`bazel test //:format_test` fails**: run `bazel run //:format`. **`//:lint_test` or `//:clang_tidy_test` fails**: fix
  the finding it prints (each tool's config, at the root, says why every check that is off is off).
