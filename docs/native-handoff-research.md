# Native OLED handoff: memory ownership evidence

The v3 returning CPU probe visibly completed on the OLED in `out/switch/IMG_5506.png`. It establishes native A57 execution in U-Boot's EL2 environment. It does not establish the RAM that a kernel may overwrite, nor the state after `bootm` cleanup. The next useful step is a shell-only inventory of live RAM banks, framebuffer and existing memory-controller carveouts.

This investigation performed only host-side source reads, extraction and validation. It did not access the console, SD device or networked Switch, and did not change a boot entry. All new decoded artifacts are under `out/research-native/handoff-inventory/`.

## Exact installed board trees

`decode_installed_dtimg.py` validates the Android DT-table header and every entry's bounds, then selects **index 3**, exactly as the installed `boot.cmd` does with `dtimg load ... ${sku} ...`. The installed script checks `FRIG`, revision `0` for SKU 3; this is an index selection, not a lookup for numeric ID 3. The source command implementation is [Switchroot `cmd/dtimg.c` at `722e2b86`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/cmd/dtimg.c).

| Artifact | Observed identity |
| --- | --- |
| Installed `nx-plat.dtimg` | 668,726 bytes; SHA-256 `adfafc99ef03d690ac4ba3f852f53a8ba9274399df78be45ef59ee003c4ac40a` |
| Selected entry | Index 3, offset 501,732, length 166,994; `FRIG`, revision 0 |
| Extracted `oled-sku3.dtb` | SHA-256 `a806b66975040d7a8ba7c04c4f173a657b610a90868eab9d0efa806c8350ce63` |
| Model | `Nintendo Switch (OLED model)` |
| Compatible | `nvidia,fric`, `nintendo,aula`, `nintendo,nx`, `nvidia,tegra210b01` |
| BL33 control DTB | Offset `0x5B060`, 18,052 bytes in the installed BL33; SHA-256 `e55f7221445f05433b2971220e4a6d98ffa0fb25d09fa6e97d74bc7684c58103` |

The extracted OLED tree contains 1,042 nodes and 5,340 properties; the embedded control tree contains 86 nodes and 583 properties. Both were decompiled with the installed `dtc` and compiled back to DTB. Every property and every reservation-map entry matched the original decoded bytes. `decode-validation.json` records that check; `dtc` diagnostics are retained separately. This validates extraction, not runtime fixups.

The two trees serve different purposes and neither static `/memory` entry is a measurement of installed RAM:

| Location | Installed bytes | Consequence |
| --- | --- | --- |
| OLED kernel tree `/memory@80000000/reg` | Base `0x80000000`, size `0x80000000` (2 GiB) | A placeholder before U-Boot supplies live banks. |
| BL33 control tree `/memory/reg` | Base `0x80000000`, size `0x180000000` (6 GiB) | Also a static placeholder; do not use it as capacity or a free-memory range. |
| OLED FDT reservation map | `[0x80000000, 0x80020000)` | Preserve the 128 KiB reservation pending a demonstrated ownership contract. |
| OLED `/reserved-memory/ramoops_carveout` | `[0xB0000000, 0xB0200000)`, `no-map` | Preserve this 2 MiB persistent crash-log region. |
| OLED `fb0_carveout`, `fb1_carveout` | Both `surface` and `lut` ranges are zero | These do not reserve the currently scanned-out framebuffer. |
| OLED `/bpmp` | `carveout-start=0xFFE00000`, size `0x100000` | The board contract explicitly locates 1 MiB of BPMP firmware state here. |
| BL33 control `/framebuffer` | Address `0xF5A00000`, size `0x00384000`, 720×1280, `a8b8g8r8`, rotation 3 | The pixel buffer spans `[0xF5A00000, 0xF5D84000)`; live values still need capture. |
| BL33 control reservation map | Empty | Absence of a reservation entry does not mean the framebuffer or firmware RAM is free. |

The installed Linux script additionally advertises `tegra_fbmem=0x400000@0xf5a00000`, a larger 4 MiB region ending at `0xF5E00000`. Preserve that full range conservatively until the native display contract is established. The U-Boot [simple framebuffer driver](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/drivers/video/simplefb.c) reads the control-tree `address` and `size` and explicitly leaves reservation as a TODO. Successful screen output proves use of this buffer, not an allocator exclusion.

