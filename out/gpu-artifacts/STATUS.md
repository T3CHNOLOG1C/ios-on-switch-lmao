# GPU userland agent status

Paths are relative to `scratchpad/gpu/`.

## Done
- **Phase 1:** `userland/PORTS_STUDY.md`.
- **Phase 2: NVK is GO.**
  - **GM20B is accepted.** NVK takes anything ≥ KEPLER_A (`nvk_physical_device.c:1545-1549`).
  - **The only Tegra gate is the conformance check** (`:87-101,1551-1563`). It is lifted with `NVK_I_WANT_A_BROKEN_VULKAN_DRIVER=1`; the darwin backend sets it.
  - **NAK compiles SM53** through ShaderModel50 (`compiler/nak/ir.rs:9775-9786`, `sm50.rs:54-57`).
  - **NVK already handles SOCs:** memory types (`nvk_physical_device.c:1680-1700`), TegraColor GOBs (`nil/tiling.rs:147`), and SOC ASTC (`nil/format.rs:88`).
- **Phase 3:** `CONTRACT.md` and `userland/abi/TegraGM20BABI.h`.
- **Phase 4, build:** `libvulkan_nouveau.dylib` cross-builds for arm64 iOS 17 and links.
  - Output: `userland/build-ios/src/nouveau/vulkan/libvulkan_nouveau.dylib`, 17 MB, `LC_BUILD_VERSION` iOS minos 17.0.
  - It exports `vkGetInstanceProcAddr`, so the Metal frontend can open it as its loader.
  - It depends only on libSystem, libc++ and IOKit.
  - **Unresolved symbols:** with `-undefined error` instead of `dynamic_lookup`, the only undefined symbols are the generated weak entrypoints (`nvk_*`, `vk_common_*`, `wsi_*`, `vk_cmd_enqueue*`). This is the same pattern as sheng's Turnip. `nm -m` on the iOS dylib shows all 2963 of them as `weak external (dynamically looked up)`, so dyld binds them to NULL rather than failing.
  - **Host smoke test passes.** `userland/tests/nvk_smoke.c` against a macOS build (`userland/build-host`) dlopens the library, then `vkCreateInstance` returns SUCCESS. `vkEnumeratePhysicalDevices` returns SUCCESS with 0 devices, because no TegraGM20B service exists on a Mac.
  - **Reproduce:** `userland/build-nvk-ios.sh`.
- **Mesa delta:** `userland/patches/nvk-darwin.patch` against Mesa 77452b90, 19 files.
  - New code:
    - `nvkmd/darwin/nvkmd_darwin.{c,h}`: the backend, about 1.2k lines.
    - `cubin/nv_cubin_none.c`: libelf-free stub.
  - Core edits, each small and `#ifdef`/meson-gated:
    - nvkmd dispatch and `enumerate` hook.
    - LC_UUID build-id length.
    - `sys/sysmacros.h` guard.
    - `vk_drm_syncobj` guard.
    - `VK_EXT_external_memory_host` (new optional nvkmd op `import_host_mem`).
    - NAK bindings without libdrm.
    - meson: no winsys without libdrm, no libelf on darwin.
    - CTR_EL0 at EL0 (sheng's fix).
    - Skip the iOS SDK's Carbon `Endian.h`.
    - `vk_image.drm_format_mod` on Apple.

- **Phase 5, the Metal plugin bundle (2026-10-29): built.**
  - Source: `drivers/TegraGM20BMetal/TegraGM20BMetalPlugin.mm`, adapted from sheng's `NativeAdrenoMetalPlugin.mm` (published
    mode only). Principal class `TegraGM20BMetalDevice : _MTLDevice`; in backboardd, `-initWithAcceleratorPort:` returns
    `native_bridge_metal_open(<bundle>/libvulkan_nouveau.dylib, <cache dir>, registryID)`; every other process gets nil. Dropped:
    the Adreno user test, marker stream, GPU scanout hand-off, benchmark/telemetry/cache-export threads and the writes into
    QuartzCore's memory. Sets `NVK_DARWIN_SERVICE=AGXAccelerator` so NVK never opens the IOAcceleratorES stub. Logs via os_log, subsystem `com.ioswitch.TegraGM20BMetal`, every line prefixed `TegraGM20BMetal:`.
  - Build: `drivers/TegraGM20BMetal/build-tegra-gm20b-metal.py <outdir> --m2v <metal2vulkan> --nvk <libvulkan_nouveau.dylib>`.
    It builds the bridge with `cargo +1.98.0` for aarch64-apple-ios (metal2vulkan 720ef1ce + the 5 bridge patches, clone in
    `m2v-src/metal2vulkan`), links the plugin with `NativeBridgeMetalFrontend.mm` as an arm64 MH_BUNDLE, copies NVK in, and
    signs both ad hoc with 4 KiB pages (dylib first). Output of this round: `metal-build/ngpu/addons/TegraGM20BMetal.bundle`
    (plugin 21.6 MB, NVK 15.9 MB stripped; CDHashes plugin ea1c4574…, NVK a484b8f6…), `metal-build/TegraGM20BMetal.trustcache` (one v2 module, 2 CDHashes),
    `metal-build/trust.json`. Every rebuild changes the CDHashes.
  - Links against the iOS 27 SDK (no iOS 17 SDK on this Mac) with minos 17.0. A symbol missing on 21H461 would make the bundle
    or NVK fail to load: Metal then finds no device and CA stays on the CPU (no crash). Not yet checked against the 21H461
    dyld cache.
