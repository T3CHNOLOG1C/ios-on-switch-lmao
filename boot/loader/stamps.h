#ifndef SWITCH_LOADER_STAMPS_H
#define SWITCH_LOADER_STAMPS_H
/* Pre-kernel boot timing, handed to XNU in one record.
 *
 * The record's clock is the system counter (19.2 MHz) that CNTPCT_EL0 reads and XNU's mach_absolute_time is, so its
 * times line up with the unified log's kernel times. Hekate's BL31 enables that counter (SYSCTR0 CNTCR |= EN) when
 * hekate starts the CPU: 0 is about then, and hekate's own time (menu reload, boot logo, bootwait) is before 0.
 *
 * U-Boot runs from hekate's env and cannot read CNTPCT from its shell, so it samples the Tegra microsecond timer
 * (TIMERUS_CNTR_1US, free-running since SoC reset; U-Boot's own timer block) with setexpr.l: from its own default
 * environment (package_raw_boot.py bl33_patch: bootcmd's start, the SD card up in mmc_boot), once from the hekate
 * entry (scan_dev_for_extlinux: hekate's env imported) and at each step of boot.cmd. Just before bootm go it writes
 * the slot below: LE32 [0] IOSW_BOOT_STAMPS_UBOOT_MAGIC, then the samples (0 = not taken) in slot order:
 * ios_uboot, ios_script, ios_loader, ios_dtb, ios_window, ios_exporter, ios_prep, ios_go, ios_u_main, ios_u_sd
 * (loader.c stamps_uboot puts them in record order); the rest zero. A slot without the magic is ignored, never refused.
 *
 * The loader samples the microsecond timer and CNTPCT together, converts U-Boot's samples to counter values, adds its
 * own and writes the record into the ADT placeholder /chosen/switch-boot-stamps (LE64 words, IOSW_STAMP_* order);
 * XNU logs it once (startup.c switch_log_boot_stamps, which keeps its own copy of this layout). */
#define IOSW_BOOT_STAMPS_PA UINT64_C(0xa001ef00)
#define IOSW_BOOT_STAMPS_BYTES 0x100u
#define IOSW_BOOT_STAMPS_UBOOT_MAGIC 0x544f4255u /* "UBOT" */
#define IOSW_BOOT_STAMPS_UBOOT_COUNT 10u
#define IOSW_TIMERUS_PA UINT64_C(0x60005010) /* TIMERUS_CNTR_1US */

#define IOSW_STAMP_RECORD_MAGIC UINT64_C(0x33504d4154534f49) /* "IOSTAMP3" */
enum iosw_stamp {
    IOSW_STAMP_MAGIC = 0,
    IOSW_STAMP_WORDS,          /* IOSW_STAMP_COUNT */
    IOSW_STAMP_FREQUENCY,      /* CNTFRQ_EL0 */
    IOSW_STAMP_TIMERUS,        /* microsecond timer (32 bits) at ... */
    IOSW_STAMP_TIMERUS_AT,     /* ... this count */
    IOSW_STAMP_UBOOT_MAIN,     /* U-Boot: initialized, its bootcmd starts */
    IOSW_STAMP_UBOOT_SD,       /* the SD card initialized (mmc dev 1) */
    IOSW_STAMP_UBOOT_DISTRO,   /* U-Boot: hekate env imported, distro scan of /switchroot/ios/ starts */
    IOSW_STAMP_UBOOT_SCRIPT,   /* boot.scr's first command */
    IOSW_STAMP_UBOOT_LOADER,   /* loader.uboot.bin read */
    IOSW_STAMP_UBOOT_DTB,      /* oled-sku3.dtb read */
    IOSW_STAMP_UBOOT_WINDOW,   /* kernel-window.bin read */
    IOSW_STAMP_UBOOT_EXPORTER, /* SD exporter staged */
    IOSW_STAMP_UBOOT_PREP,     /* bootm prep done */
    IOSW_STAMP_UBOOT_GO,       /* SD metadata ready, bootm go next */
    IOSW_STAMP_LOADER_ENTRY,   /* loader's first instructions */
    IOSW_STAMP_EMC_START,      /* EMC raise (emc.h): first BPMP request ... */
    IOSW_STAMP_EMC_END,        /* ... and done, whatever the result (both 0 when switch-emc is off) */
    IOSW_STAMP_GATES,          /* FDT, stage, MC and timing-slot checks done */
    IOSW_STAMP_UNPACK_START,   /* cacheable regime on, first payload byte */
    IOSW_STAMP_UNPACK_END,     /* window decoded and zero-filled */
    IOSW_STAMP_COPY_DONE,      /* cleaned to PoC, caches and MMU off again */
    IOSW_STAMP_XNU_ENTRY,      /* branch to XNU */
    IOSW_STAMP_EMC_RESULT,     /* enum loader_emc_result | (uint32_t)BPMP return code << 32 */
    IOSW_STAMP_EMC_RATES,      /* EMC kHz before | kHz after << 32 */
    IOSW_STAMP_EMC_CORE,       /* VDD_CORE mV | BPMP channel 0 area << 32 */
    IOSW_STAMP_COUNT
};
#define IOSW_STAMP_RECORD_BYTES (IOSW_STAMP_COUNT * 8u)
#endif
