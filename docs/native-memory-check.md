# OLED memory and display inventory

CPU probe v3 passed on the actual console. This next diagnostic collects the RAM-bank, framebuffer and memory-reservation inputs needed to place a native loader. It stays inside the existing U-Boot session and does not start XNU or perform an OS handoff.

## Status

**Installed, verified and safely ejected.** Five files totaling 444,165 bytes matched the reviewed package on SD readback; all 36 inspected existing configuration/reference/probe files retained their hashes. [Installation and safe-ejection receipt](../out/switch/native-memory-check-install.json). Its separate paths are `switchroot/ioswitch-memory/` and `bootloader/ini/ioswitch-memory.ini`. No automatic reboot was performed; the three-page hardware result is pending.

## Run and capture

1. After the USB disk has been safely ejected on the Mac, close USB storage on the Switch.
2. In Hekate, select **More Configs → IOSwitch Memory Check**.
3. Photograph each of the **three numbered pages**. Each stays for **20 seconds**, and the sequence repeats continuously, so there is no need to rush. Capture all text, including the addresses on page 3. If it stops on an error, photograph that instead.
4. To exit, hold **POWER for at least 20 seconds**, then use the usual Hekate startup method. Hold VOL− during Hekate startup to cancel autoboot if needed.

The pages show:

| Page | Information |
| --- | --- |
| 1 | U-Boot's runtime DRAM banks, translation-table address, relocation, stack and control-FDT address |
| 2 | Firmware framebuffer address/size/format/rotation, control-tree memory property and reservation map |
| 3 | Raw DRAM configuration plus SEC, MTS, VPR and five general-security carveout base/size fields |

The script reads exactly 25 32-bit memory-controller words already read by this firmware's DRAM-sizing code. It includes no storage writes, payload launch, reset, or saved environment changes. It selects screen output and a working-FDT pointer only for this boot session. It does not reconfigure RAM, clocks, power, interrupt controllers, or the display controller.

## Interpretation limits

The embedded control tree's `/memory` property is a **6 GiB template**, not a measured capacity. The installed Linux DTB also contains placeholders before boot fixups. Use the photographed runtime banks and controller fields together with explicit firmware/framebuffer exclusions before selecting a kernel window.

The framebuffer `size` may print as two strings, `"", "8@"`, because this old FDT printer interprets bytes `00 38 40 00` as strings. Those exact bytes represent `0x00384000`; retain the full output rather than assuming a missing value.

This capture precedes firmware cleanup. It does not establish that the display survives a `bootm` handoff, that all listed RAM is currently free, or that XNU can run. U-Boot's DMA pool, allocator, relocation, secure firmware, BPMP, framebuffer and selected kernel-tree reservations still need explicit accounting. The next kernel-handoff probe must use a diagnostic path valid after cleanup.

## Evidence

- [Successful CPU probe and complete decode](../out/switch/native-probe-v3-hardware-result.json)
- [New package manifest](../out/native-memory-check-final/manifest.json)
- [Independent source/package review](../out/research-native/memory-check-review.json)
- [Five command-stub control-flow cases](../out/research-native/memory-check-model.json); these are a host-side model, not U-Boot or physical execution
- [Native loader boundary](native-loader.md)
- [Exact installed device-tree extraction and memory-ownership analysis](native-handoff-research.md)

To remove this diagnostic later, delete only `bootloader/ini/ioswitch-memory.ini` and `switchroot/ioswitch-memory/`. Existing Hekate entries need no edits.
