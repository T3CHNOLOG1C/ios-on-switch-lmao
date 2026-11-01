# Native OLED CPU and interrupt-controller development

Checked 28 October 2026. The deployment target is a native Hekate firmware handoff, with no Linux or QEMU runtime on the Switch. A native register probe has now executed on the physical OLED console. The separate Mac tests below use an A57/GICv2 model to develop the CPU and interrupt-controller port before a native kernel boot; they do not model Tegra storage, display, touch, power, memory carveouts or firmware security state.

**Current result:** the user reports native `XNU VM INITIALIZED` and `BSD_INIT ENTERED` on the OLED with v2. The v3 candidate now executes an own signed PID 1 fixture in the Mac Cortex-A57/GICv2 model, with three completed system calls and a paired corrupted-signature rejection. The user has since reported the exact successful EL0 banner on the OLED. [Physical userspace result](../out/switch/native-xnu-v3-hardware-result.json). [Hardware baseline](../out/switch/native-xnu-v2-hardware-result.json), [userspace model review](../out/native-a57/userspace-v1/model-pair-review.json), [current run guide](native-xnu-run.md).

The source-only kernel and AMFI failure described below are the earlier baseline. The v3 derivative supplies the original AMFI provider and its dependencies while preserving that baseline and the known-working v2 kernel.

## Later physical EL1 handoff

The [handoff v2 photograph and independent decode](../out/switch/native-handoff-v2-hardware-result.json) now establish native AArch64 EL1 execution after U-Boot cleanup, direct framebuffer output, MMU/caches off, all exception masks set and a readable advancing physical counter. Its final FAILED banner was an incorrect HCR.HCD expectation: Cortex-A57 implements EL3, so that bit is reserved as zero and `HCR_EL2=0x80000000` is correct. The source is corrected and an EL3-present model reproduces both the original false failure and the corrected pass. [Architecture/source/model review](../out/research-native/handoff-hcr-review.json). This completes the diagnostic handoff stage without another hardware run; it does not establish native XNU, timer interrupt delivery or GIC ownership.

## Physical OLED register-probe result

The user's [probe-v3 photograph](../out/switch/IMG_5506.png) shows the 768-byte AArch64 program loaded at `0xa0000000`, returned `0`, and committed the complete `IOSWPRB1` result at `0xa0000200`. All **64 displayed u32 words** were readable and independently cross-checked. The [hardware result](../out/switch/native-probe-v3-hardware-result.json) preserves them as 32 named little-endian u64 fields, with exact offsets from [registers.S](../native/probe/registers.S), photograph/source hashes and uncertainty limits. This is photographic evidence, not an electronically acquired memory dump.

| Observation | Native result |
| --- | --- |
| `CurrentEL = 0x8` | The AArch64 probe ran at **EL2**. |
| `MIDR_EL1 = 0x411fd071` | **Cortex-A57 r1p1**; sampled affinity is 0/0/0/0. |
| `ID_AA64MMFR0_EL1 = 0x1124` | 4 KiB and 64 KiB translation granules supported; **16 KiB unsupported**. |
| `ID_AA64MMFR1_EL1 = 0` | PAN unsupported. |
| `ID_AA64ISAR0_EL1 = 0x11120` | AES/PMULL, SHA1, SHA256 and CRC32; no LSE atomics. |
| `CNTFRQ_EL0 = 0x124f800` | Firmware reports **19,200,000 Hz** for the system counter. |
| `SCTLR_EL2 = 0x30c51835` | EL2 MMU, data cache and instruction cache enabled. |
| `HCR_EL2 = 0`; `DAIF = 0` | Stage-2 translation disabled; lower-EL width selects AArch32; exception masks clear. |

