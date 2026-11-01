# Minimal native OLED Apple DeviceTree

The [generator](../scripts/native_oled_adt.py) describes the physical OLED board for the first single-CPU XNU diagnostic. It imports no model tree or executable QEMU generator. Its `build(output)` entry point creates `kernel.adt` and a manifest in a new directory below `out`; it never accesses the SD card or console. The manifest hashes the generator, hardware records and audited source consumers.

| Resource | Encoded value | Evidence and consumer |
| --- | --- | --- |
| Root | `name=device-tree`, `model=Nintendo Switch (OLED model)`; compatible list `nvidia,fric`, `nintendo,aula`, `nintendo,nx`, `nvidia,tegra210b01` | [Live OLED tree summary](../out/switch/native-platform-summary.json); [ArmDTPlatform personality](../out/native-a57/tree/drivers/ArmDTPlatform/Info.plist) matches the root name, so no QEMU or iPad compatibility is needed |
| CPU | Only `cpu0`, `state=running`, `device_type=cpu`, `compatible=arm,cortex-a57`, LE u32 `reg=0` | [Native probe](../out/switch/native-probe-v3-hardware-result.json): MIDR `0x411fd071`, MPIDR `0x80000000` (affinity zero, bit 31 is RES1); `ml_parse_cpu_topology` and `ArmCpu::probe/start` consume the affinity |
| Counter | LE u32 `timebase-frequency=19200000` | [Native post-cleanup EL1 observation](../out/switch/native-handoff-v2-hardware-result.json); `pe_identify_machine` consumes the running CPU's timebase |
| GIC distributor | Physical `0x50041000`, size `0x1000` | [Live OLED tree summary](../out/switch/native-platform-summary.json); `ArmCpu::mapGic` |
| GIC CPU interface | Physical `0x50042000`, size `0x100` | Same evidence and consumer; compatible is exactly `arm,cortex-a15-gic` |
| Bus translation | LE u64 `/arm-io/ranges=(0,0x50041000,0x1100)`; `/arm-io/intc/reg=(0,0x1000,0x1000,0x100)` | The bus span covers exactly the adjacent GIC ranges; ArmCpu computes each physical address from the bus base plus relative offset |
| Virtual timer | Architectural interrupt ID 27 (PPI 11) | Live timer cells corroborate ID 27. ArmCpu selects it in source; no fake MMIO timer or Apple `interrupt-controller=master` is emitted |
| Installed DRAM | LE u64 `/chosen/dram-base=0x80000000`, `dram-size=0x100000000` | [Native memory inventory](../out/switch/native-memory-hardware-result.json): MC_EMEM_CFG reports 4096 MiB; two firmware banks plus excluded 3 MiB tail span the installed memory |
| Managed window request | LE u64 `/chosen/darwinboot-memory-size=0x30000000`; separate loader contract uses base `0xB2004000` and end `0xE2004000` | Candidate only. The native loader must establish runtime bounds and non-overlap before use |
| Entropy seed | 64 bytes from host Python `os.urandom` at build time | `PE_get_random_seed` requires a nonempty value. This is host entropy, not an OLED hardware RNG or attestation; repeated boots of one package reuse its seed |
| NVRAM | Empty volatile 8192-byte CHRP v1 bank, 2048-byte common partition, no system partition or variables | `ArmDTPlatform::hasNVRAMProxy` and `IODTNVRAM::initImageFormat` require a usable bootstrap image. No persistent controller or firmware NVRAM is claimed |

`arm_init` explicitly distinguishes `/chosen/dram-base/size` (actual DRAM) from `boot_args.physBase/memSize` (kernel-managed memory). `gDramBase/gDramSize` also classify physical addresses for DRAM versus MMIO memory attributes in `pmap::wimg_to_pte`; they do not expand the allocator window. The excluded tail, firmware allocations, framebuffer and other reservations remain outside the candidate managed window and remain the loader's validation responsibility.

The tree includes empty `/defaults` and `/options` for ArmDTPlatform. It includes an empty `/chosen/memory-map` because `arm_vm_init` asserts that the node exists before looking for an optional TrustCache property. It does not declare a RAMDisk, TrustCache, root volume, manifest identity, VMM, PL011, VirtIO, UFS, secondary CPU, PSCI startup node, or unmeasured CPU/bus/memory clock. The unknown clock rates are left absent; this is not a CPU frequency measurement.

The first-run display diagnostics use the loader's separate, validated `boot_args.Video` contract, not a claimed Tegra display driver in this tree. Hardware GIC nonsecure access and timer interrupt delivery remain unqualified. With no root storage or complete AMFI stack, later kernel initialization may fail; this ADT does not supply userspace or establish a native XNU or iPadOS boot.

Host checks: `python3 -m unittest tests.test_native_oled_adt tests.test_native_xnu_layout -v` passed all 21 tests. The focused checks independently decode the generated binary through the loader reader, compare GIC translations and identity with captured board data, check installed-versus-managed memory, reject unwanted device claims, and validate CHRP partition bounds/checksums and empty contents.
