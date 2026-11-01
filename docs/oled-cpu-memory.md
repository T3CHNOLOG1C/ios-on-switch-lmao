# Switch OLED: CPU, KVM and memory feasibility

> Current direction: native Hekate boot, without Linux or QEMU as the deployed runtime. This document preserves the earlier virtualization assessment and measured memory evidence. See [native CPU/GICv2 development](native-a57.md) and [native firmware handoff](native-boot.md) for the active work.

Research checked on 28 October 2026. The owner has a stock Switch OLED with working Hekate, Switchroot Ubuntu Noble and Android. A live host inventory has now been collected from that console: its installed kernel is `4.9.140-l4t`, Linux reports 3.898 GiB total RAM and 2.650 GiB available, and that kernel has virtualization disabled. The full iPadOS guest has reached Home on the development Mac under both HVF and TCG. A Switch guest boot and its actual performance remain pending.

## Earlier virtualization decision

Continue with **Hekate → the existing Switchroot Linux → QEMU TCG → source-built UniXNU → iPadOS** for the first full boot. Keep KVM as a separate development track. An existing Linux installation already solves the Switch's physical display, SD, touch and power drivers; it does not make the current XNU image compatible with the physical CPU or interrupt controller.

The likely feasibility limit is sustained CPU rendering and memory pressure, rather than whether a virtual machine can reserve a 6 GiB address space. A usable iPad experience is still unproved. Native XNU does not eliminate the CPU compatibility work and adds a Tegra platform port.

## Hardware facts and boundaries

| Topic | Evidence | Consequence |
| --- | --- | --- |
| CPU | Switchroot's T210B01 platform describes four `arm,cortex-a57` CPUs. | The guest's Cortex-A710 is an emulated target, not the OLED CPU. |
| Virtualization | Arm's Cortex-A57 manual explicitly implements EL0–EL3, including EL2. | Do not claim that the OLED lacks hardware virtualization. Linux must actually receive usable EL2 and enable KVM. |
| PAN | A57 is the original Armv8 generation; its `ID_AA64MMFR1_EL1` is reserved-zero. Hardware PAN is an Armv8.1 addition. | Current UniXNU/HakiOS PAN-enabled kernels cannot simply be substituted into a KVM launch. |
| MMU granules | A57 supports 4 KiB and 64 KiB translation granules, but not 16 KiB (Arm manual table 4-35). | Use the source-built UniXNU 4 KiB kernel/user ABI; a 16 KiB kernel is a separate incompatibility. |
| Interrupt controller | Tegra210 uses GIC-400, with GIC distributor, CPU, hypervisor and virtual CPU MMIO windows. | The initial KVM target must use GICv2, while current UniXNU's ArmCpu driver implements GICv3. |
| Memory | The stock 4 GiB OLED reports 3.898 GiB Linux `MemTotal` and 2.650 GiB baseline `MemAvailable`. Hekate has RAM-ID-specific T210B01 configurations and reserved memory. | Use the measured available memory for the experiment; do not budget the entire physical capacity for QEMU. |