- **Placement.** Reuses sheng's addon mount, which our XNU already has: with `sheng-gpu-addon=1`, `native_owner_data_prepare()`
  mounts Data and then `native_gpu_addon_prepare()` (kern_exec.c) union-mounts `/private/var/ngpu/addons` read-only over
  `/System/Library/Extensions`, before launchd. `drivers/TegraGM20BMetal/stage-tegra-gm20b-metal.py <outdir> <Data mount>`
  copies the bundle to `<Data>/ngpu/addons/`, and merges `CAEnableDeepFramebuffer`, `CAEnableFramebufferCompression` and
  `CAEnableIOSurfaceCompression` = false into `{mobile,root}/Library/Preferences/{backboardd,.GlobalPreferences}.plist`
  (sheng's add-gpu-plugin-data.py). Tested on a scratch APFS image.
- **Trust.** `integration/package_raw_boot.py.diff` appends `out/gpu/TegraGM20BMetal.trustcache` to the static trust-cache
  bundle when it exists (4 modules; checked with sheng's loader validity rules).

- **Integration (files in `integration/`).** `Drivers.cmake.tegra-gm20b.txt`: the GM20B objects for the block archive,
  built and archived in a scratch cmake project with exactly those lines (0 warnings; every undefined symbol resolves in the
  current kernel, which has the IOGPUFamily prelink and the IOSwitchAccelStub bindings). `IOSwitchRootDiag.inc.diff`: the
  hook, `tegra_gm20b_probe()` right after `ioswitch_board_start()` in `ioswitch_root_boot_prepare()`.
  `package_raw_boot.py.diff`: the trust module. No XNU change: the addon union and the open grant already exist.
- **Power-on audit for T210B01 (subagent, verified on the rail facts).** VDD_GPU = MAX77812 M1 (DT tegra210b01-nx-pmic.dtsi
  m1vout "vdd-gpu"; M4 = vdd-cpu, M3 = vddio-ddr), address 0x33/0x31 from fuse ODM28 bit 0 (hekate max7762x.c), EN_CTRL bits
  0/4/6 = M1/M3/M4 (L4T max77812-regulator.c:29-38). Fixed in platform.c: I2C5 was initialised only in rail_enable but
  rail_set_uv runs first; the PMIC is written only if EN_CTRL reads twice the same with M4 (the running CPU) on, writes keep
  M3/M4 bits, and every write is read back (-71 otherwise); enable ramp 5 ms; ceiling 800 mV (switchroot cap, boot value
  800 mV); rail-off now blocks MC, clamps and resets first. tegra_shim.c: pll_p_out5 divider before ungating. gpu.c:
  tegra-gpu-speedo-id != 0 refused (nouveau's NAPLL path has T210 B1 constants; B01 needs C1). CAR/PMC/reset/MC flush order
  matched nvgpu gm20b_tegra_unrailgate; no GPU powergate partition on T210/B01.

## Next
1. **First hardware runs** (boot-args go in package_raw_boot.py COMMAND_LINE): `tegra-gpu=1` (rail, clocks, reset, BOOT_0,
   PRIV ring, GPCPLL), then `=2` (ACR/PMU/FECS/GPCCS: decides linux-firmware vs the L4T firmware set on B01), `=3` (channel,
   copy engine, fence by interrupt), then `tegra-gpu=4 sheng-gpu-addon=1` with the bundle staged and the trust module packaged.
2. **IOAcceleratorES stub at stage 4.** The stub ends QuartzCore's 30 s poll at once, so backboardd may ask Metal for its
   device before AGXAccelerator is published, and Metal keeps that answer for the life of the process. Asked the coordinator to
   skip the stub when tegra-gpu >= 4 (the poll then waits up to 30 s for ours).
3. **Presentation coherency.** The GPU is not IO-coherent. NativeLegacyDisplay's CPU copy of a GPU-rendered surface must not
   read stale cache lines; if the screen shows stale tiles at stage 4, invalidate the surface range before the copy.
4. **Later.** The Exec retry when the GPFIFO is full waits for all queued work to drain, where sheng waits only for the oldest
   fence. Shader/disk cache (`-Dshader-cache`), an exported-symbol list, NVK_DEBUG plumbing, a host mock of the user client,
   and a check of both binaries' imports against the 21H461 dyld cache (libc++ imports are all old-ABI). Nouveau's legacy
   GPCPLL path skips nvgpu's SYNC_MODE step (one possible clock glitch while idle). The GPU SMMU ASID (MC 0xaac) is
   unverified for M2. nvkm's volt tables ask for 950 mV and get 800 mV (enough for <= 998 MHz per switchroot).

## Log
- 2026-10-29 plugin round: Rust 1.98.0 + aarch64-apple-ios target installed (rustup); metal2vulkan cloned to
  `m2v-src/metal2vulkan` at 720ef1ce with the 5 bridge patches applied; bridge build dir `metal-build/` (m2v-target 300 MB).
- Mesa main 77452b90 cloned to `mesa-src/mesa`. Our changes are committed on branch `nvk-darwin` as e4bd722.
- nullmoth/nvidia-macos-driver cloned to `nvidia-macos-src/repo`. It is reference only (PolyForm Noncommercial); none of its code was used.
- Tools installed: brew meson 1.12.1, glslang, spirv-tools, spirv-llvm-translator 23 and libclc; cargo bindgen-cli 0.73.2 and cbindgen 0.29.4; rustup aarch64-apple-ios; venv `userland/venv`.
- Host `mesa_clc` and `vtn_bindgen2` are in `userland/build-host-clc`, linked from `userland/host-tools/bin`.
- Disk: build-ios 351 MB, build-host-clc 102 MB, mesa-src 548 MB.