CPU identification, granule and instruction fields follow the [Arm Cortex-A57 reference manual](https://documentation-service.arm.com/static/5e906b9fc8052b1608760b6b), with PAN decoded from [Arm's register definition](https://documentation-service.arm.com/static/5fad544dca04df4095c1c90c). `CNTFRQ_EL0` is software-programmed frequency metadata: the probe did not measure counter progress or CPU core frequency. [Arm counter-frequency definition](https://documentation-service.arm.com/static/6023d5362cb3723f202089b8).

The next loader must deliberately establish AArch64 EL1, including `HCR_EL2.RW`, and handle the live EL2 translation/cache state. This result proves native probe execution and return through U-Boot. It does not establish XNU execution, timer interrupt delivery, GIC ownership, a safe RAM map, secondary CPUs or native device drivers.

## Earlier source-only XNU model result

An isolated source-built XNU kernel now boots on a Cortex-A57 model with PAN disabled, registers the CPU and GICv2 controller, and handles real modeled timer interrupts and a self-IPI. It reaches the attempt to execute `/sbin/launchd`, then fails because this source-only build has no AMFI entitlement provider. The test process has **not** reached EL0. No native Switch kernel boot or iPadOS UI is established by these tests.

The source changes live only under `out/native-a57/tree`; the pinned source checkouts and desktop reference bundle were preserved. The reviewable change is [ArmCpu-gicv2.patch](../out/native-a57/ArmCpu-gicv2.patch).

## What changed

- Rebuilt source XNU with `XNU_NATIVE_PAN=0` and explicit `-march=armv8-a` for C/C++ compilation. This uses the existing source-build option, rather than patching PAN instructions out of a binary.
- Added a separate, single-CPU GICv2 path to ArmCpu: distributor and CPU-interface MMIO, architectural virtual timer PPI 27, IAR/EOIR acknowledgement with the complete SGI source bits retained, and SGIR self-IPI delivery. The existing GICv3 path remains separate. GICv2 secondary-CPU startup is explicitly unsupported.
- Matched the source-only fixture's existing `kt_fixed_zones=120` boot argument. Without it this small source build hits the allocator's per-size-class zone-count assertion before driver startup; no allocator assertion was removed.
- Corrected target-mask discovery for uniprocessor controllers: their target registers read as zero and ignore writes. A controller reporting multiple CPU interfaces still requires a nonzero, single-bit discovered target. This follows [Arm GICv2 specification section 4.3.12](https://documentation-service.arm.com/static/5f8ff21df86e16515cdbfafe).
- Accepted a GICv2 CPU-interface span of 0x100 bytes, covering the used registers through `GICC_IIDR` at 0xfc. The running OLED device tree reports this span. The model has a larger span; it does not validate the actual Tegra mapping.

## Build and fixture

The isolated source pins, path rebasing and original reference-kernel digest are retained in [isolation.json](../out/native-a57/isolation.json). Build options are in [build-options.json](../out/native-a57/build-options.json); [build-kernel.py](../out/native-a57/build-kernel.py) compiles ArmCpu before relinking the kernel.

The [instruction audit](../out/native-a57/instruction-audit.json) checks 1,010 XNU compile records for isolated source paths and PAN-off definitions. All C/C++ records use the explicit Armv8-A target; 27 assembly records use the baseline arm64 target. A scan of the final executable text finds no PAN instructions or the selected later-ISA instruction families. This is a named-instruction scan, not an exhaustive proof that every dependency and execution path supports A57. The linked source crypto module did complete its runtime self-tests in the model.

The development fixture uses one A57 CPU, GICv2, 2 GiB guest RAM and a 768 MiB XNU-owned window. Its timer is 62.5 MHz and MMIO layout is QEMU `virt`. The own signed static process asks for its PID and `hw.pagesize`; it is included so the eventual EL0 gate has concrete results. No Apple kernel extensions are prelinked into this fixture. Those dependencies require separate A57 auditing before adding Apple userspace.

The model's timer frequency and physical addresses are **not** the Switch values. The running OLED Linux device tree reports GIC distributor 0x50041000/0x1000, CPU interface 0x50042000/0x100, and a 19.2 MHz architectural timer with virtual timer PPI 27. See the [live hardware inventory](../out/switch/native-live-hardware.json). A live Linux device tree does not prove the pre-Linux handoff's complete memory map, actual timer register value, interrupt security ownership or secondary CPU state.

## Evidence and first unresolved dependency

Final qualification is recorded separately from the generic emulator exit status. The tests are bounded to 120 seconds, sample host RSS every two seconds, retain screenshots and diagnostics, impose a 10 MiB serial/debug-log stop threshold, and verify fixture hashes after shutdown. A clean QMP exit is not a boot pass.

The allocator-negative runs and the first GIC target-mask refusal remain under [runs](../out/native-a57/runs). The first successful controller run is [20261028-172949-1r3_j8cq](../out/native-a57/runs/20261028-172949-1r3_j8cq): CPU registration and GIC-ready globals were both 1, GIC version was 2, timer count was 2, and one self-IPI had been sent and received. The kernel then attempted `/sbin/launchd` and data-aborted at `accelerate_entitlement_queries+0x1c`. Its saved fault address was 0x1a0 and the `amfi` callback table pointer was zero. This identifies a missing entitlement provider in the source-only fixture, not another observed A57 instruction failure.

The [final qualification receipt](../out/native-a57/qualification.json) and [final run](../out/native-a57/runs/20261028-173402-d8dil3d2) identify kernel SHA-256 `3bb0d2d08add9d1b8d1c8ca199084ca2ca324ec87eea3df4140a3ac9acd87e5b`, 19,325,040 bytes. This run lasted **27.83 seconds**, peaked at **308,912 KiB sampled RSS**, reproduced CPU/GIC registration and one self-IPI, and reached the same AMFI fault. Its timer count was **zero** before the fast early fault; the two-timer observation belongs to the preceding build, whose only subsequent source change relaxed the CPU-interface span check. Do not merge those observations into one run or claim sustained timer operation.

The [fault disassembly](../out/native-a57/amfi-fault-disassembly.txt) resolves the exact instruction to `ldr x8, [x8, #0x1a0]`, immediately after loading the null AMFI table. The final saved state reports FAR 0x1a0 and ESR 0x96000007. All final fixture files passed post-run hash checks. The canonical source-kernel digest still matches the pre-experiment value in `isolation.json`, and the canonical ArmCpu source has no working-tree diff.

## What remains

The correct existing integration route is the board's `[kernel.prelink]` configuration: [the pinned QEMU board](../vendor/unixnu-qemu/board.toml) includes the original `com.apple.driver.AppleMobileFileIntegrity` and selects its exact-kernelcache early initializer at `0xfffffff005ab4ae0`. [NativePrelink](../vendor/unixnu/kits/NativePrelink/emit_prelink.py) checks the selected component and initializer address and records the relocated early-init metadata. [Upstream security documentation](../vendor/unixnu/docs/concepts/security.md) describes the original AMFI/AppleImage4/trust-cache path. The XNU `amfi_interface_register()` function only registers a provider; it does not supply one itself.

The next userspace gate therefore needs a separate A57 prelink profile with the exact original AMFI dependency closure and its required platform services audited for A57, correctly initialized, and paired with the normal verified code-signing path. The existing ApplePMGR contract is another platform dependency to inspect. The desktop board's full prelink configuration is a reference for that work, not a safe native Switch configuration to copy unchanged. Do not turn the missing callback into an unconditional success stub to claim a boot. Then establish the own test process's EL0 execution, PID 1 and 4 KiB page-size result; audit and add the original Apple dependencies afterward.

The physical probe now establishes the U-Boot application entry's exception level and reported timer frequency. Native firmware work still needs the actual memory map and carveouts, counter progress, accessible nonsecure GIC groups and safe diagnostics. The controller code assumes firmware has already assigned the OS interrupts appropriately; it does not change security ownership from EL1. SGIs are architecturally always enabled on GICv2; this fixture only generates SGI 0. External peripheral SPIs and SMP have not been exercised. Continue with the [native boot plan](native-boot.md) before constructing a device XNU entry.


## Bounded original-AMFI audit

The [static audit summary](../out/native-a57/amfi-audit/summary.json) uses the exact original iPad7,1 kernelcache with SHA-256 `f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7`. Selecting only `com.apple.driver.AppleMobileFileIntegrity` as the prelink root yields this **minimum declared dependency closure**, before any extra runtime service requirements:

| Component | Reason it enters the closure |
| --- | --- |
| AppleMobileFileIntegrity | Selected root and entitlement provider |
| ApplePMGR | AMFI dependency |
| AppleARMPlatform | ApplePMGR dependency |
| CoreAnalyticsFamily | AMFI dependency |
| AppleImage4 | AMFI dependency |
| CoreTrust | AMFI / AppleImage4 dependency |
| corecrypto | AMFI / CoreTrust / AppleImage4 dependency |
| IOCryptoAcceleratorFamily | AppleImage4 dependency |
| IOReportFamily | AppleARMPlatform / ApplePMGR dependency |
| IOSlowAdaptiveClockingFamily | AppleARMPlatform dependency |
| IOStorageFamily | AppleARMPlatform dependency |

The [unmodified prelink planner result](../out/native-a57/amfi-audit/prelink-plan.json) resolves **16,276 kernel pointer bindings and 7,753 component pointer bindings** against the final A57 source kernel, with seven kernel-interface resources. There was no missing declared dependency or pointer dependency. This is a static link-plan check: no prelinked kernel was emitted and no original module was run.

The [machine-code inventory](../out/native-a57/amfi-audit/report.json) scans 1,248,404 bytes of original executable segments. It found no PAN, LSE atomics, selected later acquire/release instructions, non-hint pointer authentication or selected later crypto instructions. Original corecrypto contains AES, PMULL, SHA1 and SHA256 instructions; the console's [live baseline](../out/switch/baseline.json) reports the corresponding CPU features. Six BTI instructions use the backward-compatible hint encoding; on older Armv8-A implementations those hints provide no BTI protection and act as NOPs. [Arm compiler reference](https://documentation-service.arm.com/static/641d6f0fac798355e7426b54).

This is not an ISA clearance: **253 words in corecrypto remain unclassified** after excluding two hash-identified SHA256 constant tables. Some resemble vector constants, but the audit has not proved every executable/data boundary. Inline jump tables also decode as `udf`; those are not automatically reachable instructions. The report retains every unresolved word and all disassembly. Only one decoded system-register operation was observed in the closure: ApplePMGR reads `MPIDR_EL1`.

The [rechecked PMGR contract](../out/native-a57/amfi-audit/pmgr-contract.json) confirms AMFI searches for `ApplePMGRNub`, obtains its provider and calls provider vtable slots 320/321 for a read / bit-2 OR / write sequence. The original iPad-specific provider's corresponding methods return a constant and perform no MMIO, but that does not establish the service lifecycle on Tegra. AMFI still needs a correctly published service and compatible provider. The existing QEMU profile enables `XNU_SHENG_PMGR` for this integration; that QEMU/Sheng provider must be audited separately before native reuse.

The next configuration should therefore preserve the original early-security initializer (`0xfffffff005ab4ae0`, relocated to `0xfffffff004ab4ae0` for the existing slide), enable the real runtime trust-cache path, and use the prelinked crypto mode and required source provider archives coherently. The current diagnostic source-only configuration uses source crypto and has no runtime trust-cache provider, so merely appending AMFI bytes would be incomplete. Establish the original AMFI table ownership and checked own-process signature path first; only then move from the own fixture to real iPadOS, with its separate owner-root and device-identity requirements. No fallback that returns unconditional entitlement success was introduced.