Sources: [Switchroot T210B01 CPU definitions](https://github.com/CTCaer/switch-l4t-platform-t210-nx/blob/cf785c4c176499b301170d79fe57b77f365b73cd/kernel-dts/nx-platforms/tegra210b01-nx-common.dtsi#L1296), [Arm Cortex-A57 manual, sections 3.2.2 and register summary](https://documentation-service.arm.com/static/5e906b3d8259fe2368e2aa8a), [Linux PAN configuration](https://kernel.googlesource.com/pub/scm/linux/kernel/git/bpf/bpf/+/24e3a7fb60a9187e5df90e5fa655ffc94b9c4f77/arch/arm64/Kconfig), [Tegra210 GIC definition](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/nvidia/tegra210.dtsi), [Hekate SDRAM configurations](https://github.com/CTCaer/hekate/blob/45cd81ed08e30a26ba380958c7e20b55c342834b/bdk/mem/sdram.c).

The checked Switchroot source has a **2 GiB placeholder** in `memory@80000000`; that is not evidence that this OLED has 2 GiB. The running, bootloader-adjusted device tree and Linux memory accounting are authoritative for the actual launch. This distinction also matters for a future native handoff.

## Why `-accel kvm -cpu host` is not a current solution

There are independent requirements:

1. **Host support.** Linux must have working KVM, usable EL2, compatible interrupt virtualization and access to `/dev/kvm`. This console's live baseline has no `/dev/kvm` and its `/proc/config.gz` reports `CONFIG_VIRTUALIZATION=n`, so KVM is unavailable with the installed kernel. That does not establish whether the boot chain could expose EL2 to a different kernel. Cortex-A57 lacks VHE, but VHE is not required for ordinary arm64 KVM. Linux documents non-VHE operation. [Live Switch baseline](../out/switch/baseline.json), [Linux boot protocol](https://docs.kernel.org/arch/arm64/booting.html), [non-VHE memory mapping](https://kernel.org/doc/html/v6.16/arch/arm64/memory.html).
2. **CPU-compatible XNU.** UniXNU's common ARM64 and QEMU configurations set `CONFIG_XNU_NATIVE_PAN=y`. `native-arm-pan.patch` defines `__ARM_PAN_AVAILABLE__`, and `build-xnu-4k-baseline.sh` adds `-march=armv8.1-a` to selected objects when PAN is enabled. A dedicated A57 build must use the existing PAN-off compilation path and audit exception entry, user-copy helpers, kernel extensions and userspace for any remaining unsupported instructions. A successful CPU name substitution does not perform that audit. [PAN patch](https://github.com/UniXNU/UniXNU/blob/33da81310819b86d3ea62cb704f156c56407764a/patches/xnu/native-arm-pan.patch), [build switch](https://github.com/UniXNU/UniXNU/blob/33da81310819b86d3ea62cb704f156c56407764a/scripts/build-xnu-4k-baseline.sh#L550).
3. **GICv2 support.** Linux's VGICv3 API requires suitable host GIC hardware. It cannot supply the current GICv3 interface on the Tegra GICv2 host. UniXNU `ArmCpu::mapGic()` accepts only `arm,gic-v3`; `enableGic()`, IRQ dispatch and IPI paths use GICv3 system registers. Add a separate GICv2 implementation using distributor/CPU-interface MMIO and build its device tree from the actual QEMU machine. Changing a compatible string cannot implement this. [KVM VGICv3 requirements](https://docs.kernel.org/virt/kvm/devices/arm-vgic-v3.html), [pinned ArmCpu implementation](https://github.com/UniXNU/ArmCpu/blob/9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b/ArmCpu.cpp#L196).
4. **Timer, firmware and memory contract.** The TCG payload assumes 62.5 MHz. KVM uses the host CPU model and must carry the actual guest-visible architectural timer frequency. Add an explicit KVM profile to both ADT generation and launch validation; currently upstream only supports TCG and macOS HVF. Keep guest EL2 exposure (`virtualization=on`) separate from the host's ability to run a guest at EL1. The former is not how KVM is enabled. [QEMU CPU models](https://www.qemu.org/docs/master/system/arm/cpu-features), [virt machine options](https://www.qemu.org/docs/master/system/arm/virt.html), [current ADT generator](https://github.com/UniXNU/UniXNU/blob/33da81310819b86d3ea62cb704f156c56407764a/scripts/qemu-entry/make-dt.py).

The lowest-risk KVM development sequence is: prove a normal AArch64 guest on the console; prove UniXNU's open 4 KiB userspace fixture under TCG/A57/GICv2 on the Mac; then run that same compatible payload under Switch KVM. Add Apple userspace only after those gates. The HakiOS prebuilt smoke kernel is a useful TCG fixture, but is not the A57 compatibility target.

### Measured first A57 failure

A bounded local negative test reused the exact hash-checked HakiOS smoke kernel with QEMU 11.1.2 TCG, `cortex-a57` and an **emulated GICv3**. In 16.02 seconds it reached the kernel's virtual-address `_arm_init`, then its first recorded undefined-instruction exception had `ELR=0xfffffff00743995c`. Disassembly of that same binary resolves the instruction to `0xd500419f`, **`msr PAN, #1`**, at `_arm_init+0x430`. Subsequent debugger/return failures are downstream of this first exception.

This is direct evidence that PAN is the first observed blocker in this particular prebuilt image. It is not a Switch boot, does not exercise GICv2, and does not establish that the image's later page-table configuration would work on A57. The CPU's lack of 16 KiB granule support remains an independent architectural constraint. QEMU was stopped with QMP `quit`, so its zero exit status is not a boot pass.

Evidence: [bounded trace](../out/research-cpu/a57-first-fault/trace.log), [exact command](../out/research-cpu/a57-first-fault/command.json), [instruction disassembly](../out/research-cpu/a57-first-fault/first-fault-disassembly.txt), [register snapshot](../out/research-cpu/a57-first-fault/qmp.log), [result and input hash](../out/research-cpu/a57-first-fault/result.json). The trace is approximately 120 KiB; the test imposed a 10 MiB log cap.

An isolated follow-up changed the 24 immediate PAN set/clear instructions in a **copied** executable kernel to NOPs, solely to locate the next incompatibility. It progressed to the XNU banner and `vm_page_bootstrap`, then faulted at `0xfffffff00744bd70`: `mrs x8, PAN` in `_machine_switch_context+0x10`. This is not a PAN-off source build or a deployable kernel; neither the original kernel nor the launcher was modified. It demonstrates why removing one faulting instruction is insufficient. Its further progress also does not override the hardware manual's granule restrictions.

Follow-up evidence: [every changed offset and both hashes](../out/research-cpu/a57-diagnostic-pan-nop/patches.json), [serial log](../out/research-cpu/a57-diagnostic-pan-nop/serial.log), [next instruction](../out/research-cpu/a57-diagnostic-pan-nop/next-fault-disassembly.txt), [bounded result](../out/research-cpu/a57-diagnostic-pan-nop/result.json). Run time was 16.02 seconds and trace size approximately 119 KiB. Both probes are Mac-hosted CPU diagnostics, not iPadOS or Switch boot tests.

## What the memory numbers actually mean

The pinned QEMU board has **6 GiB total guest RAM**, with a **4 GiB DarwinBoot-owned window inside it**. These are not additive. The remaining address space accommodates firmware and allocations outside the XNU window. DarwinBoot requires a contiguous region in the guest's physical map; QEMU's host pages do not need to form a physically contiguous 4 GiB host allocation. [Board memory settings](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/board.toml#L86), [DarwinBoot allocation and boot arguments](https://github.com/UniXNU/DarwinBoot/blob/3325e352d7df68a9f9ad74832ed94a6acbbd7390/DarwinBootPkg/Driver/DarwinBoot.c#L401).

DarwinBoot zeros only its initially used kernel/ADT/ramdisk/trust region, not the entire owned window. The current packed Data image also stays sparse: a 1 GiB logical APFS Data volume is not automatically 1 GiB of resident RAM. The driver allocates nonzero 4 KiB pages plus a pointer table; for 1 GiB the table is approximately 2 MiB. Later guest writes allocate more pages. The original packed seed and imported pages also coexist, so their memory costs must both be counted. [Packed handoff](https://github.com/UniXNU/DarwinBoot/blob/3325e352d7df68a9f9ad74832ed94a6acbbd7390/DarwinBootPkg/Driver/PackedRamdisk.h), [RamBlock import](https://github.com/UniXNU/RamBlock/blob/25c57e0e9b68bf452dd1986914525edc9d2320bc/NativeRamBlock.cpp#L191), [sparse allocation](https://github.com/UniXNU/RamBlock/blob/25c57e0e9b68bf452dd1986914525edc9d2320bc/SparseDisk.h).

This makes a first TCG boot on a 4 GiB host worth measuring. It does **not** prove it fits. XNU metadata, userspace, translated code, framebuffer copies, QEMU, the Linux desktop and disk caches compete for the same host RAM. Host overcommit acceptance or `VmSize` is not a fit measurement. Record QEMU RSS/PSS, swap, host `MemAvailable`, major faults and any OOM events throughout boot and app use. The existing 3 GiB available-memory floor is a precaution, not a calculated working-set requirement.

For an initial experiment keep any existing swap arrangement and collect data before changing it. Zram consumes real RAM for compressed pages; zswap uses CPU to reduce swap I/O. Neither guarantees a usable UI. Do not treat a multi-gigabyte swap file as extra physical RAM or introduce it silently. [Kernel zram documentation](https://cdn.kernel.org/doc/html/latest/admin-guide/blockdev/zram.html), [zswap tradeoff](https://www.kernel.org/doc/html/v6.10/admin-guide/mm/zswap.html).

### Measured Switch baseline, 28 October 2026

The live collector identifies `Nintendo Switch (OLED model)` / `nvidia,tegra210b01`, four Arm CPU-part `0xd07` cores, Ubuntu 24.04 Noble and kernel `4.9.140-l4t`. Its [baseline report](../out/switch/baseline.json) records the following values before QEMU installation and before a guest boot:

| Resource | Live baseline | Interpretation |
| --- | --- | --- |
| Linux total RAM | **4,185,714,688 bytes = 3.898 GiB** | Linux-visible memory on the stock 4 GiB console, after platform reservations. |
| Available RAM | **2,845,716,480 bytes = 2.650 GiB** | A point-in-time host baseline; below the launcher's existing 3 GiB preflight floor. |
| Existing swap | **3 GiB logical zram**, LZ4, **0 bytes swap used** | This is the console's existing configuration. Zram is compressed storage in real RAM, not another 3 GiB of physical memory. The device's `mm_stat` reported only 4 KiB allocated at collection. |
| Available disk space | **25,740,845,056 bytes = 23.973 GiB** | Capacity at collection; transfers and package installation reduce it. |
| KVM | No `/dev/kvm`; **`CONFIG_VIRTUALIZATION=n`** | TCG is the available route with this installed kernel. This is a kernel configuration finding, not evidence that A57 lacks EL2. |

QEMU was unavailable when the baseline was collected; installation is a separate step. No VM was created by the collector and its `boot_test_performed` field is false. Switch boot measurements must be recorded separately when that test runs. This assessment does not change the existing swap setup or construct a smaller guest profile.

### Completed Mac measurements: full iPadOS, 28 October 2026

The full source-built iPadOS guest has now reached the lock screen and Home on the development Mac under HVF with CPU rendering. Touch opened Settings, which displayed a white view, and Home returned to the launcher. This establishes a running interactive guest on that host, while the Settings result remains a graphics limitation. It does not establish Switch execution or complete app compatibility.

The portable TCG bundle also completed a bounded **894.1-second** Mac run. The retained [602-second frame](../out/runs/20261028-161317-lh5nn82q/frame-000602.png) shows the lock screen; the [682-second frame](../out/runs/20261028-161317-lh5nn82q/frame-000682.png) shows Home after input. Tapping Settings launched `Preferences` as PID 344 in the [process snapshot](../out/build/tcg-processes-ui.txt), but its view remained blank/black. Home returned to SpringBoard before QMP `quit`. Thus the TCG run proves the limited lock/Home/input path on the Mac, with incomplete app rendering. Its clean emulator exit is not itself the boot evidence, and the generic runner deliberately leaves `ipad_ui_verified=false`; the screenshots and process observations provide the explicit, narrower evidence.

| Run / observation | Exact measurement | What it establishes |
| --- | --- | --- |
| HVF run `20261028-160544`, retained sampler | 86 samples, every 2 seconds; highest sampled process RSS **3,682,368 KiB = 3.512 GiB**. Sampling began after guest launch. | A real later-boot/UI process working set on macOS; neither a complete boot peak nor a Switch RAM requirement. |
| Earlier HVF observation reported during the build | **4,708,272 KiB = 4.490 GiB** process RSS. This separate observation is not in the retained 86-sample series. | Evidence of a larger early footprint, with weaker provenance than the saved time series; retain the distinction rather than merging it into the sampled maximum. |
| Portable TCG run `20261028-161317-lh5nn82q`, completed | **440 samples** at a nominal two-second interval, through **892.83 seconds** of the **894.1-second** run; highest sampled RSS **3,562,048 KiB = 3.397 GiB**, at **77.32 seconds**. Final sampled RSS was **1,313,504 KiB = 1.253 GiB**. | A measured Mac boot-and-limited-interaction working set, including lock/Home and the blank Settings attempt. Sampling can miss short peaks; lower later RSS does not establish sustained app usability or Switch fit. |

Evidence: [HVF summary](../vendor/unixnu/out/qemu/run/20261028-160544/host-memory-summary.json), [retained HVF samples](../vendor/unixnu/out/qemu/run/20261028-160544/host-memory.jsonl), [complete TCG samples](../out/runs/20261028-161317-lh5nn82q/memory.jsonl), [TCG final result](../out/runs/20261028-161317-lh5nn82q/result.json), [explicit TCG observations](../out/build/ipados-tcg-result.json), [TCG machine arguments](../out/runs/20261028-161317-lh5nn82q/command.json). The completed TCG run retained **44 automatic screenshots** with **zero observation errors** and exited with status 0 after QMP `quit`. It uses one emulated Cortex-A710, 6144 MiB guest RAM, a 128 MiB TCG translation-cache setting and a 1376×1024 framebuffer. These settings matter when comparing a later smaller profile.

The retained HVF maximum is already about **88% of the OLED's entire stock 4 GiB physical capacity**. Pure subtraction leaves only about **500 MiB** for everything else; the separate early reading exceeds 4 GiB before accounting for Linux. This arithmetic is a warning about the current configuration, **not a prediction that Linux/QEMU will report identical RSS**. macOS/HVF and Linux/TCG differ in guest-memory backing, resident/shared accounting, translation overhead, compression, page-cache behavior and host services. macOS `ps` RSS samples also omit host-wide pressure and swap/compression accounting, and sampling can miss short peaks.

The current 6 GiB/4 GiB profile should therefore remain the desktop reference build, not be advertised as fitting a stock OLED without significant memory pressure. The completed Mac TCG peak was **85% of the entire stock 4 GiB capacity** and about **765 MiB above the console's measured baseline available RAM**. Those comparisons motivate the on-console measurement; they do not predict identical Linux RSS or an inevitable OOM. The existing zram may absorb some compressible pages at a real RAM and CPU cost, but its effectiveness and the resulting responsiveness are unmeasured. Before claiming an OLED fit, collect actual QEMU `VmHWM`, RSS/PSS and swap, plus host `MemAvailable`, pressure/OOM evidence and interactions through a bounded Switch run. A separately built smaller guest/window remains a further experiment; no low-memory variant or swap change was made for this assessment.

## Memory and CPU experiments, in order

| Gate | Experiment | Required observation |
| --- | --- | --- |
| 0 | Capture installed Switchroot facts: `uname -a`, `/etc/os-release`, `lscpu`, `/proc/meminfo`, `/proc/swaps`, QEMU version/accelerators, live device-tree CPU/GIC nodes, available kernel config and KVM startup logs. | A reproducible host inventory. Record unavailable logs as unavailable. |
| 1 | Run the unchanged HakiOS TCG smoke fixture on Switch Linux without Apple images. | PID 1/uid 0 output, full command, timing and peak host memory. A kernel banner is not a pass. |
| 2 | Build and boot the pinned source-4-KiB UniXNU open userspace fixture on the Mac with the original A710/GICv3 settings. | `getpid=1` and `hw.pagesize=4096`; isolates the new source/toolchain from Apple userspace. |
| 3 | Boot the complete original iPadOS profile on the Mac under TCG. | SpringBoard, changing rendered frames, touch response, no repeated service crashes; measure working set. |
| 4 | Try matched low-memory payload variants on the Mac, initially 4 GiB guest/2.5 GiB XNU, then 3.5 GiB guest/2 GiB XNU if the first works. These are hypotheses, not supported configurations. | DarwinBoot window allocation, open-user fixture, iPadOS boot, app stability, vnode/jetsam logs and lower peak working set. |
| 5 | Run the best proven variant on Switch Linux with a bounded duration and host-memory monitoring. | No host OOM, recoverable termination, stable input and app rendering; report actual latency and frame rate. |
| 6 | Separately build PAN-off XNU, implement GICv2 and test TCG/A57 before Switch KVM. | Open userspace first, then full guest; matched timing and memory behavior under KVM. |

No Switch timing estimate follows from the Mac's six-second HakiOS test. Upstream's reported iPadOS TCG boot timing is likewise specific to its host. A one-vCPU guest does not become a four-vCPU guest by enabling multithreaded TCG; change SMP only after CPU-on, interrupt and scheduler correctness are tested.

## Concrete code work for the next build

| Change | Source locations | Required constraint |
| --- | --- | --- |
| Smaller memory profile | UniXNU-qemu `board.toml` (`dram_size`, `owned_memory_size`), `scripts/run-qemu.py` (`MEMORY_MIB`), `scripts/make-qemu-adt.py`; IOSwitch export/run contracts. | Regenerate the ADT and bundle together. Do not lower only QEMU `-m`. Keep a separate profile and original bundle. |
| Vnode sizing | UniXNU-qemu `scripts/make-qemu-adt.py` currently calculates `kern.maxvnodes` from a hardcoded 4 GiB. | Derive it from the tested owned window; do not restore BCM2837's 1024-vnode cap, which upstream reports causes vnode jetsam. |
| Entry-loop measurement | DarwinBoot `DarwinBootPkg/Driver/Entry.S` performs cache maintenance across the entire owned window, one cache line at a time. | Timestamp before/after; measure before optimizing. Preserve required cache coherency across the handoff. |
| Data-seed initialization | RamBlock `NativeRamBlock::start()` hashes every logical 4 KiB page of the 1 GiB sparse seed, including zero gaps. | Measure the hashing phase; preserve seed identity and persistence semantics in any optimization. |
| A57-compatible build | UniXNU `patches/xnu/native-arm-pan.patch`, common/board configs, `scripts/build-xnu-4k-baseline.sh`. | Disable PAN through the supported compilation option and audit emitted instructions, rather than deleting arbitrary faulting instructions. |
| GICv2 | ArmCpu `mapGic`, distributor setup, `enableGic`, IRQ acknowledge/EOI, SPI routing and IPI delivery; UniXNU ADT generators. | Single CPU/timer first; implement a separate controller backend, then SMP. |
| KVM timing | `scripts/qemu-entry/make-dt.py`, QEMU board ADT wrapper, launcher/exporter validation. | Record actual `CNTFRQ_EL0` for the KVM guest and reject mismatched artifacts. |

Sources for the extra hotspots: [DarwinBoot cache-maintenance loop](https://github.com/UniXNU/DarwinBoot/blob/3325e352d7df68a9f9ad74832ed94a6acbbd7390/DarwinBootPkg/Driver/Entry.S), [RamBlock hash loop](https://github.com/UniXNU/RamBlock/blob/25c57e0e9b68bf452dd1986914525edc9d2320bc/NativeRamBlock.cpp#L225), [QEMU ADT/vnode policy](https://github.com/xVanTuring/UniXNU-qemu/blob/97b75b1f63c6b7e895cd7646029db741d5fe5488/scripts/make-qemu-adt.py#L123).

## Native execution remains a distinct port

PAN-off/source-4-KiB XNU and GICv2 work could serve both KVM and native targets. Native additionally needs a verified Hekate/ATF/U-Boot handoff into a loader, an actual Tegra memory map with carveouts, usable console diagnostics, storage and display/input drivers, power management and recovery behavior. Neither QEMU's `virt` MMIO addresses nor its physical memory layout describe the console. Keep native experiments as separately selected boot entries, beginning with an open userspace fixture and observable return/logging, after the CPU work is demonstrated under emulation.
