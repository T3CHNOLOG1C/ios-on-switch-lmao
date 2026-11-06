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
 * environment (bl33.py: bootcmd's start, the SD card up in mmc_boot), once from the hekate entry
 * (scan_dev_for_extlinux: hekate's env imported) and at each step of the boot script. Just before bootm go it writes
 * the slot below: LE32 [0] SWITCH_BOOT_STAMPS_UBOOT_MAGIC, then the samples (0 = not taken) in slot order:
 * ios_uboot, ios_script, ios_args, ios_dtb, ios_window, ios_exporter, ios_prep, ios_go, ios_u_main, ios_u_sd
 * (loader.c stamps_uboot puts them in record order); the rest zero. A slot without the magic is ignored, never refused.
 *
 * The loader samples the microsecond timer and CNTPCT together, converts U-Boot's samples to counter values, adds its
 * own and writes the record into the ADT placeholder /chosen/switch-boot-stamps (LE64 words, SWITCH_STAMP_* order);
 * XNU logs it once (startup.c switch_log_boot_stamps, which keeps its own copy of this layout). */
#define SWITCH_BOOT_STAMPS_PA          UINT64_C(0xa001ef00)
#define SWITCH_BOOT_STAMPS_BYTES       0x100u
#define SWITCH_BOOT_STAMPS_UBOOT_MAGIC 0x544f4255u /* "UBOT" */
#define SWITCH_BOOT_STAMPS_UBOOT_COUNT 10u
#define SWITCH_TIMERUS_PA              UINT64_C(0x60005010) /* TIMERUS_CNTR_1US */

#define SWITCH_STAMP_RECORD_MAGIC UINT64_C(0x33504d4154534f49) /* "IOSTAMP3" */

enum switch_stamp {
    SWITCH_STAMP_MAGIC = 0,
    SWITCH_STAMP_WORDS,          /* SWITCH_STAMP_COUNT */
    SWITCH_STAMP_FREQUENCY,      /* CNTFRQ_EL0 */
    SWITCH_STAMP_TIMERUS,        /* microsecond timer (32 bits) at ... */
    SWITCH_STAMP_TIMERUS_AT,     /* ... this count */
    SWITCH_STAMP_UBOOT_MAIN,     /* U-Boot: initialized, its bootcmd starts */
    SWITCH_STAMP_UBOOT_SD,       /* the SD card initialized (mmc dev 1) */
    SWITCH_STAMP_UBOOT_DISTRO,   /* U-Boot: hekate env imported, distro scan of /switchroot/ios/ starts */
    SWITCH_STAMP_UBOOT_SCRIPT,   /* the boot script's first command */
    SWITCH_STAMP_UBOOT_ARGS,     /* bootargs.txt read (or found missing) */
    SWITCH_STAMP_UBOOT_DTB,      /* oled-sku3.dtb in place */
    SWITCH_STAMP_UBOOT_WINDOW,   /* staged window checked */
    SWITCH_STAMP_UBOOT_EXPORTER, /* SD exporter staged */
    SWITCH_STAMP_UBOOT_PREP,     /* bootm prep done */
    SWITCH_STAMP_UBOOT_GO,       /* SD metadata ready, bootm go next */
    SWITCH_STAMP_LOADER_ENTRY,   /* loader's first instructions */
    SWITCH_STAMP_EMC_START,      /* EMC raise (emc.h): first BPMP request ... */
    SWITCH_STAMP_EMC_END,        /* ... and done, whatever the result (both 0 without LOAD_EMC_KHZ) */
    SWITCH_STAMP_GATES,          /* FDT, stage, MC and timing-slot checks done */
    SWITCH_STAMP_UNPACK_START,   /* cacheable regime on, first payload byte */
    SWITCH_STAMP_UNPACK_END,     /* window decoded and zero-filled */
    SWITCH_STAMP_COPY_DONE,      /* cleaned to PoC, caches and MMU off again */
    SWITCH_STAMP_XNU_ENTRY,      /* branch to XNU */
    SWITCH_STAMP_EMC_RESULT,     /* enum loader_emc_result | (uint32_t)BPMP return code << 32 */
    SWITCH_STAMP_EMC_RATES,      /* EMC kHz before | kHz after << 32 */
    SWITCH_STAMP_EMC_CORE,       /* VDD_CORE mV | BPMP channel 0 area << 32 */
    SWITCH_STAMP_COUNT
};

#define SWITCH_STAMP_RECORD_BYTES (SWITCH_STAMP_COUNT * 8u)
#endif
