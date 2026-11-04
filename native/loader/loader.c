#include <stdint.h>
#include "record.h"
#include "display.h"
#include "layout.h"
#include "memory.h"
#include "sd_reference.h"
#include "copy.h"

#ifndef LOAD_FAST_COPY
#define LOAD_FAST_COPY 0
#endif

extern volatile uint64_t probe_record[R_RECORD_WORDS];
extern void probe_halt(void) __attribute__((noreturn));
extern void loader_enter_kernel(uint64_t args, uint64_t entry) __attribute__((noreturn));
#if LOAD_VERIFY_KERNEL
extern uint32_t loader_crc32_update(uint32_t crc, const volatile void *address, uint64_t bytes);
#endif
volatile uint64_t loader_progress[8] __attribute__((section(".data"))) = {0};
#if LOAD_FAST_COPY
volatile uint64_t loader_copy_prepare_ticks __attribute__((section(".data"))) = 0;
#endif

// Additional words in the existing 512-byte diagnostic record.
enum { L_SOURCE_CRC=48, L_DEST_CRC, L_SOURCE, L_DEST, L_BYTES, L_ARGS, L_ENTRY,
       L_MEMORY_CHECK, L_RESERVATIONS, L_RESERVED_NODES, L_GICD_CTLR, L_GICD_TYPER,
       L_GICD_ENABLE, L_GICC_CTLR, L_GICC_PMR, L_GICC_IIDR };

_Static_assert((LOAD_BYTES & 7) == 0, "copy requires complete aligned words");
_Static_assert((LOAD_SOURCE & 7) == 0 && (LOAD_DEST & 7) == 0, "unaligned copy");
_Static_assert(LOAD_BYTES > 0 && LOAD_BYTES <= 0x8000000, "bounded payload");
_Static_assert(LOAD_SOURCE + LOAD_BYTES <= LOAD_DEST || LOAD_DEST + LOAD_BYTES <= LOAD_SOURCE,
               "copy source and destination overlap");
_Static_assert(LOAD_ARGS >= LOAD_DEST && LOAD_ARGS + 0x480 <= LOAD_DEST + LOAD_BYTES,
               "arguments outside payload");
_Static_assert(LOAD_ENTRY >= LOAD_DEST && LOAD_ENTRY < LOAD_DEST + LOAD_BYTES,
               "entry outside payload");

static uint64_t ticks(void) {
    uint64_t value;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value) :: "memory");
    return value;
}

#if !LOAD_FAST_COPY || LOAD_VERIFY_KERNEL
static void progress(unsigned row, uint64_t done, uint64_t total, uint64_t started) {
    unsigned percent = (unsigned)(done * 100 / total);
    uint64_t elapsed = (ticks() - started) / 19200;
    char line[] = "000 PERCENT    000000 MS";
    for (unsigned i=0; i<3; ++i) { line[2-i]=(char)('0'+percent%10); percent/=10; }
    if (elapsed > 999999) elapsed=999999;
    for (unsigned i=0; i<6; ++i) { line[20-i]=(char)('0'+elapsed%10); elapsed/=10; }
    display_text(500,row,line,DISPLAY_WHITE);
    loader_progress[2]=done;
}
#endif

#if LOAD_FAST_COPY
// Timing only (the record words, for a debugger): nothing is drawn on the way to the kernel.
static void copy_progress(uint64_t done, uint64_t total, uint64_t started) {
    uint64_t elapsed=ticks()-started;
    // The zero callback occurs after the temporary cache regime is enabled,
    // before the first payload load/store.
    if (!done) loader_copy_prepare_ticks=elapsed;
    (void)total;
    loader_progress[7]=elapsed;
    loader_progress[2]=done;
}
#endif

#if LOAD_VERIFY_KERNEL
static uint32_t checksum(uint64_t base, unsigned row) {
    uint64_t started=ticks();
    uint32_t crc=UINT32_MAX;
    progress(row,0,LOAD_BYTES,started);
    for (uint64_t offset=0; offset<LOAD_BYTES;) {
        uint64_t bytes=LOAD_BYTES-offset;
        if (bytes > 0x100000) bytes=0x100000;
        crc=loader_crc32_update(crc,(const volatile void *)(uintptr_t)(base+offset),bytes);
        offset+=bytes;
        progress(row,offset,LOAD_BYTES,started);
    }
    loader_progress[row==64 ? 3 : 5]=ticks()-started;
    return ~crc;
}
#endif

