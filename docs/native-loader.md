# Native loader boundary for the OLED

Source review, 28 October 2026. **The layout packer and entry adapter are now implemented and tested locally; v1 stopped after the loader verified the copy, and the user subsequently reported native XNU VM initialization and BSD entry with v2.** See the [current run guide](native-xnu-run.md), [host packer](../scripts/native_xnu_layout.py), [entry adapter](../native/loader/loader.c) and [native Apple DeviceTree](native-oled-adt.md). The contract analysis below documents the source basis for that implementation.

The `bootm` diagnostic established physical EL2-to-EL1 execution, post-cleanup framebuffer output and counter progress; its HCD validator mistake is corrected in source. [Hardware result](../out/switch/native-handoff-v2-hardware-result.json). The existing `BOOTAA64.EFI` is not runnable by the installed U-Boot. The candidate kernel window now has runtime exclusion checks, but no firmware-certified ownership proof; the small probe's footprint alone is not that proof.

The reviewed source baseline is UniXNU `1dba43325b8242c3c55001999cda841c25e9ca68`, the patched A57 development tree recorded in [isolation.json][isolation], and Switchroot U-Boot `722e2b86be9ab1ae073335585c5997093e05f4f2`. The U-Boot source is an informed match for the pinned installed BL33; its binary has not been reproduced from that source. The A57 development model's addresses and timer frequency are not OLED handoff facts.

The v3 userspace candidate adds a real RAM-disk/test-process and trust-cache pair, plus the original AMFI closure. Its physical base is `0xB207C000` to match the prelinked minimum VA `0xFFFFFFF00427C000`; the 768 MiB allocation still fits the same reviewed rounded envelope `[0xB2000000, 0xE2200000)`. The Mac model reaches the own signed EL0 fixture; the user has since reported the exact successful EL0 banner on the OLED. [Physical userspace result](../out/switch/native-xnu-v3-hardware-result.json). [Paired model review](../out/native-a57/userspace-v1/model-pair-review.json).

## The contracts do not match yet

| Boundary | Actual source contract | Native consequence |
| --- | --- | --- |
| UniXNU frontend | Locates `gUniXNULoaderProtocolGuid`, builds a request containing an EFI volume/path, and calls its `Boot` callback. Missing protocol refuses the boot. [Boot.c:58–93][frontend] | The frontend alone is not a Mach-O loader. Installing EFI execution support would still leave firmware protocols and board support to supply. |
| DarwinBoot | Requires EL1 (`CurrentEL == 4`), rejects externally supplied FDTs and staged kexts, reads kernel, `kernel.adt`, optional `kernel.trustcache`, allocates a real conventional-memory window, then exits UEFI boot services. [DarwinBoot.c:490–642][darwinboot] | Reuse its validated formats and layout calculations, not the EFI executable as an existing Switch loader. |
| U-Boot ARM64 OS handoff | Quiesces devices, disables interrupts/caches/MMU, calls its EL helper with `x0 = FDT PA`, `x1–x3 = 0`, and enters the image's entry address. The normal Switch configuration does not define `CONFIG_ARMV8_SWITCH_TO_EL1`; its helper retains EL2 when already at EL2. [bootm.c:326–349][bootm], [cpu.c:36–60][cleanup], [transition.S:14–32][transition] | An EL2-to-EL1 adapter is expected, but the physical post-cleanup EL and register state must be measured. The FDT pointer is not an XNU `boot_args` pointer. |
| XNU cold entry | `_start` branches to `start_first_cpu`; `x0` is a physical `boot_args` address. Code immediately writes EL1 registers, reads the boot memory fields, installs its own exception/interrupt stacks and bootstrap page tables, enables EL1 translation, and converts the argument pointer to kernel VA. [patched start.s:225–227,465–520,648–777,816–834][xnu-start] | Enter the audited non-Apple A57/4 KiB kernel at its physical entry with the complete Darwin contract. The source has no general EL2 entry or U-Boot FDT adapter. |

The installed defconfig explicitly disables `EFI_LOADER` and `BOOTI` while enabling `BOOTM`. [nintendo-switch_defconfig:21–40][defconfig] A U-Boot kernel-image wrapper can select its existing ARM64 OS handoff without containing Linux; it does not translate a Mach-O file, synthesize Darwin arguments, or create native drivers.

## Layout and entry that must be reproduced

DarwinBoot accepts a little-endian 64-bit ARM64 `MH_EXECUTE`, CPU subtype 0, bounded nonoverlapping `LC_SEGMENT_64` commands, and one ARM64 `LC_UNIXTHREAD` entry inside executable file-backed bytes. It accepts a kernel file up to 128 MiB, limits the virtual segment span to 256 MiB minus 2 MiB, and zeroes the destination before copying each file-backed segment. It does **not** implement an arbitrary Mach-O relocation/linker pass. [DarwinBoot.c:113–180,519–575][parser]

