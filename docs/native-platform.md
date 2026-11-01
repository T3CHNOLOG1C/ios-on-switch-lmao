# OLED hardware facts for the native port

Collected read-only from the owner's running Switchroot Linux on 28 October 2026. The [captured properties](../out/switch/native-live-hardware.json) and [decoded summary](../out/switch/native-platform-summary.json) identify the physical board. They do **not** describe a complete free-memory map after U-Boot, establish interrupt security ownership, or prove native XNU execution.

| Observed property | Value |
| --- | --- |
| Model | Nintendo Switch (OLED model) |
| Compatible identities | `nvidia,fric`, `nintendo,aula`, `nintendo,nx`, `nvidia,tegra210b01` |
| First Linux memory bank | `0x80000000` through `0xFFD00000` exclusive |
| Second Linux memory bank | `0x100000000` through `0x180000000` exclusive |
| GIC distributor | `0x50041000`, size `0x1000` |
| GIC CPU interface | `0x50042000`, size `0x100` |
| Architectural timer | 19,200,000 Hz |
| Virtual timer interrupt | PPI 11, architectural interrupt ID 27 |
| PSCI conduit | `smc` |
| Ramoops reservation | `0xB0000000` through `0xB0200000` exclusive |

The GIC compatible string is `arm,cortex-a15-gic`. This describes the interrupt controller interface, not the CPU model. The timer node contains four PPI descriptors, preserved verbatim in the summary.

The capture has no property-read errors. Network MAC and serial identity properties are omitted. The temporary Linux sleep inhibitor was active at capture time.

## What cannot be inferred from this capture

Linux has already modified the device tree and taken ownership of the hardware. Its two RAM banks include regions that the next native loader must separately reserve. The framebuffer reservation nodes have empty `reg` properties in this live tree, while the boot arguments name a 4 MiB framebuffer at `0xF5A00000`; neither supplies a complete native display handoff contract. The device-tree reserve map, secure firmware carveouts, U-Boot relocation, and the probe's own live code/stack must also be accounted for.

The first [returning native probe](native-boot.md) reads CPU/exception-level/timer state within U-Boot. A subsequent kernel-handoff probe must inspect the actual OLED FDT and reservations after the firmware's kernel cleanup before the XNU allocator can use physical memory. Replacing QEMU MMIO addresses with the values above is insufficient by itself.