static void refuse(uint64_t reason) __attribute__((noreturn));
static void refuse(uint64_t reason) {
    probe_record[R_FAILURE] = reason;
    display_text(16, 490, "LOADER REFUSED", DISPLAY_RED);
    char number[17];
    const char digits[] = "0123456789ABCDEF";
    for (unsigned i=0; i<16; ++i) number[i]=digits[(reason >> ((15-i)*4)) & 15];
    number[16]=0;
    display_text(16, 520, "REFUSAL CODE", DISPLAY_WHITE);
    display_text(300, 520, number, DISPLAY_WHITE);
    probe_halt();
}

#if LOAD_FAST_COPY
void loader_copy_refuse(uint64_t reason) __attribute__((noreturn));
void loader_copy_refuse(uint64_t reason) { refuse(reason); }
#endif

static uint64_t native_gate(void) {
#if LOAD_NATIVE_GATES
    // Recognize the photographed inherited stack before switching to ours.
    // The full RAM profile later retires completed U-Boot allocations; live
    // FDT reservations, secure carveouts and the display remain protected.
    if (probe_record[R_ENTRY_SP] < UINT64_C(0xf0001000) ||
        probe_record[R_ENTRY_SP] >= UINT64_C(0xffd00000)) return 0x401;
    // Exact bounded configuration reads already photographed in IMG_5510.
    // Refuse a changed carveout layout instead of inferring new free memory.
    static const uint32_t mc[][2] = {
        {0x70019050,0x1000}, {0x70019670,0}, {0x70019674,0}, {0x700199d4,0},
        {0x700199a0,0xfff00000}, {0x700199a4,0}, {0x700199a8,0},
        {0x70019648,0}, {0x7001964c,0}, {0x70019978,0},
        {0x70019c0c,0xffe00000}, {0x70019c10,0}, {0x70019c14,8},
        {0x70019c5c,0xffd00000}, {0x70019c60,0}, {0x70019c64,2},
        {0x70019cac,0xffd40000}, {0x70019cb0,0}, {0x70019cb4,6},
        {0x70019cfc,0}, {0x70019d00,0}, {0x70019d04,0},
        {0x70019d4c,0}, {0x70019d50,0}, {0x70019d54,0},
    };
    for (unsigned i=0; i<sizeof(mc)/sizeof(mc[0]); ++i)
        if (*(const volatile uint32_t *)(uintptr_t)mc[i][0] != mc[i][1]) return 0x410+i;
    // Non-destructive register observation only: never read IAR or change
    // security groups. The kernel will check its accessible NS interface.
    const uint32_t addresses[]={0x50041000,0x50041004,0x50041100,0x50042000,0x50042004,0x500420fc};
    for (unsigned i=0; i<6; ++i)
        probe_record[L_GICD_CTLR+i]=*(const volatile uint32_t *)(uintptr_t)addresses[i];
#endif
    return 0;
}