For virtual base `V`, selected physical base `P`, virtual entry `E`, and proved owned size `N`, the current rules are:

- Copy each segment to `P + segment.vmaddr - V`; entry PA is `P + E - V`. Preserve linked virtual addresses initially: physical placement is not permission to introduce a virtual slide.
- Require `P mod 2 MiB == V mod 2 MiB`. The allocator enforces this congruence. XNU's early block mappings mask physical addresses down to block boundaries, so also prove the safety of the block-rounded extent around the window; claiming ownership only from an unaligned `P` is insufficient. [DarwinBoot.c:421–424][allocate], [patched start.s:391–405][xnu-block]
- Place arguments after the kernel span at 16 KiB alignment, tree one 4 KiB page later, optional ramdisk at the next 4 KiB boundary, then trust cache. Round the used end to 16 KiB and retain at least 2 MiB unused within the owned window. `topOfKernelData` is that used physical end. [DarwinBoot.c:564–599][layout]
- Set the little-endian 0x480-byte arguments to Revision/Version 2/2, `virtBase=V`, `physBase=P`, `memSize=memSizeActual=N`, and `deviceTreeP=V + treePA - P`. The incoming `x0` remains the **physical** arguments pointer. The tree field is its **kernel virtual** pointer. Video is six 64-bit fields; command line is 1024 bytes. [BootArgs.h:7–31][args]

`N` is the memory XNU may manage, not total installed DRAM. The parser permits 256 MiB–8 GiB in 2 MiB steps, but the UEFI implementation separately demands one actual free conventional-memory run. The native adapter needs an equivalent ownership proof. XNU later sets its allocation interval from `topOfKernelData` through `physBase + memSize`; a carveout inside that interval cannot be made safe merely by mentioning it in an unrelated FDT. [WindowSize.h:4–7][window], [arm_vm_init.c:1833–1854,1947–1970][vm]

DarwinBoot's final assembly masks DAIF, cleans/invalidates the owned data range, invalidates instructions with barriers, disables EL1 M/C/I, invalidates EL1 TLBs, and branches to entry. It is explicitly EL1-only. [Entry.S:8–52][enter] It neither exports an EFI runtime-services table in `boot_args` nor calls `SetVirtualAddressMap`; after the branch the reviewed path supplies no firmware filesystem, allocator or display protocol. Native storage, interrupts, power and persistent state remain kernel/platform work.

XNU creates its own initial stacks and translation tables; the adapter still needs a private stack/vector area for any validation or EL transition before entry. Its linked image contains those kernel stack/table objects, so BSS/zero-fill and segment placement are essential. The patched build also bounds bootstrap table allocation and disables the legacy BCM timer-routing MMIO when `XNU_NATIVE_ARCH_TIMER=1`. [bootstrap patch][bootstrap-patch], [timer patch][timer-patch] Do not use the unpatched reference `start.s` as the executable build contract.

## ADT and platform inputs

The Apple tree is a different serialization from the inherited FDT: little-endian property/child counts, 32-byte property names, 32-bit lengths and four-byte padded values. The loader validates `/chosen/memory-map` placeholders, fills optional RAMDisk/PackedRAMDisk and TrustCache properties with physical address/size pairs, and rejects inconsistent sidecars. [DarwinBoot.c:185–260,535–559,589–599][adt] XNU consumes the tree through `SecureDTInit`, then uses it to identify clocks and platform devices. [pe_init.c:420–448][pe-init]

A first native ADT needs the real CPU affinity and timebase, the selected native platform-driver match, nonsecure GICv2 resources/timer interrupt, and a bounded memory/diagnostics description. Preserve an explicit `/chosen` and only the services the selected kernel actually uses. The existing QEMU generator deliberately inserts `linux,dummy-virt`, GICv3, PL011/VirtIO addresses, optional `vmm-present`, and a 62.5 MHz model timer; its serialization helpers are reusable, its board values are not. [make-dt.py:71–84,140–178][model-adt] Firmware random-seed sourcing and any later owner policy/trust-cache identity must be specified before the corresponding kernel features are enabled; synthetic fixture data is not hardware evidence.

## Smallest concrete implementation and physical gates