The kernel tree is later changed by [ARM `arch_fixup_fdt`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/arch/arm/lib/bootm-fdt.c), which replaces its memory banks with `gd->bd->bi_dram[]`. Therefore a future kernel-handoff probe must capture its actual `x0` FDT after preparation, rather than reusing this unmodified file.

## Bounded MC register inventory

The exact Switch defconfig enables `CONFIG_TEGRA210_CARVEOUT_EXACT_SIZE`. The matching U-Boot [memory discovery code](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/arch/arm/mach-tegra/board2.c#L253) already reads each register below during startup. Their offsets come from [`arch-tegra210/mc.h`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/arch/arm/include/asm/arch-tegra210/mc.h); [`tegra.h`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/arch/arm/include/asm/arch-tegra210/tegra.h) defines the base as `0x70019000`. `mc-register-offsets.json` independently computes the offsets from the sequential C structure fields and array lengths, rather than relying only on comments.

These are configuration-register reads with matching startup `readl()` provenance. This is evidence for repeating these specific reads, not permission to sweep arbitrary MMIO or a guarantee that all related registers are harmless.

| Command | Returned words in order |
| --- | --- |
| `md.l 0x70019050 1` | `MC_EMEM_CFG` |
| `md.l 0x70019670 2` | SEC base low, size in MiB |
| `md.l 0x700199D4 1` | SEC base high |
| `md.l 0x700199A0 3` | MTS base low, size in MiB, base high |
| `md.l 0x70019648 2` | VPR base low, size in MiB |
| `md.l 0x70019978 1` | VPR base high |
| `md.l 0x70019C0C 3` | GSC1 base low, base high, size in 128 KiB units |
| `md.l 0x70019C5C 3` | GSC2 base low, base high, size in 128 KiB units |
| `md.l 0x70019CAC 3` | GSC3 base low, base high, size in 128 KiB units |
| `md.l 0x70019CFC 3` | GSC4 base low, base high, size in 128 KiB units |
| `md.l 0x70019D4C 3` | GSC5 base low, base high, size in 128 KiB units |

This is **25 register words in 11 commands**, with no intervening lock, interrupt-status, client-access or reserved register reads. Eleven short labels and eleven data rows fit within 25 output lines. Every count is explicit and uses 32-bit reads. Decode `base = (high << 32) | low`; multiply SEC/MTS/VPR sizes by `0x100000`, GSC sizes by `0x20000`. A zero size disables that range, regardless of a stale base.

`MC_EMEM_CFG` is also read by [`query_sdram_size`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/arch/arm/mach-tegra/board.c#L71). Normally its Tegra210 value is capacity in MiB; if bit 31 is set, this fork masks to 14 bits and subtracts the 2 GiB physical base before deriving capacity. Preserve the raw word and apply the same handling rather than assuming the expected `0x1000` value.

U-Boot builds each bank by truncating it at the lowest active carveout base. It does not enumerate exact reclaimable holes between carveouts. The MC readings can explain the truncated bank and preserve each range explicitly; they must not be used to reclaim gaps without checking firmware ownership. Live `bdinfo` and these registers should agree, or native allocation must remain blocked.

## Firmware and U-Boot ownership still missing

| Owner | Source-backed facts | Remaining evidence |
| --- | --- | --- |
| BL31 / secure monitor | The pinned Hekate source selects a 1 MiB TZDRAM region at `0xFFF00000` in its non-NVDEC configuration, or 8 MiB at `0xFF800000` in the alternative configuration. It passes that base and size to BL31. | The installed BL31 is only 45,655 bytes, but its reservation is not its file length. Capture actual SEC registers after BL31 initialization; do not read or overwrite the secure payload. |
| BPMP / SC7 / memory training | OLED `/bpmp` specifies `0xFFE00000`+1 MiB. Hekate's B01 path loads firmware there, configures its heap/training state and starts it at base+`0x40`. | Match live GSC1 to this contract and retain the whole range while BPMP runs. A CPU kernel handoff does not stop the BPMP. |
| GPU firmware / protected memory | Hekate's non-NVDEC path places a 256 KiB GSC2 region and adjacent 768 KiB GSC3 region below the BPMP area; other builds alter placement and enable GSC4/5. | Capture GSC1–5, VPR and SEC. Do not assume unused graphics features make these ranges free. |
| Relocated U-Boot image and BSS | `bdinfo` prints `relocaddr` and relocation offset. `reserve_uboot()` reserves `gd->mon_len`, including BSS, rounded to 4 KiB. | A BL33 file length does not bound the runtime image. Capture relocation and eventually exact image/BSS end from a matched build or narrowly instrumented loader. |
| U-Boot heap | The matching default configuration provides a 4 MiB malloc pool, possibly plus environment space through `TOTAL_MALLOC_LEN`. `initr_malloc()` places it directly below `relocaddr`. | `bdinfo`'s **Early malloc usage** describes the pre-relocation allocator, not this heap. Require exact total/start/end before reusing any nearby memory. |
| Noncached DMA allocator | `CONFIG_SYS_NONCACHED_MEMORY` is 1 MiB, but ARM64 rounds to a 2 MiB MMU section. `end=ALIGN(mem_malloc_start,2 MiB)-2 MiB`; `start=end-2 MiB`. | This separate pool lies below malloc and is not printed by `bdinfo`. `sp start` alone is not a complete lower boundary for U-Boot-owned memory. |
| Stack, global data, board data and live tree | Allocation descends below malloc through board data, global data, relocated FDT and stack. `bdinfo` prints `irq_sp`, `sp start`, `fdt_blob` and TLB base. | Capture those pointers now. FDT header totalsize adds its logical extent; exact allocated padding, live-tree heap nodes and worst-case stack extent still require ownership accounting. |
| Translation tables | `reserve_mmu()` allocates `PGTABLE_SIZE`, rounds the base to 64 KiB and prints it via `bdinfo`. On ARM64 this size is computed, not a universal constant. | Preserve the tables while MMU is on. The future kernel must own a separate, validated mapping before reclaiming them. |
| Framebuffer | Live simplefb is separate from relocated U-Boot and can remain scanned out by hardware. | Capture address, size, dimensions, format and rotation, then preserve its full region through handoff and framebuffer use. |

Firmware source: [pinned Hekate L4T loader](https://github.com/CTCaer/hekate/blob/45cd81ed08e30a26ba380958c7e20b55c342834b/bootloader/l4t/l4t.c). Runtime allocation sources: [`common/board_f.c`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/common/board_f.c), [`common/board_r.c`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/common/board_r.c), [`arch/arm/lib/cache.c`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/arch/arm/lib/cache.c), and [`cmd/bdinfo.c`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/cmd/bdinfo.c). The fork revision is the existing binary's source reference; it has not been rebuilt to reproduce the firmware hash. Live measurements take precedence over inferred build constants.

U-Boot's [`arch_lmb_reserve`](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/arch/arm/lib/bootm.c#L48) reserves from its current SP minus 4 KiB through the end of bank 0 for boot allocations. That protects a particular boot operation; it is not an exported native-kernel memory-ownership manifest. Separately, the existing [Tegra EL2 map](https://gitlab.com/switchroot/bootstack/switch-uboot/-/blob/722e2b86be9ab1ae073335585c5997093e05f4f2/arch/arm/mach-tegra/arm64-mmu.c) maps normal RAM only from `0x80000000` to `0x100000000`. A bank above 4 GiB may be physically present without being accessible through the current U-Boot virtual map. Do not add arbitrary shell reads above 4 GiB.

## What to collect next

The proposed shell inventory should include `bdinfo`, the live control FDT header and reservation map, `/memory`, and all `/framebuffer` properties. Beyond those, the **11 bounded MC reads above are the highest-value addition**: they record actual capacity and carveout bases/sizes after secure firmware has run. Optional short environment output can record `t210b01`, `sku`, `loader_rev`, `display_id`, `scriptaddr`, `fdtcontroladdr`, and any explicit `bootm_low`, `bootm_size` or `bootm_mapsize` overrides; an unset variable is not an error.

The next native handoff must then retain an explicit exclusion list containing firmware, framebuffer, crash-log memory, U-Boot runtime regions, staging buffers and the final FDT. It must record entry EL, MMU/cache state and the final FDT after boot preparation. A returning shell inventory can close the hardware-map uncertainty without transferring ownership; a later nonreturning kernel probe is a separate milestone.
