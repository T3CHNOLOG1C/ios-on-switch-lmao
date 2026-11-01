# Physical OLED memory inventory

Observed 28 October 2026 in the user's [U-Boot page](../out/switch/IMG_5508.png), [display/control-tree page](../out/switch/IMG_5509.png) and [carveout page](../out/switch/IMG_5510.png). The [complete result](../out/switch/native-memory-hardware-result.json) retains every photographed data field, all 25 MC register words, original/converted photograph hashes and source references. All address/register values were independently cross-checked. The early-malloc used count has an unresolved `138`/`13B` glyph; it is not used for any bound.

All ranges below are half-open: the end address is excluded.

| Reported region | Start | End | Size |
| --- | --- | --- | --- |
| U-Boot DRAM bank 0 | `0x80000000` | `0xffd00000` | 2,045 MiB |
| U-Boot DRAM bank 1 | `0x100000000` | `0x180000000` | 2,048 MiB |
| GSC2 | `0xffd00000` | `0xffd40000` | 256 KiB |
| GSC3 | `0xffd40000` | `0xffe00000` | 768 KiB |
| GSC1 | `0xffe00000` | `0xfff00000` | 1 MiB |
| Firmware framebuffer | `0xf5a00000` | `0xf5d84000` | `0x384000` bytes |

Preserve the entire excluded tail **`[0xffd00000, 0x100000000)`**, including the final 1 MiB beyond the sampled nonzero GSC ranges. The pinned firmware excludes everything from the lowest active carveout base through the bank end. SEC, VPR, MTS and GSC4/5 report zero size; that does not make other firmware allocations reclaimable. `MC_EMEM_CFG = 0x1000` reports 4,096 MiB of installed DRAM; the two reported banks sum to 4,093 MiB.

The live firmware pointers are TLB `0xffcf0000`, relocated U-Boot `0xffc87000`, stack `0xff87f710` and control FDT `0xff87f728`. Their allocation sizes were not printed. The framebuffer is 720 × 1280, `a8b8g8r8`, rotation 3. Its displayed `size = "", "8@"` is U-Boot's printable-string heuristic: those two strings reconstruct bytes `00 38 40 00`, hence big-endian size `0x384000`. The decoder follows [the exact formatter](../out/research-native/switch-uboot/cmd/fdt.c), rather than assuming a size from resolution.

The control FDT's memory cells describe a static 6 GiB range starting at `0x80000000`; that conflicts with the runtime banks. Its empty reservation table likewise does not erase the photographed framebuffer, firmware allocations or carveouts. Neither is a complete free-memory map.

For the next tiny diagnostic, a conservative candidate is **`[0xa0000000, 0xa0020000)`** for code, record and private stacks, with a separate bounded input-image area starting at `0xa0100000`. Cap each area at 128 KiB, including any container header in the staging area. The previous register probe used only 768 bytes at `0xa0000000`; it did not prove the whole proposed area free. Check every image/FDT/LMB destination for overlap, and preserve **`[0xf0000000, 0x100000000)`** as an intentionally broad firmware/display exclusion for this test. Do not clear or memory-test an entire bank.

This is sufficient provenance for reviewing a small bounded experiment. It does not establish a full XNU allocation window, high-bank suitability, post-cleanup display persistence or a native kernel boot. No SD or device action was performed during this analysis.