1. **First capture the OS handoff without XNU.** A separately reviewed, nonreturning `bootm` image should record post-cleanup `CurrentEL`, DAIF, SCTLR/TCR/TTBR/VBAR, HCR/CNTHCTL/CNTVOFF where accessible, counter frequency and incoming FDT. It needs diagnostics that remain valid after U-Boot has removed devices. It cannot return through the current returning-probe ABI after cleanup.
2. **Build a host-side fixed-layout packer and a small freestanding entry stub.** Port DarwinBoot's bounded segment/ADT/layout checks; initially preserve linked VA and use one explicitly verified physical window. Prelayout kernel zero-fill, arguments and ADT into an uncompressed U-Boot image so the first device implementation needs no EFI protocols, filesystem reader, decompressor or dynamic linker. Its manifest must name and bound source staging, destination, FDT, stub, stack, arguments, tree and diagnostics ranges and refuse every overlap with preserved firmware/carveouts.
3. **Validate the adapter with a tiny contract-checking payload before XNU.** First prove the exact arguments, zero-fill, placement, cache state and deliberate EL1 transition. At EL2, establish appropriate AArch64 EL1 execution, trap/timer access and stage-2-disabled state using measured inputs and a reviewed transition; do not assume changing SCTLR_EL1 alone leaves EL2. U-Boot's source EL1 transition is a reference for timer access, traps and `eret`, not proof those settings match the installed secure firmware. [macro.h:238–297][el1]
4. **Only then select the A57 kernel checkpoint.** Start with one CPU and a bounded early XNU/diagnostic target. The earlier source-only model reached an AMFI-provider fault before EL0, so native loader success would still not establish a userspace boot. [native-a57.md][a57]

| Required evidence | What the current register probe provides | Still needed before choosing a native kernel window/handoff |
| --- | --- | --- |
| CPU and privilege | MIDR, MPIDR, architectural features, current EL, DAIF and selected EL1/EL2 state | Actual post-`bootm` state, EL2 controls omitted from the current record, accessibility of required EL1 operations |
| RAM ownership | No memory-map or carveout inventory | Final selected FDT and reservations; Hekate/BL31 secure and BPMP ranges; U-Boot relocation/stack/allocator extent; framebuffer and DMA ownership; explicit contiguous free window including block-rounded boundaries |
| Timer/interrupts | `CNTFRQ_EL0`, HCR/CNTHCTL when at EL2 | Post-transition timer access/offset, accessible nonsecure GIC groups, PPI route, interrupt-controller state; Linux's live tree is corroboration, not handoff proof |
| Visible diagnostics | Existing U-Boot screen only | Framebuffer PA, span, stride, format/rotation and persistence after cleanup, or a separately verified native serial path |
| Runtime services | None | Native drivers or explicitly deferred functionality; U-Boot services do not remain callable after OS cleanup |

The original review did not create a loader. The subsequent implementation now supplies a Mach-O/boot-arguments packer, entry adapter and runtime reservation gate. Local models reach XNU and readable panic output. The user now reports XNU VM initialization and BSD entry on the physical OLED; remaining platform dependencies are unverified. Historical checklist items should be read alongside the [current run guide](native-xnu-run.md).

[isolation]: /Users/brunostjohn/Development/Projects/ioswitch/out/native-a57/isolation.json
[frontend]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/package/UniXNUPkg/Library/UniXNULib/Boot.c:58
[darwinboot]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c:490
[bootm]: /Users/brunostjohn/Development/Projects/ioswitch/out/research-native/switch-uboot/arch/arm/lib/bootm.c:326
[cleanup]: /Users/brunostjohn/Development/Projects/ioswitch/out/research-native/switch-uboot/arch/arm/cpu/armv8/cpu.c:36
[transition]: /Users/brunostjohn/Development/Projects/ioswitch/out/research-native/switch-uboot/arch/arm/cpu/armv8/transition.S:14
[defconfig]: /Users/brunostjohn/Development/Projects/ioswitch/out/research-native/switch-uboot/configs/nintendo-switch_defconfig:21
[xnu-start]: /Users/brunostjohn/Development/Projects/ioswitch/out/native-a57/tree/.cache/xnu-work/osfmk/arm64/start.s:465
[xnu-block]: /Users/brunostjohn/Development/Projects/ioswitch/out/native-a57/tree/.cache/xnu-work/osfmk/arm64/start.s:391
[parser]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c:113
[allocate]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c:421
[layout]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c:564
[args]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/drivers/DarwinBoot/DarwinBootPkg/Driver/BootArgs.h:7
[window]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/drivers/DarwinBoot/DarwinBootPkg/Driver/WindowSize.h:4
[vm]: /Users/brunostjohn/Development/Projects/ioswitch/out/native-a57/tree/.cache/xnu-work/osfmk/arm64/arm_vm_init.c:1833
[enter]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/drivers/DarwinBoot/DarwinBootPkg/Driver/Entry.S:8
[bootstrap-patch]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/patches/xnu/native-4k-bootstrap-capacity.patch:15
[timer-patch]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/patches/xnu/architectural-timer-entry.patch:3
[adt]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c:185
[pe-init]: /Users/brunostjohn/Development/Projects/ioswitch/out/native-a57/tree/.cache/xnu-work/pexpert/arm/pe_init.c:420
[model-adt]: /Users/brunostjohn/Development/Projects/ioswitch/vendor/unixnu/scripts/qemu-entry/make-dt.py:71
[el1]: /Users/brunostjohn/Development/Projects/ioswitch/out/research-native/switch-uboot/arch/arm/include/asm/macro.h:238
[a57]: /Users/brunostjohn/Development/Projects/ioswitch/docs/native-a57.md