void loader_after_transition(void) {
    volatile uint64_t *r = probe_record;
    if (r[R_EL1_EL] != 4 || (r[R_EL1_SCTLR] & 0x1005) ||
        (r[R_EL1_DAIF] & 0x3c0) != 0x3c0 || r[R_EL1_CNTFRQ] != 19200000 ||
        r[R_HCR_AFTER] != 0x80000000 || r[R_CNTHCTL_AFTER] != 3 || r[R_CNTVOFF_AFTER])
        refuse(0x301);
    struct loader_memory_report memory;
    uint64_t failure = (uint64_t)loader_check_fdt_memory(
        (const uint8_t *)(uintptr_t)r[R_FDT], 0x40000,
        LOAD_DEST & ~UINT64_C(0x1fffff),
        (LOAD_DEST + LOAD_WINDOW + 0x1fffff) & ~UINT64_C(0x1fffff), &memory);
    r[L_MEMORY_CHECK]=failure; r[L_RESERVATIONS]=memory.reservations;
    r[L_RESERVED_NODES]=memory.reserved_nodes;
    if (failure) refuse(0x500+failure);
#if LOAD_SD_REFERENCE
    // The reference uses the last page of our existing 128KiB adapter region.
    // The builder checks that every load segment and stack ends before it.
    // Do not access that page until the live FDT rejects any retained overlap.
    failure=(uint64_t)loader_check_fdt_memory(
        (const uint8_t *)(uintptr_t)r[R_FDT],0x40000,
        IOSW_SD_REFERENCE_PA,IOSW_SD_REFERENCE_PA+IOSW_SD_REFERENCE_BYTES,&memory);
    if (failure) refuse(0x600+failure);
#endif
#if LOAD_FAST_COPY
    /* Cacheable mappings cover only the exact staged bytes and loader image,
     * in addition to the destination envelope already checked above. Tables
     * are image-backed and included in __image_end, below the SD reference. */
    extern char __image_end[];
    failure=(uint64_t)loader_check_fdt_memory(
        (const uint8_t *)(uintptr_t)r[R_FDT],0x40000,
        LOAD_SOURCE,LOAD_SOURCE+LOAD_BYTES,&memory);
    if (failure) refuse(0x680+failure);
    failure=(uint64_t)loader_check_fdt_memory(
        (const uint8_t *)(uintptr_t)r[R_FDT],0x40000,
        UINT64_C(0xa0000000),(uint64_t)(uintptr_t)__image_end,&memory);
    if (failure) refuse(0x6c0+failure);
#endif
    failure=native_gate();
    if (failure) refuse(failure);
#if LOAD_VERIFY_KERNEL
    uint64_t isar0;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    // CRC32 is optional in Armv8.0; the physical OLED probe reports field=1.
    // Refuse unexpected CPUs before executing the optional instruction.
    if (((isar0 >> 16) & 15) != 1) refuse(0x305);
    loader_progress[0]=UINT64_C(0x494f535743524332);
    loader_progress[6]=isar0;
#else
    loader_progress[0]=UINT64_C(0x494f5357434f5059);
#endif
    r[L_SOURCE] = LOAD_SOURCE; r[L_DEST] = LOAD_DEST; r[L_BYTES] = LOAD_BYTES;
    r[L_ARGS] = LOAD_ARGS; r[L_ENTRY] = LOAD_ENTRY;
    r[R_STAGE] = 0x50;
#if !LOAD_FAST_COPY
    display_clear();
    display_text(16, 24, "LOADER", DISPLAY_WHITE);
#endif
#if LOAD_VERIFY_KERNEL
    display_text(16,64,"CHECK SOURCE",DISPLAY_WHITE);
    display_text(16,144,"CHECK DESTINATION",DISPLAY_WHITE);
    loader_progress[1]=1;
    r[L_SOURCE_CRC] = checksum(LOAD_SOURCE,64);
    if (r[L_SOURCE_CRC] != LOAD_CRC) refuse(0x302);
#endif
#if !LOAD_FAST_COPY
    display_text(16,104,"COPY KERNEL",DISPLAY_WHITE);
#endif
#if LOAD_SD_REFERENCE
    struct iosw_sd_reference reference;
#if LOAD_SD_CARD
    struct iosw_sd_card card;
    failure=(uint64_t)iosw_sd_card_reference_validate(
        (const volatile uint8_t *)(uintptr_t)IOSW_SD_REFERENCE_PA,
        IOSW_SD_REFERENCE_BYTES,0,&reference,&card);
#else
    failure=(uint64_t)iosw_sd_reference_validate(
        (const volatile uint8_t *)(uintptr_t)IOSW_SD_REFERENCE_PA,
        IOSW_SD_REFERENCE_BYTES,0,&reference);
#endif
    if (failure) refuse(0x640+failure);
#endif
    r[R_STAGE] = 0x51;
    uint64_t started=ticks();
    loader_progress[1]=2;
#if LOAD_FAST_COPY
    failure=loader_copy_run(copy_progress,started);
    if (failure) refuse(failure);
#else
    progress(104,0,LOAD_BYTES,started);
    const volatile uint64_t *source = (const volatile uint64_t *)(uintptr_t)LOAD_SOURCE;
    volatile uint64_t *dest = (volatile uint64_t *)(uintptr_t)LOAD_DEST;
    for (uint64_t offset=0; offset<LOAD_BYTES;) {
        uint64_t end=offset+0x100000;
        if (end > LOAD_BYTES) end=LOAD_BYTES;
        for (uint64_t i=offset/8; i<end/8; ++i) dest[i]=source[i];
        offset=end;
        progress(104,offset,LOAD_BYTES,started);
    }
#endif
    __asm__ volatile("dsb sy" ::: "memory");
    loader_progress[4]=ticks()-started;
    loader_progress[1]=3;
#if LOAD_VERIFY_KERNEL
    r[L_DEST_CRC] = checksum(LOAD_DEST,144);
    if (r[L_DEST_CRC] != LOAD_CRC) refuse(0x303);
#endif
    const volatile uint64_t *a = (const volatile uint64_t *)(uintptr_t)LOAD_ARGS;
    if (a[0] != 0x00020002 || a[1] != LOAD_BOOT_VIRT || a[2] != LOAD_BOOT_PHYS ||
        a[3] != LOAD_BOOT_SIZE || a[4] != LOAD_TOP || a[12] != LOAD_ADT_VA ||
        a[0x478 / 8] != LOAD_BOOT_ACTUAL ||
        *(const volatile uint32_t *)(uintptr_t)(LOAD_ARGS + 0x68) != LOAD_ADT_BYTES)
        refuse(0x304);
#if LOAD_FULL_RAM
    /* bootm has quiesced devices before cleanup. Refuse an active SD data
     * engine before retiring the old U-Boot heap/noncached pool. */
    if ((*(volatile uint32_t *)(uintptr_t)0x700b0024 & 0x307) ||
        *(volatile uint8_t *)(uintptr_t)0x700b002f || memory.bank_count != 2)
        refuse(0x710);
    if (memory.ledger.count+1 > IOSW_RAM_RANGES) refuse(0x711);
    memory.ledger.reserved[memory.ledger.count++]=(struct iosw_ram_range){0xa0000000,0xa0020000};
    memory.ledger.committed=1;
    volatile uint8_t *ledger=(volatile uint8_t *)(uintptr_t)(LOAD_ADT_PA+LOAD_RAM_ADT_OFFSET);
    const uint8_t *wire=(const uint8_t *)&memory.ledger;
    for (unsigned i=0;i<sizeof(memory.ledger);++i) {
        if (ledger[i]) refuse(0x712);
    }
    for (unsigned i=0;i<sizeof(memory.ledger);++i) ledger[i]=wire[i];
    __asm__ volatile("dsb sy" ::: "memory");
#endif
#if LOAD_SD_REFERENCE
    // Verify the immutable ADT pointer in the actual destination, then commit
    // only the external reference page. The staged/kernel image stays intact.
    const volatile uint8_t *sd_bytes=(const volatile uint8_t *)(uintptr_t)
        (LOAD_DEST+(LOAD_ADT_VA-LOAD_VIRT)+LOAD_SD_REFERENCE_ADT_OFFSET);
    // AppleDT properties are only four-byte aligned; load individual bytes.
    uint64_t sd_address=0,sd_size=0;
    for (unsigned i=0;i<8;++i) {
        sd_address|=(uint64_t)sd_bytes[i]<<(8*i);
        sd_size|=(uint64_t)sd_bytes[8+i]<<(8*i);
    }
    if (sd_address!=IOSW_SD_REFERENCE_PA || sd_size!=IOSW_SD_REFERENCE_BYTES)
        refuse(0x650);
#if LOAD_SD_CARD
    failure=(uint64_t)iosw_sd_card_reference_validate(
        (const volatile uint8_t *)(uintptr_t)IOSW_SD_REFERENCE_PA,
        IOSW_SD_REFERENCE_BYTES,0,&reference,&card);
#else
    failure=(uint64_t)iosw_sd_reference_validate(
        (const volatile uint8_t *)(uintptr_t)IOSW_SD_REFERENCE_PA,
        IOSW_SD_REFERENCE_BYTES,0,&reference);
#endif
    if (failure) refuse(0x660+failure);
    *(volatile uint32_t *)(uintptr_t)(IOSW_SD_REFERENCE_PA+IOSW_SD_REFERENCE_COMMIT_OFFSET)=1;
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    r[R_STAGE] = 0x52;
    loader_progress[1]=4;
#if !LOAD_FAST_COPY
    display_text(16, 200, "COPY COMPLETE", DISPLAY_GREEN);
#endif
    loader_enter_kernel(LOAD_ARGS, LOAD_ENTRY);
}
