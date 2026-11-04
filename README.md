# IOSwitch

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
- [Coming from the CMake flow](#coming-from-the-cmake-flow)
- [Troubleshooting](#troubleshooting)

## Prerequisites

- **A Mac on Apple silicon** with about 25 GB free. The build takes 10–13 GB; the IPSW members the installer
  needs take another 6.5 GB, fetched only when you install.
- **Xcode-beta 27.0 (27A5194q)**, selected with `sudo xcode-select -s /Applications/Xcode-beta.app`, or named by
  exporting `DEVELOPER_DIR`.
  - The kernel is compiled with Apple clang, and its code generation depends on clang's version. So the build refuses
    any other Xcode and tells you how to re-pin.
  - Apple's licence doesn't allow downloading Xcode or its SDKs automatically, so this is the one input you install
    yourself.
- **bazelisk**: `brew install bazelisk`. `.bazelversion` pins Bazel 9.3.1, and bazelisk fetches it on first use.
  Nothing else comes from Homebrew: lz4 and the LLVM tools are pinned downloads, and the unit tests get them too.
- **For the GPU bundle only**:
  - the prebuilt NVK dylibs in `out/gpu/nvk/` (r9: `libvulkan_nouveau.r9{,.host}.dylib` and their `.source` files);
  - `out/gpu-artifacts/` (the m2v library store and QuartzCore's `default.metallib`).
  - Each is checked by sha256 (`bazel/gpu/deps.bzl`). They can live elsewhere: export `IOSWITCH_NVK_DIR` and
    `IOSWITCH_GPU_ARTIFACTS`, or put `common --repo_env=IOSWITCH_NVK_DIR=...` in `user.bazelrc`.

## First build

```sh
bazel build //:boot          # kernel + boot entry, with the Metal bundle's trust cache: bazel-bin/boot/ioswitch
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
- a few MB of the IPSW (kernelcache, trust caches, device tree), by HTTP range request.

It then compiles XNU (about 80 s), about 300 driver objects, corecrypto and libpthread, links, prelinks and packages.
The Rust bridges add about 5 minutes the first time.

`bazel-bin/boot/ioswitch` is what `out/boot/ioswitch` used to be:
- `switchroot/ioswitch/`: `bl33.bin` (U-Boot with the LZ4 kernel window staged after it), `loader.uboot.bin`,
  `boot.scr`, `boot.cmd`, `bl31.bin`, `oled-sku3.dtb`, `exporter.uimg`;
- `bootloader/ini/ioswitch.ini`;
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
| Read the logs | `/usr/bin/log show --archive out/switch/device-logs/boot-MMDD-HHMM/boot.logarchive --style compact --info --debug` |
| Keep this build (rollback) | `bazel run //boot:snapshot -- 1930-r9-mything` → `out/boot/lkg/1930-r9-mything/` |
| Deploy a kept build | `bazel run //deploy:sd -- disk4 --yes --stage-gpu --boot-package=$PWD/out/boot/lkg/NAME --gpu-bundle=$PWD/out/boot/lkg/NAME/gpu` |
| Install iPadOS on a fresh card | `bazel run //install:sd -- disk4` (dry run), then `... --yes` (see below) |
| Export XNU changes | `bazel run //xnu:patch_export` |
| Format / lint | `bazel run //lint:format`, `bazel run //lint:format_check`, `bazel run //lint:tidy` |
| compile_commands.json for clangd | `bazel run //lint:compdb` |
| Tests | `bazel test //...` |

**Which disk?** `diskutil list external` shows the card, usually `disk4`. Every card command takes the whole disk and:
- refuses anything that isn't a removable card with the Switch layout: the `hos_data` FAT32 first, and one
  32 GiB `userdata`/`IOSWITCH` partition;
- never touches the eMMC, emuMMC, Android/Linux partitions, the MBR or other files on the FAT;
- is a dry run unless you add `--yes` and retype the disk name.

**Deploy** (`tools/deploy/deploy.py`, no sudo):
1. pulls the last boot's logs (as below);
2. writes only `switchroot/ioswitch/*` and `bootloader/ini/ioswitch.ini` on the FAT (changed files only, AppleDouble
   files removed);
3. replaces Data's `ioswitch/overlay`;
4. with `--stage-gpu`, installs the bundle into Data's `ngpu/addons` with the backboardd prefs;
5. unmounts the iPad volumes and ejects.

Other flags: `--no-pull-logs`, `--no-csstore`, `--no-harvest`, `--no-launchd-log`, `--launchd-check LABEL...`, `--no-eject`.

**Pull logs** (`tools/deploy/pull_logs.py`):
- Copies the unified log (`db/diagnostics`, `db/uuidtext`), launchd.log, crash reports (WebKit's at top level),
  mobileactivationd logs, migration state, the boot I/O census, the LaunchServices database and every process's m2v
  shader cache.
- Builds `boot.logarchive`.
- Merges the shader journals into `out/native-a57/root-v1/tree/drivers/TegraGM20BMetal/precompile/corpus.log`, a
  source file: commit it. The next `//:gpu` build seeds from it.
- Then unmounts and ejects (`--no-eject` keeps the card).

**Install** (`tools/install/sd.py`; read its docstring first):
- The kernel needs:
  - exactly one APFS-type partition of exactly 32 GiB (the card's `userdata`);
  - the System volume from the IPSW's OS image;
  - Data (`D1B1D053-…`) and Preboot (`4AA4A761-…`) with the UUIDs compiled into the kernel;
  - the two cryptex images in `Preboot/IOSwitch-Staging`.
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
| `//:boot` (= `//boot:package`) | The boot entry (tree artifact `bazel-bin/boot/ioswitch`). Boot-args, ADT edits and `USB_HOST` live in `tools/package/package_raw_boot.py` |
| `//:kernel` | `kernel.macho`: XNU + drivers, linked and prelinked with Apple's kexts |
| `//:gpu` (= `//gpu:bundle`) | `bazel-bin/gpu/bundle`: `ngpu/addons/{TegraGM20BMetal.bundle, .m2v-seed, .raw/pack.bin}`, `TegraGM20BMetal.trustcache`, `trust.json` |
| `//gpu:trustcache` | The bundle's trust cache module. `//:boot` appends it to the kernel's static trust cache |
| `//out/native-a57/root-v1/tree:<driver>` | One driver's objects (e.g. `:SwitchUSB`); `:xnu` is the XNU objroot; `:kernel_unprelinked` is the link |
| `//ipsw:kernelcache`, `//ipsw:static_system_trust` | The original kernelcache (decompressed) and the root+OS-cryptex trust bundle, from the IPSW |
| `@ipsw//:os`, `:system_cryptex`, `:app_cryptex`, … | IPSW members (`bazel/ipsw/pins.bzl`) |
| `//install:sd`, `//install:artifacts` | SD-card install and what it writes |
| `//deploy:sd`, `//deploy:pull_logs` | Deploy and log pull |
| `//boot:snapshot` | Copy this build to `out/boot/lkg/<name>` |
| `//xnu:patch_export`, `:patch_check`, `:patch_apply` | XNU patch ⇄ working repository |
| `//lint:format`, `:format_check`, `:format_test`, `:tidy`, `:compdb` | clang-format / clang-tidy (LLVM 23.1.4) |
| `//tests/...`, `//bazel/pinned:sha256`, `//install:*_test` | Tests |
| `//tools/repro:check`, `//tools/repro:equivalence` | Reproducibility and old-flow comparison |

Flags:
- `--//boot:smp=false`: package for one core.
- `--//boot:gpu_trustcache=//boot:no_gpu`: package without the GPU trust module.
- `--//bazel/kernel:path_root=…`, `build_date=…`, `builder=…`: what paths and the XNU version string show; they're for
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
| **One m2v patch changed** (a line in 0110) | 284.4 s | `INFO: 15 processes: 2 action cache hit, 1 internal, 14 darwin-sandbox.` |
| m2v patch reverted | 4.3 s | `INFO: 15 processes: 2 action cache hit, 14 disk cache hit, 1 internal.` |
| **Fresh output base**, disk cache warm (a new clone, or after `bazel clean --expunge`) | 78.5 s (critical path 2.2 s) | `INFO: 428 processes: 346 disk cache hit, 82 internal.` |
| An XNU patch change | ≈117 s | `INFO: 295 processes: … 294 darwin-sandbox.` (XNU plus every driver object, which include XNU's headers) |

What each scenario shows:
- **Comment added.** Only that object recompiled. It came out byte-identical, so nothing after it ran: Bazel keys on
  content, not timestamps.
- **Code changed.** Exactly five actions ran: that object, its archive, the XNU link, the prelink and the package.
  XNU, the other ~300 objects and the GPU bundle were untouched.
- **m2v patch changed.** Both cargo bridges reran, then the plugin objects (the raw stack key hashes the patches and is
  compiled in), link, sign, seed, raw pack and bundle. The kernel didn't rebuild; only the package step reruns once the
  bundle's trust cache changes.
- **Fresh output base.** Nothing executed. The time is server start, analysis and copying 346 outputs out of the disk
  cache.

Run the scenarios yourself with `bazel run //tools/bench:cache` (add `-- --cold` for the cold build). Times were
measured while other agents loaded the machine; expect less on an idle one.

## Caches and disk use

| Where | What | Cap |
|---|---|---|
| `~/Library/Caches/ioswitch-bazel/repository` | Downloads by sha256, plus Bazel 9's unpacked-repository cache. Shared by every checkout | None; about 3.5 GB for the build, plus 6.5 GB of IPSW members after an install |
| `~/Library/Caches/ioswitch-bazel/disk` | Action results (the disk cache) | `--experimental_disk_cache_gc_max_size=10G`, entries older than 30 days dropped, collected after 2 min idle |
| `~/Library/Caches/bazel/_bazel_$USER/<hash>` | The output base: this checkout's bazel-out and external repos | About 1.5–2.5 GB. `bazel clean` empties it; the disk cache refills it in seconds |

Notes:
- IPSW members are stored once. On APFS, the copy Bazel makes from the repository cache is a clone.
- The OS image (5.16 GB) is only fetched by `//install:sd`. The system cryptex is held compressed (1.3 GB) and
  unpacked (3.3 GB) only for an install.
- To shrink everything: `bazel clean --expunge`; delete `~/Library/Caches/ioswitch-bazel/disk` (rebuilt on demand);
  delete `~/Library/Caches/ioswitch-bazel/repository/contents` (unpacked repositories, re-extracted on demand).

## Adding a driver

1. Put the sources in `out/native-a57/root-v1/tree/drivers/SwitchFoo/`.
2. In `out/native-a57/root-v1/tree/BUILD.bazel`, add one call:

   ```python
   kernel_driver(name = "SwitchFoo")   # every drivers/SwitchFoo/*.cpp (sorted), then its firmware.S if it has one
   ```

3. Add `":SwitchFoo"` to the `members` of the archive it rides in. Most board drivers ride in
   `libNativeIOSwitchBlockDevice`; drivers that subclass original HID/display classes ride in `libNativeLegacyDisplay`.
   Member order is link order (`-force_load`).

The default is the IOKit C++ recipe (XNU's own compile flags) plus our warnings. More knobs, used by existing drivers:

| Knob | For |
|---|---|
| `srcs = [...]` | An explicit, ordered list. Names are relative to the driver's directory; `:path` and `//label` reach elsewhere. `.c` and `.S` work too |
| `copts`, `c_copts`, `src_copts = {"X.cpp": [...]}` | Extra flags. `{tree}`, `{build}`, `{xnu}` and `{sdk}` expand to the tree, this package's output directory, the patched XNU and the iPhoneOS SDK |
| `late = ["X.cpp"]` | A late object: subclasses an original class; the board code runs its static initializer |
| `abi_checks = {"bindings.S": "xudc X.cpp"}`, `abi_check_script` | SwitchUSB-style vtable and import checks against the original kernelcache |
| `prelink = ["com.apple.iokit.IOUSBHostFamily"]` | Original kexts to prelink |
| `prelink_cuts`, `prelink_deferred` | Dependencies cut and personalities deferred (see SwitchUSB) |
| `c_recipe = ":freestanding_c"`, `object_names = "path"` | Vendored Linux code (nvkm, BPMP, DFLL) |

`bazel build //:kernel` then compiles just the new objects, re-archives and relinks. The old CMake flow had the same
pieces; `bazel/kernel/kernel.bzl` documents each.

## Changing XNU

Bazel builds XNU from the pinned tarball plus `out/native-a57/root-v1/tree/patches/xnu-ioswitch.diff`. Work in the XNU
repository (`out/native-a57/root-v1/tree/.cache/xnu-work`, branch `ioswitch`) as before. Then:

```sh
git -C out/native-a57/root-v1/tree/.cache/xnu-work commit -am "..."
bazel run //xnu:patch_export      # rewrites the patch from the branch; commit it with your tree change
bazel build //:kernel
```

The patch is the source of truth for the build. A tree change that calls new XNU code fails to link until the patch is
exported. `bazel run //xnu:patch_check` tells you whether the patch is current. Any XNU change recompiles XNU (about
80 s) and every driver object, because they include XNU's headers.

## Bumping a pin

| Pin | Where | How |
|---|---|---|
| IPSW | `bazel/ipsw/pins.bzl` | `bazel run //bazel/ipsw:pin -- <ipsw url> iPad7,1 j120ap > bazel/ipsw/pins.bzl` (streams each member once to hash it; nothing stored) |
| XNU tag | `MODULE.bazel` `@xnu`/`@xnu_base` | New URL and sha256. The patch must apply to it |
| Apple OSS, corecrypto, lz4 | `MODULE.bazel` | Version in the URL, new sha256, `strip_prefix` |
| m2v | `bazel/gpu/deps.bzl` | Commit and sha256 of the GitHub tarball. The series is `tree/drivers/TegraGM20BMetal/patches/series` |
| NVK (prebuilt) | `bazel/gpu/deps.bzl` `HOST_FILES` | New dylib names and sha256s (`shasum -a 256`) |
| Mesa | not built yet | `bazel/gpu/deps.bzl` lists what a Mesa build needs |
| Rust | `MODULE.bazel` GPU section | `rust.toolchain(versions = [...])` |
| LLVM | `bazel/llvm/llvm.bzl` and `bazel/gpu/deps.bzl` | URL and sha256 in both (one archive) |
| Python packages | not used yet | — |
| Xcode | `bazel/host/xcode.bzl` | Select the new Xcode, then `bazel run @xcode//:print_pins` and paste |
| Pinned files | `bazel/pinned` | Replace the file, update `SHA256SUMS`, `bazel test //bazel/pinned:sha256` |

A few BUILD arguments name external repositories by their directory, which Bazel 9 derives from `MODULE.bazel`. For
example, `external/+http_archive+xnu`, `+http_archive+corecrypto`, `+http_archive+lz4` and
`+http_archive+iostoragefamily` appear in `out/native-a57/root-v1/tree/BUILD.bazel` and `tools/BUILD.bazel`. If you
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
- Every compile maps the sandbox to a fixed path root with `-ffile-prefix-map` (`/ioswitch/...`). Apple clang's UBSan
  source locations ignore that flag, so they keep only their last three path components.
- XNU's version string has a fixed date and builder (`--//bazel/kernel:build_date`, `builder`).
- XNU's make sorts symbol sets under `en_IE.UTF-8`, the locale the hardware-tested kernels were built under.
- Archives are written with `ZERO_AR_DATE=1`, and the kernel link with `-oso_prefix`.
- Code signatures are ad hoc, with no timestamp.
- The GPU plugin uses classic objc_msgSend call sites, because ld's selector stubs come out in a random order.

**The old CMake flow, compared** (`bazel run //tools/repro:equivalence -- <a CMake-built checkout>`, with the Bazel
build run with `path_root`, `build_date` and `builder` set to that checkout's values):
- **(C)** Every section of the linked kernel is identical (49 of 49).
- **(B)** The prelinker, run on the CMake kernel, reproduces its `kernel.macho` bit for bit.
- **(A)** The packaging, run on the CMake kernel, reproduces its whole `out/boot/ioswitch` bit for bit (23 files at main
  3263cdc).

What differs in `//:boot` itself, and why:
- `bl33.bin` (the staged window), `loader.uboot.bin`, `boot.cmd` and `boot.scr` follow from the linker's debug map:
  N_OSO object paths and modification times in `__LINKEDIT`, which name the CMake flow's absolute paths. They change
  the compressed window's length and stage ID, and the loader and boot script embed both.
- The default (`path_root=/ioswitch`) also changes `__FILE__` strings from your home directory to `/ioswitch/...`.
- No build can reproduce those bytes, not even a second CMake build: its debug map records each object's modification
  time.

For the GPU bundle, every file matches main's flow except the plugin. In the plugin, classic call sites replace
selector stubs, paths become `/ioswitch/...`, and 96 bytes of `llvm-as` version string differ (the official LLVM 23.1.4
build instead of Homebrew's). Because the plugin differs, the trust cache does too.

## Host inputs

The only things the build reads from the Mac, each declared and checked:
- **Xcode** (pinned version, clang, ld and SDK digests; `bazel/host/xcode.bzl`). It supplies the compiler for the kernel
  and the iOS binaries, the SDKs, `xcodebuild` (firehose), `codesign`, `xcrun ar`/`nm` and `unifdef`.
- **macOS base tools**: `/usr/bin/{make,perl,tar,patch,plutil,compression_tool}`; the installer and deploy also use
  `diskutil`, `hdiutil`, `fsck_apfs` and `ditto`.
- **The GPU bundle**:
  - the prebuilt NVK (until Mesa is built here);
  - the m2v library store;
  - QuartzCore's metallib.
  - All three are checked by sha256.
  - The seed and the raw pack run Metal on this Mac's GPU, so they never run on a remote executor. Their results are
    still identical across builds.

Pinned, unregenerable files (in `bazel/pinned`, with `SHA256SUMS`):
- the kernelcache audit outputs (`original-bindings.S`, ABI headers);
- the owner key;
- the V20 prelink report;
- the switchroot ATF/U-Boot and the OLED DTB;
- `exporter.uimg` and the BT patch RAM.

`data-v10.dbr` and the ProgressUI logo contain Apple-derived content: check before publishing the repository.

## Coming from the CMake flow

| Before | Now |
|---|---|
| `cmake -S . -B out/cmake -G Ninja -DIOSWITCH_SMP=ON && cmake --build out/cmake` | `bazel build //:boot` |
| `cmake --build out/cmake --target kernel` | `bazel build //:kernel` |
| `python3 drivers/TegraGM20BMetal/build-tegra-gm20b-metal.py ...` | `bazel build //:gpu` (and `//:boot` picks up its trust cache automatically) |
| `touch scripts/package_raw_boot.py` after a GPU rebuild | Not needed: `//:boot` depends on `//gpu:trustcache` |
| `cp -R out/boot/ioswitch out/boot/lkg/NAME` (+ `out/gpu`) | `bazel run //boot:snapshot -- NAME` |
| `PKG=... [STAGE_GPU=1] zsh out/switch/deploy-sd.sh` | `bazel run //deploy:sd -- disk4 --yes [--stage-gpu] [--boot-package=... --gpu-bundle=...]` |
| Logs part of deploy-sd.sh + `mkarchive.py` + `log show` | `bazel run //deploy:pull_logs -- disk4`, then `log show --archive .../boot.logarchive` |
| `scripts/xnu-patch.sh export / check / apply` | `bazel run //xnu:patch_export / :patch_check / :patch_apply -- DIR` |
| `scripts/lint.py format / tidy / compdb` | `bazel run //lint:format / :tidy / :compdb` |
| `python3 -m unittest ...` | `bazel test //...` |
| Edit `cmake/Drivers.cmake` for a new driver | One `kernel_driver()` in the tree's `BUILD.bazel` |
| Boot-args in `scripts/package_raw_boot.py` | The same file, now `tools/package/package_raw_boot.py` |

## Troubleshooting

- **"The selected Xcode does not match the pin"**: select Xcode-beta 27.0, or re-pin (see [Bumping a pin](#bumping-a-pin)).
  `IOSWITCH_ALLOW_UNPINNED_XCODE=1` builds anyway, with a warning; the output won't match.
- **"GPU host input … is missing"**: put the NVK r9 dylibs in `out/gpu/nvk` and the store in `out/gpu-artifacts`, or set
  `IOSWITCH_NVK_DIR` / `IOSWITCH_GPU_ARTIFACTS`.
- **The link fails with an undefined `ioswitch_*` symbol from XNU code**: the XNU patch is stale. Run
  `bazel run //xnu:patch_export`.
- **Another Bazel command is running**: one server per checkout. Agents working in the same checkout at the same time
  should pass `--output_base=<own dir> ... --experimental_convenience_symlinks=ignore` (or wait).
- **A pinned download moved** (`Checksum was ...`): the pin is doing its job. Fix the URL, never the sha256, unless you
  meant to bump.
- **Something odd after editing `.bzl` files**: `bazel clean`. The disk cache brings everything else back quickly.
- **`bazel test //lint:format_test` fails**: run `bazel run //lint:format`. It's `manual` while main still has
  unformatted files.
