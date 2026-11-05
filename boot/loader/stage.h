#ifndef SWITCH_LOADER_STAGE_H
#define SWITCH_LOADER_STAGE_H
/* The kernel window rides in bl33.bin, so hekate reads it (its SD at SDR104) together with U-Boot, instead of U-Boot
 * reading a separate file at 48 MHz. hekate's L4T launcher loads the whole of bl33.bin at 0xAA000000 (l4t.c
 * _l4t_sd_load, no size limit) and jumps there; U-Boot only ever runs from the first 1 MiB of it (its image, DTB and
 * pre-relocation BSS end below 0xAA068200; it relocates itself to the top of RAM, its stack starts at 0x8A800000).
 *
 *   0xAA000000  switchroot's bl33.bin as shipped (the boot_targets default excepted, package_raw_boot.py)
 *   0xAA100000  header page: LE32 [0] MAGIC [1] HEAD [2] VERSION [3] window offset from the header (0x1000)
 *               [4] stream bytes [5] staged bytes (a 4 KiB multiple) [6..7] stream id, the rest zero
 *   0xAA101000  the window's LZ4 blocks back to back (build_native_loader.py compress_window), zero-padded
 *   + staged    trailer page: LE32 [0] MAGIC [1] TAIL [2] stream bytes [3] staged bytes [4..5] stream id, rest zero
 *
 * The stream id is the first 8 bytes of the staged bytes' SHA-256, pinned in boot.cmd and in the loader: header and
 * trailer prove this build's window is in RAM, whole, at the address the loader decodes from. The region ends before
 * hekate's ramoops at 0xB0000000 and is one reserved range (build_native_loader.py 'bl33-and-kernel-window'). */
#define IOSW_STAGE_HEADER_PA UINT64_C(0xaa100000)
#define IOSW_STAGE_PA UINT64_C(0xaa101000)
#define IOSW_STAGE_END UINT64_C(0xb0000000)
#define IOSW_STAGE_MAGIC 0x57534f49u   /* "IOSW" */
#define IOSW_STAGE_HEAD 0x4e49574bu    /* "KWIN" */
#define IOSW_STAGE_TAIL 0x444e454bu    /* "KEND" */
#define IOSW_STAGE_VERSION 1u
#endif
