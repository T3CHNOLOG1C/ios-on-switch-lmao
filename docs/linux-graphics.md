# Switch OLED graphics investigation: native target and Linux reference

Updated 28 October 2026. The owner's requested deployment is **native iPadOS
booted through Hekate, without Linux, virtualization or emulation**. This
document retains Linux-driver inventory and Mac guest results as research
evidence, not as an alternative deployment. The new isolated software-rendering
fixture targets native ARM64 iOS and does not depend on QEMU.

## Native software-rendering experiment

HakiOS has a concrete CPU-rendering route worth testing independently:
`CoreImage → EAGLContext → Mesa OSMesa → softpipe`. Its `eagl.c` replaces a
small set of EAGL methods inside the calling process and forwards the GLES
dispatch table into Mesa. `softpipe` is a native CPU rasterizer in C; this
path neither emulates an ARM CPU nor provides GPU acceleration. HakiOS's other
Mesa backend, freedreno, targets Adreno and does not drive the Switch GPU.
[HakiOS adapter](https://github.com/oosexgang/hakios-xnu-kernel/blob/88513176b1e96ecf0b764cd62c6f4e6ac71345e3/userland/src/eagl.c),
[Mesa adapter](https://github.com/oosexgang/hakios-xnu-kernel/blob/88513176b1e96ecf0b764cd62c6f4e6ac71345e3/userland/src/gl_osmesa.c),
[build configuration](https://github.com/oosexgang/hakios-xnu-kernel/blob/88513176b1e96ecf0b764cd62c6f4e6ac71345e3/tools/mk-mesa.sh).

HakiOS reports an iOS 16.5 `CIColorInvert` test succeeding with an explicitly
supplied EAGL context and software Mesa. Its missing `sharegroup` method had
caused an earlier crash; returning `nil` avoids reading uninitialized Apple
EAGL state. The default Core Image context and the `UseSoftwareRenderer` option
were reported to crash on that system. The successful fixture does not
establish integration into iconservicesagent, Settings, Photos or the camera.
[HakiOS experiment and recorded pixel output](https://github.com/oosexgang/hakios-xnu-kernel/blob/88513176b1e96ecf0b764cd62c6f4e6ac71345e3/docs/ru/metal.md#7-шесть-мест-которые-без-metal-падают-у-кого-есть-путь-без-него),
[upstream fixture](https://github.com/oosexgang/hakios-xnu-kernel/blob/88513176b1e96ecf0b764cd62c6f4e6ac71345e3/userland/src/metal_test.m).

The exact iPadOS 17.7.11 (`21H461`) cache used here retains `CI::GLContext`,
`internalContextWithEAGLContext:options:`, `CIEAGLContextCreate` and the EAGL
methods used by that adapter. A read-only machine-code audit found **all 457
named GLES dispatch offsets** in HakiOS's table at the same offsets in this
cache. Its original `sharegroup` accessor still dereferences the private EAGL
state that HakiOS does not initialize. These observations justify the fixture;
they do not verify method signatures, execution or workload compatibility.
[Static audit and exact cache identity](../out/research-graphics/eagl-slot-comparison.json),
[audit script](../out/research-graphics/audit_eagl.py).

QuartzCore changed from the HakiOS-observed `CA::WindowServer::EAGLServer` to
`CA::WindowServer::AccelServer` in this iPadOS cache. Consequently the isolated
Core Image experiment must pass before proposing any system compositor change.
Even a passing software filter would leave native Tegra CPU/interrupt support,
RAM layout, framebuffer handoff, touchscreen, storage and power drivers to
implement and prove. No native Switch iPadOS boot or rendering is established.

### Isolated fixture now built

The [native fixture](../out/research-graphics/README.md) compiled successfully:
a 70,848-byte executable, a 559,840-byte EAGL adapter and a 14,164,848-byte
Mesa library. All three are ARM64 Mach-O files with iOS 17.0 minimum deployment,
built using the installed iOS 27.0 SDK. Mesa enables **softpipe only**, with
LLVM, Vulkan and freedreno disabled. Only HakiOS's OSMesa ES-profile patch is
needed for this experiment. The reference system images and bundle are unchanged.

Mach-O structure and local ad hoc signatures validate. A static check of
**704 imported names** against the exact 21H461 cache and the built Mesa
library found the required dependencies and exports, including re-exports.
This rules out missing import names in that check; it does not prove dyld
loading, native trust integration, ABI behavior or execution.
[Build provenance and hashes](../out/research-graphics/fixture-build.json),
[import audit](../out/research-graphics/fixture-import-audit.json).

At runtime the fixture must identify `softpipe` and Core Image's OpenGL ES
backend, then invert a 4×4 binary-color pattern and verify all 64 RGBA bytes.
It refuses a pass if any unimplemented EAGL slot was used. A 120-second alarm
bounds the test. The small HakiOS adapter change only adds an atomic count of
missing calls; the adapter is loaded into the test process alone.
[Fixture source](../out/research-graphics/ci_softpipe_fixture.m).

**Execution is pending.** A native iOS userspace on the target and an explicit
trust/invocation path for these test files are not yet available. No emulator
was launched to test them. A future pass would establish this one Core Image
workload on its recorded native host; system composition and icon/app rendering
would still require separate tests.

## Archived Linux host-renderer proposal

The earlier Linux-host proposal would keep Switchroot responsible for hardware
and replace UniXNU's Mac host renderer with a Linux process. It does not satisfy
the current native requirement. Its source analysis is retained below because
it identifies dependencies and limits. The installed Linux NVIDIA driver reports
Vulkan 1.1.85, below the proposed metal2vulkan runtime's Vulkan 1.2 requirement.

The promising boundary is the existing `QemuMetalWire.h` protocol. It carries
Metal operations, resource handles and shared-memory offsets between
`QemuMetalRemote.cpp` in the guest and `QemuMetalHost.mm` on the Mac. The current
host imports the mapped heap as an `MTLBuffer`, creates real Metal objects and
executes requests with Apple's Metal framework. Its builder links Foundation
and Metal explicitly. The wire, guest plugin and RAM framebuffer can remain
largely intact while a new host implements the protocol with a Vulkan runtime.
This is an engineering proposal, not an implemented backend. Sources:
[wire](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/package/QemuMetal/QemuMetalWire.h),
[host](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/package/QemuMetal/QemuMetalHost.mm),
[build](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/scripts/build-qemu-metal.py).

## What the existing renderer actually proves

At QEMU-board revision `97b75b1f63c6b7e895cd7646029db741d5fe5488`, upstream reports:

| Path | Reported result | Remaining limitation |
| --- | --- | --- |
| CPU / RAM framebuffer | Welcome screen in roughly two minutes under Mac HVF | Not a Switch TCG timing; Settings remains white after CoreImage/icon-service failures |
| Mac Metal bridge | Home/lock screen composition and Settings contents | Some icons still miss their symbol |
| Input | One-finger click/drag; Home/F1 | General keyboard typing is absent |
| Safari | Exits at startup | AppOS cryptex is not mounted; adding network alone cannot fix this |
| Power | Preferences prevent idle sleep | Guest suspend is unsupported and can panic |

These are [upstream observations](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/README.md#state).
IOSwitch has now independently reproduced the CPU path under Mac HVF: lock screen by the 60-second capture, Home input unlocks, a touch opens Preferences, and Home returns. Home has blank tiles and Settings remains white; the kernel log repeatedly records iconservicesagent failures. This confirms the rendering limitation locally. The Mac Metal result and Switch GPU execution remain unverified. [Local UI evidence](../out/build/hvf-ui-verification.json).

Use an initial 1280x720 framebuffer for the Switch experiment after validating
the payload configuration. It matches the handheld display and its width is
divisible by 16, avoiding the alignment issue reported by the board. This is a
candidate test setting, not a measured performance fix. Record boot time, frame
hash changes and touch-to-visible-response time rather than assuming a visible
frame means the UI is usable.

## Vulkan on the existing Linux installation (reference only)

Switchroot documents OLED support, its NVIDIA Vulkan/OpenGL/EGL stack, touch and
Joy-Con support. Its working desktop already solves the physical display and
input-driver layer. [Switchroot features](https://wiki.switchroot.org/wiki/linux/linux-features.md)

### Measured initial Switch baseline

The [retained report](../out/switch/baseline.json) identifies
`Nintendo Switch (OLED model)` / `nintendo,aula`, Ubuntu 24.04 `noble`,
`aarch64` and kernel `4.9.140-l4t`. These are the initial observations, before
any later dependency installation or testing.

| Observation | NVIDIA hardware device | Software device |
| --- | --- | --- |
| Name and type | `NVIDIA Tegra X1 (rev B) (nvgpu)`, integrated GPU | `llvmpipe (LLVM 17.0.6, 128 bits)`, CPU |
| Driver | NVIDIA proprietary `32.3.1.0`; driver info `32.3.1` | Mesa `24.0.9-0ubuntu0.2`; driver version `0.0.1` |
| Physical-device Vulkan API | **1.1.85** | **1.3.274** |
| `timelineSemaphore` | Unobserved (`null`) | `true` |
| `bufferDeviceAddress` | Unobserved (`null`) | `true` |
| `VK_EXT_external_memory_host` | Unobserved (`null`) | Listed |
| `VK_KHR_external_memory_fd` | Listed | Listed |
| `VK_KHR_external_semaphore_fd` | Listed | Unobserved (`null`) |
| Imported-host-pointer alignment | Unobserved (`null`) | Unobserved (`null`) |

The NVIDIA report also explicitly returns `true` for `shaderInt64`,
`shaderFloat16`, both collected 8-bit storage-access features, and unformatted
storage-image reads/writes. It observes host-visible, host-coherent and
host-cached memory flags. These flags neither prove that an arbitrary shared
mapping can be imported nor establish its ordering/coherency with the guest.
The collector did not retain a complete memory-type/heap or image-format table.

`null` means the initial collector did not observe a value. A subsequent full,
headless `vulkaninfo` query retained the NVIDIA device's **complete list of 61
extensions**. That list lacks `VK_KHR_timeline_semaphore`,
`VK_KHR_buffer_device_address` and `VK_EXT_external_memory_host`. The device's
API remains **1.1.85**, before timeline semaphores and buffer device addresses
became core. These later observations resolve the initial unknown extension
entries: the required extensions are not advertised by this driver. The
Vulkan **1.1.85** version also falls below the pinned runtime's documented
**1.2** requirement.
[Full on-device report](../out/switch/graphics-probe/vulkaninfo.txt),
[NVIDIA extraction with source hash](../out/switch/graphics-probe/nvidia-capabilities.json).

The later report lists one **2,790,476,458-byte** heap and four memory types.
This integrated GPU shares physical RAM; that heap is not extra RAM. The
headless query skipped display surfaces, so its present-support field is not
evidence of a display failure. No compute dispatch, memory-import operation
or pixel correctness test was performed. These Linux capabilities also do not
provide an NVIDIA driver for native XNU.

llvmpipe's newer API and feature values belong to a CPU implementation and
cannot be credited to the NVIDIA GPU. A renderer probe must select and report
the NVIDIA physical device, and refuse software fallback when testing GPU
acceleration.

The same snapshot reports 3.898 GiB Linux-visible RAM, 2.650 GiB available RAM,
and an existing 3 GiB LZ4 zram device with effectively unused swap. Zram is
compressed memory backed by the same physical RAM, not another 3 GiB of GPU
or guest RAM. `/dev/kvm` was absent and `CONFIG_VIRTUALIZATION=n`; KVM is not
available in this running configuration. QEMU was unavailable in the initial
inventory. `glxinfo -B` exited with status 255, so OpenGL properties were not
observed; the report does not establish an OpenGL driver failure or its cause.

Keep the working NVIDIA stack for initial TCG/framebuffer and bounded GPU
diagnostics. NVIDIA's final R32.7.6 Jetson BSP advertises Vulkan 1.2, but that
does not establish a compatible upgrade for this Switchroot kernel and board.
Updating Ubuntu packages alone is not evidence of a newer Tegra GPU driver.
A driver change needs a supported, matched Switchroot kernel/userspace stack
and separate regression checks for display, touch, power and recovery.
[NVIDIA R32.7.6](https://developer.nvidia.com/embedded/linux-tegra-r3276)

The remaining graphics evidence is a direct feature/extension and memory-limit
query for the NVIDIA device, followed by an authored GPU dispatch/readback
test. Shared-memory import needs its own alignment and write/dispatch/readback
test. Record resident RAM, swap activity, shader compilation peak RAM and
cold/warm frame times for any renderer experiment.

`vulkaninfo --summary` is useful inventory, but the full report is needed for
the feature and memory checks. Keep the active ICD path and device identifiers
with each result so a test cannot silently select a different GPU or software
driver.

The baseline used the read-only collector. It can be repeated in a terminal
**on Switchroot Linux**, with Python 3.8 or later, after an intentional change:

```sh
python3 scripts/collect_switch.py --graphics --output switch-report.json
```

Use `--storage-path /path/to/existing/linux/storage` to inspect a different
filesystem. With no `--output`, it prints JSON; output files must be new.
It does not install tools or change settings. Missing tools remain unavailable
in the report. Each external query is limited to 15 seconds; GPU checks only
run with `--graphics`. The shared report omits paths, hostnames, serials, UUIDs,
network addresses and raw command diagnostics. Keep the installed ICD path
locally if needed for a later renderer test; it is deliberately not exported.

The collector queries KVM's API version without creating a VM. A returned
version does not establish that the Switch CPU can execute the target iPadOS
guest. Vulkan feature observations likewise do not prove rendering or shared
memory import. `null` means unobserved, rather than unsupported. Only explicit
feature values or complete recognized QEMU help lists produce `false` values.
[KVM query contract](https://docs.kernel.org/virt/kvm/api.html#kvm-get-api-version),
[zram statistics](https://docs.kernel.org/admin-guide/blockdev/zram.html)

NVK is a separate alternative investigation. Current Mesa documentation
requires Linux 6.6 or later, while this console runs 4.9.140-l4t. It cannot
serve as a replacement on the current kernel. Mesa also lists explicit Tegra
cache-coherency debugging and an opt-in
path for poorly tested GPUs. The broad supported NVIDIA architecture list
does not establish OLED board support on a specific kernel. Changing from
Switchroot's working BSP to a newer Nouveau kernel is a separate hardware
bring-up project. Zink provides OpenGL over Vulkan; it does not translate Metal
or Apple AIR shaders. [NVK](https://docs.mesa3d.org/drivers/nvk.html),
[Zink](https://docs.mesa3d.org/drivers/zink.html)

## Linux host candidate and its hard gaps

UniXNU already pins `steelbrain/metal2vulkan` at
`720ef1ce9ae2a2068999a8675717418b9416283e`. Its runtime accepts Metal-shaped
operations and translates AIR shaders to Vulkan. At that revision it requires
Rust 1.98, Vulkan 1.2 and timeline semaphores. It supports importing host memory
through `VK_EXT_external_memory_host` when the device can honor the mapping.
Guest IOSurface handling and presentation remain the consumer's responsibility.
[Pinned runtime contract](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/crates/metal2vulkan-runtime/README.md)

The baseline's NVIDIA Vulkan 1.1.85 device does not satisfy this contract.
The runtime's default-device selection can consider CPU devices after GPUs;
its success on this machine could therefore come from llvmpipe. Keep software
correctness checks separate from an explicitly selected NVIDIA test. Neither
changing an instance-version setting nor installing a newer Vulkan loader
demonstrates the missing physical-device capability. A Vulkan 1.1 adaptation
would require an audit and implementation of the runtime's synchronization,
feature and shader requirements, with its own correctness tests.

An adapter should target this pinned runtime once the compatibility gate is
resolved. UniXNU also applies sixteen recorded
patches to its translator for its native Adreno bridge. Audit those patches
against the Linux adapter's workloads instead of assuming the bare runtime
matches its phone result.
[Input pin](https://github.com/UniXNU/UniXNU/blob/33da81310819b86d3ea62cb704f156c56407764a/scripts/data/gpu-metal-vulkan-inputs.json),
[bridge builder and patch list](https://github.com/UniXNU/UniXNU/blob/33da81310819b86d3ea62cb704f156c56407764a/scripts/build-adreno-metal-bridge.py)

There is a particularly important incompatibility: the runtime has **no Metal
Shading Language source compiler**. The QEMU guest's `QemuMetalLibraries.mm`
forwards CoreImage-generated source through `QM_SOURCE_LIBRARY`, and its Mac
host compiles that source using Metal. The same guest also sends function
stitching graphs. Therefore implementing Vulkan draw calls and AIR translation
does not reproduce the current Mac path. Source compilation needs a separate,
demonstrated route; returning success with an empty shader is invalid.
[Guest library paths](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/package/QemuMetal/QemuMetalLibraries.mm),
[runtime coverage](https://github.com/steelbrain/metal2vulkan/blob/720ef1ce9ae2a2068999a8675717418b9416283e/docs/METAL-COVERAGE.md)

Implementation gates:

0. Resolve the NVIDIA device/runtime API mismatch. Demonstrate a supported
   driver with the required capabilities, or validate a separately scoped
   runtime adaptation. Do not let llvmpipe make this gate pass.
1. Inventory requests from a real Mac iPadOS boot and Settings interaction.
   Record request kinds, feature queries, library hashes and refusal counts.
   Capture enough resource/pipeline input to replay authored cases; a counter
   log is not a self-contained replay stream.
2. Implement the Linux wire server with explicit unsupported-operation replies.
   Validate all slot offsets, lengths, object lifetimes and completion-ring
   bounds before handling requests from a guest.
3. Prove memory semantics. Zero-copy import requires the actual driver's
   alignment/coherency support. A copying fallback needs explicit
   upload/readback and completion ordering, plus measured cost; replacing the
   buffer constructor is insufficient.
4. Map resource formats, argument-buffer addresses, function constants,
   render/compute pipelines, events/fences and readback. Keep capabilities
   limited to operations the backend really supports.
5. Classify source-compilation and stitching requests separately. Test whether
   the guest compiler can produce usable AIR and whether that output can be
   fed into the existing frontend; this is unverified. Otherwise source
   compilation remains an explicit blocker for those CoreImage paths.
6. Compare authored compute/readback and rendered fixtures against the Mac,
   then test backboardd and iconservicesagent separately. Only then claim
   Settings, icons or another application works.

UniXNU's native AdrenoMetal driver is useful reference code, but its Turnip
backend and Adreno740 kernel interface do not drive the Switch's NVIDIA GPU.
Its component manifest only records the `sheng` device as verified.
[AdrenoMetal component](https://github.com/UniXNU/UniXNU/blob/33da81310819b86d3ea62cb704f156c56407764a/components.toml#L101-L121)

## Why VirGL, Venus and rutabaga are not immediate fixes

QEMU's accelerated virtio-gpu modes expose protocols the guest must implement.
VirGL carries OpenGL/Gallium operations; Venus carries Vulkan; rutabaga can
forward GLES/Vulkan through gfxstream. The inspected iPadOS payload instead
uses its custom Metal frontend and RAM framebuffer. It has no corresponding
virtio-gpu guest stack. A `-device virtio-gpu-gl` flag cannot supply a Metal
device to CoreImage. Current QEMU documentation also lists kernel and host-blob
requirements that must be checked against Switchroot. Porting the guest stack
is larger than using the existing Metal wire seam.
[QEMU virtio-gpu documentation](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)

## Input, networking and persistent Data

| Area | Source-backed state | Smallest useful candidate and proof |
| --- | --- | --- |
| Touch | `VirtioInput.cpp` consumes absolute X/Y and left-button/touch state, and publishes one finger; input polling is every 8 ms | Verify physical touchscreen → QEMU absolute tablet → guest hit-test, including edges and rotation. Native multitouch needs a new event path; it is not enabled by the 12-entry guest touch array alone. |
| Home / Joy-Con | Home and F1 events generate the guest Home action | Use Switchroot's existing controller-to-key mapping. Prove Home exits an app; do not infer success from a Linux key event. |
| Typing | Other ordinary keyboard keys are ignored by this driver | Add a proper guest keyboard HID report device, scancode mapping, modifiers and release handling. Prove text entry, backspace, shifted text and focus changes. |
| Networking | Runner sets `-nic none`; the QEMU board provides no net driver | Adapt a modern VirtIO-MMIO Ethernet driver and XNU interface registration; attach QEMU user-mode networking only after that. Prove guest DHCP, DNS and an HTTP request independently. |
| Persistent Data | `native_ufs_store_mode()` returns zero in `LinkStubs.c`; `/private/var` changes live in the RAM overlay | Add an explicitly separate writable VirtIO-backed store or generalize the RAM overlay's backing interface. Prove a sentinel survives a clean guest reboot and verify both immutable root-image hashes stay unchanged. |

Input sources:
[VirtioInput.cpp](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/package/VirtioInput.cpp),
[TouchState.cpp](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/package/TouchState.cpp).
Network/persistence sources:
[runner](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/scripts/run-qemu.py),
[disabled store hooks](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/package/LinkStubs.c),
[Data construction](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/scripts/build-os-root.py).

HakiOS contains a compact VirtIO-MMIO network implementation with polling and
no checksum/segmentation offloads. It calls HakiOS's own network abstraction;
copying that one source file into UniXNU will not link or create a working iPad
network service. It is a reference for the transport, subject to its license
and the different XNU contracts.
[HakiOS network transport](https://github.com/oosexgang/hakios-xnu-kernel/blob/88513176b1e96ecf0b764cd62c6f4e6ac71345e3/xnu/files/bsd/dev/hakios.virtio_net.c)

QEMU disk overlays alone cannot persist `/private/var`: that mount currently
comes from a seed unpacked into the guest RAM driver. The OS and SystemOS block
policies must continue to match their immutable disks. A new writable data
device also needs explicit identity so it cannot be selected as a verified
root merely by matching a size.

## Archived Linux probe proposal

Build a **standalone NVIDIA capability and rendering probe** before undertaking
the full Metal wire server or adding another guest device. Its preflight must
report the physical-device identity and Vulkan API, reject CPU devices for a
GPU test, and explain that the baseline NVIDIA 1.1.85 device does not meet the
pinned runtime's 1.2 minimum. This expected refusal is useful evidence, not a
successful renderer test.

A small authored Vulkan 1.1 compute/readback and triangle test can independently
exercise the existing NVIDIA driver without a Metal runtime or driver upgrade.
After the runtime compatibility gate is resolved, run equivalent authored
fixtures through the pinned metal2vulkan runtime. Test a small file-backed
memory import only when supported; keep copied-memory upload/readback separate
so the cost and absence of zero-copy support remain visible. Report exact
device identity, typed refusals, peak RSS, shader compilation time and
cold/warm execution times in JSON.

This produces useful evidence without an IPSW or a multi-gigabyte guest build.
Its acceptance criterion is verified output produced by the NVIDIA GPU on
Switchroot. Native Vulkan success proves that limited workload on the driver;
success through the pinned runtime would justify implementing the wire server.
Neither proves CoreImage source compilation, iPadOS boot, usable frame rate
or enough RAM. llvmpipe can be useful for development fixtures, but cannot pass
the hardware-acceleration gate.

The current guest reserves 6 GiB virtual RAM and the Mac Metal path defaults to
an additional 2 GiB shared window with nine client slots. The kernel itself
manages a 4 GiB window and upstream reports tight free memory. Those allocations
are not a sensible on-device budget without measurement and redesign. Shrinking
only `-m` or the shared file can break the guest map or starve individual slots.
Graphics acceleration cannot solve the separate CPU compatibility and memory
constraints. [Memory configuration](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/scripts/run-qemu.py)
