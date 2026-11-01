#include <stdint.h>
#include "record.h"
#include "display.h"

extern volatile uint64_t probe_record[R_RECORD_WORDS];
extern void probe_halt(void) __attribute__((noreturn));

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static int equal(const char *a, const char *b) {
    unsigned i = 0;
    for (; a[i] && b[i] && a[i] == b[i]; ++i) {}
    return a[i] == b[i];
}
static void field(unsigned y, const char *label, uint64_t value) {
    char hex[17];
    const char digits[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < 16; ++i) hex[i] = digits[(value >> ((15 - i) * 4)) & 15];
    hex[16] = 0;
    display_text(16, y, label, DISPLAY_WHITE);
    display_text(376, y, hex, DISPLAY_WHITE);
}
static uint32_t crc32(const uint8_t *p, uint32_t length) {
    uint32_t crc = UINT32_MAX;
    for (uint32_t i = 0; i < length; ++i) {
        crc ^= p[i];
        for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// Bounded reader for the selected, fixed-location FDT after U-Boot fixups.
// It reads no arbitrary pointer/property address and never modifies the FDT.
static int inspect_fdt(void) {
    if (probe_record[R_FDT] != UINT64_C(0x8d000000)) return 1;
    const uint8_t *fdt = (const uint8_t *)(uintptr_t)probe_record[R_FDT];
    if (be32(fdt) != 0xd00dfeed) return 2;
    uint32_t total = be32(fdt + 4), structure = be32(fdt + 8), strings = be32(fdt + 12);
    uint32_t reserve = be32(fdt + 16), version = be32(fdt + 20), compatible = be32(fdt + 24);
    uint32_t string_size = be32(fdt + 32), structure_size = be32(fdt + 36);
    if (total < 40 || total > 0x40000 || version != 17 || compatible > 17 ||
        structure < 40 || strings < 40 || reserve < 40 || (structure & 3) || (reserve & 7) ||
        structure > total || structure_size > total - structure ||
        strings > total || string_size > total - strings || reserve > total - 16) return 3;
    probe_record[R_FDT_SIZE] = total;
    probe_record[R_FDT_CRC] = crc32(fdt, total);
    unsigned reserve_count = 0, framebuffer_reserved = 0;
    for (uint32_t pos = reserve; ; pos += 16) {
        if (pos > total - 16 || reserve_count > 64) return 4;
        uint64_t base = be64(fdt + pos), size = be64(fdt + pos + 8);
        if (!base && !size) break;
        if (size > UINT64_MAX - base) return 5;
        if (base <= DISPLAY_BASE && base + size >= DISPLAY_BASE + 0x400000) framebuffer_reserved = 1;
        ++reserve_count;
    }
    probe_record[R_FDT_RESERVES] = reserve_count;
    if (!framebuffer_reserved) return 6;

    uint32_t pos = structure, end = structure + structure_size;
    unsigned depth = 0, memory_depth = 0, banks = 0;
    int saw_end = 0;
    while (pos <= end && end - pos >= 4) {
        uint32_t token = be32(fdt + pos); pos += 4;
        if (token == 1) { // FDT_BEGIN_NODE
            uint32_t start = pos;
            while (pos < end && fdt[pos]) ++pos;
            if (pos >= end || depth >= 64) return 7;
            ++depth;
            if (depth == 2 && pos - start >= 6 &&
                fdt[start] == 'm' && fdt[start+1] == 'e' && fdt[start+2] == 'm' &&
                fdt[start+3] == 'o' && fdt[start+4] == 'r' && fdt[start+5] == 'y' &&
                (pos - start == 6 || fdt[start+6] == '@')) memory_depth = depth;
            pos = (pos + 4) & ~3u;
        } else if (token == 2) { // FDT_END_NODE
            if (!depth) return 8;
            if (memory_depth == depth) memory_depth = 0;
            --depth;
        } else if (token == 3) { // FDT_PROP
            if (!depth || end - pos < 8) return 9;
            uint32_t len = be32(fdt + pos), name = be32(fdt + pos + 4); pos += 8;
            if (len > end - pos || name >= string_size) return 10;
            uint32_t n = name;
            while (n < string_size && fdt[strings + n]) ++n;
            if (n == string_size) return 11;
            const char *key = (const char *)(fdt + strings + name);
            if (depth == 1 && (equal(key, "#address-cells") || equal(key, "#size-cells"))) {
                if (len != 4 || be32(fdt + pos) != 2) return 12;
            }
            if (memory_depth == depth && equal(key, "reg")) {
                probe_record[R_MEMORY_REG_BYTES] = len;
                // This firmware serializes CONFIG_NR_DRAM_BANKS=3, including
                // the unused zero-filled third slot. Accept that exact shape
                // without treating any extra nonzero range as usable memory.
                if (banks || (len != 32 && len != 48)) return 13;
                if (len == 48 && (be64(fdt + pos + 32) || be64(fdt + pos + 40))) return 18;
                probe_record[R_BANK0_BASE] = be64(fdt + pos);
                probe_record[R_BANK0_SIZE] = be64(fdt + pos + 8);
                probe_record[R_BANK1_BASE] = be64(fdt + pos + 16);
                probe_record[R_BANK1_SIZE] = be64(fdt + pos + 24);
                banks = 2;
            }
            pos = (pos + len + 3) & ~3u;
        } else if (token == 4) {
            continue; // FDT_NOP
        } else if (token == 9) {
            if (depth) return 14;
            saw_end = 1;
            break;
        } else return 15;
    }
    if (!saw_end || banks != 2) return 16;
    probe_record[R_FDT_BANKS] = banks;
    if (probe_record[R_BANK0_BASE] != UINT64_C(0x80000000) ||
        probe_record[R_BANK0_SIZE] != UINT64_C(0x7fd00000) ||
        probe_record[R_BANK1_BASE] != UINT64_C(0x100000000) ||
        probe_record[R_BANK1_SIZE] != UINT64_C(0x80000000)) return 17;
    return 0;
}

int probe_before_transition(void) {
    display_clear();
    display_text(16, 12, "HANDOFF", DISPLAY_GREEN);
    field(96, "ENTRY EL / EXPECT 8", probe_record[R_ENTRY_EL]);
    field(116, "ENTRY SCTLR", probe_record[R_ENTRY_SCTLR]);
    field(136, "ENTRY DAIF", probe_record[R_ENTRY_DAIF]);
    field(156, "ENTRY STACK", probe_record[R_ENTRY_SP]);
    field(176, "ENTRY HCR", probe_record[R_HCR]);
    field(196, "ENTRY CNTHCTL", probe_record[R_CNTHCTL]);
    field(216, "ENTRY CNTVOFF", probe_record[R_CNTVOFF]);
    field(236, "FDT PHYSICAL", probe_record[R_FDT]);
    uint64_t reason = 0;
    if (probe_record[R_ENTRY_EL] != 8 || (probe_record[R_ENTRY_SCTLR] & 0x1005)) reason = 0x101;
    else if (probe_record[R_X1] || probe_record[R_X2] || probe_record[R_X3]) reason = 0x102;
    else if (((probe_record[R_MIDR] >> 24) & 255) != 0x41 ||
             ((probe_record[R_MIDR] >> 4) & 0xfff) != 0xd07) reason = 0x103;
    else if (probe_record[R_CNTFRQ] != 19200000) reason = 0x104;
    else reason = inspect_fdt();
    field(256, "FDT BYTES", probe_record[R_FDT_SIZE]);
    field(276, "FDT CRC32", probe_record[R_FDT_CRC]);
    field(296, "FDT RESERVATIONS", probe_record[R_FDT_RESERVES]);
    field(316, "BANK 0 BASE", probe_record[R_BANK0_BASE]);
    field(336, "BANK 0 SIZE", probe_record[R_BANK0_SIZE]);
    field(356, "BANK 1 BASE", probe_record[R_BANK1_BASE]);
    field(376, "BANK 1 SIZE", probe_record[R_BANK1_SIZE]);
    field(396, "MEMORY REG BYTES", probe_record[R_MEMORY_REG_BYTES]);
    uint64_t now;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
    probe_record[R_COUNTER_POST] = now;
    if (!reason && now <= probe_record[R_COUNTER_PRE]) reason = 0x105;
    probe_record[R_FAILURE] = reason;
    if (reason) {
        display_text(16, 416, "HANDOFF REFUSED", DISPLAY_RED);
        field(436, "REFUSAL CODE", reason);
        return 0;
    }
    probe_record[R_STAGE] = 2;
    display_text(16, 416, "EL1 ENTRY", DISPLAY_GREEN);
    return 1;
}

void probe_after_transition(void) {
    volatile uint64_t *r = probe_record;
    field(456, "EL1 CURRENTEL / EXPECT 4", r[R_EL1_EL]);
    field(476, "EL1 SCTLR", r[R_EL1_SCTLR]);
    field(496, "EL1 DAIF", r[R_EL1_DAIF]);
    field(516, "EL1 STACK", r[R_EL1_SP]);
    field(536, "EL1 CNTFRQ", r[R_EL1_CNTFRQ]);
    uint64_t now;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
    r[R_EL1_COUNTER2] = now;
    r[R_TICK_DELTA] = now - r[R_EL1_COUNTER];
    field(556, "EL1 COUNTER DELTA", r[R_TICK_DELTA]);
    field(576, "HCR AFTER SETUP", r[R_HCR_AFTER]);
    if (r[R_EL1_EL] == 4 && !(r[R_EL1_SCTLR] & 0x1005) &&
        (r[R_EL1_DAIF] & 0x3c0) == 0x3c0 && r[R_EL1_CNTFRQ] == 19200000 &&
        now > r[R_EL1_COUNTER] && r[R_HCR_AFTER] == 0x80000000 &&
        r[R_CNTHCTL_AFTER] == 3 && r[R_CNTVOFF_AFTER] == 0) {
        r[R_STAGE] = 4;
        display_text(16, 616, "EL1 PASS", DISPLAY_GREEN);
    } else {
        r[R_FAILURE] = 0x201;
        display_text(16, 616, "EL1 STATE FAILED", DISPLAY_RED);
    }
    probe_halt();
}

void probe_show_exception(uint64_t el, uint64_t esr, uint64_t far, uint64_t pc) {
    probe_record[R_STAGE] = 0xee;
    display_text(16, 416, "EXCEPTION", DISPLAY_RED);
    field(456, "FAULT EL", el);
    field(476, "ESR", esr);
    field(496, "FAR", far);
    field(516, "PC", pc);
    field(536, "ENTRY FDT", probe_record[R_FDT]);
    probe_halt();
}
