# Full-RAM kernel virtual reservations (V23)

**Installed, verified and safely ejected.** [Installation receipt](../out/switch/native-xnu-v23-install.json), [package](../out/native-vmregions-v23-delta/manifest.json).

V22's photograph shows every stage through **21 VM RETIRING EARLY DIAGNOSTICS**, following **20 VM FINAL KERNEL TABLES ACTIVE**. XNU had entered and its early renderer worked. The absence of subsequent text does not mean XNU never entered or lacked a framebuffer. [Hardware record](../out/switch/native-xnu-v22-hardware-result.json).

The full-RAM boot shifts the conceptual `gVirtBase` below `LOW_GLOBAL_BASE_ADDRESS` while keeping the kernel's actual VA-to-PA relationship unchanged. The old `pmap_virtual_region()` assumed the opposite order. It returned a bootstrap reservation that now covered the low globals, then separately returned those same low globals and an overlapping lower reservation. `kmem_init()` inserts these as fixed permanent ranges and rejects the overlap.

V23 changes only that native-profile enumeration. It returns the exact union of the old lower, bootstrap and low-global reservations once, followed by the unchanged high CPU/I/O window. Enumeration ends after those two disjoint regions. The legacy path remains unchanged. This changes virtual reservations, not physical RAM capacity, RAM-bank coverage or reserved hardware pages.

The source defect matches the otherwise silent stage-21 screen: `PE_create_console()` prepares the text mode, but the screen is not enabled until later `vcattach()`. `kmem_init()` runs before that acquisition. A panic there can therefore leave the early loader/stage output visible. No panic message was captured on hardware; the identified failure is reproduced from the actual reservation function and its consumer's fixed-range contract. [Independent console-order trace](../out/research-native/native-full-memory-review/v22-post21-console-order.md).

Eighteen checks execute the actual preprocessed function and real build macros. The old code fails the fixed insertion at ordinal 2; the correction returns two disjoint ranges preserving the complete old union. Slide extrema, early I/O growth, map bounds, ordinal termination and unchanged legacy 4 KiB/16 KiB behavior pass under ASan/UBSan. Native compilation and exact patch replay pass. [Source/fixture proof](../out/research-native/v23-virtual-regions/result.json), [independent review](../out/research-native/native-full-memory-review/v23-virtual-reservation-independent-review.json).

The linked kernel's actual native gate and reservation instructions were checked independently. V22's early fault and panic diagnostics, 4 KiB pages, both RAM banks and required DMA remain. [Final linked review](../out/research-native/native-full-memory-review/v23-final-artifact-review.json), [package and bindings](../out/research-native/v23-artifact-review/result.json).

The correction still needs a hardware boot. It does not establish DMA throughput, upper-bank allocations, a complete userspace boot or the UI. Multicore support remains separate; [the other-port comparison](../out/research-native/sol-switch-smp-audit.md) identifies the relevant GICv2/PSCI implementation.
