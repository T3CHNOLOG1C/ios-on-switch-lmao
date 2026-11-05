#ifndef SWITCH_RAM_LEDGER_H
#define SWITCH_RAM_LEDGER_H
/* Native little-endian ADT wire format. Filled only after live FDT/MC gates. */
#define SWITCH_RAM_MAGIC 0x324d4152u
#define SWITCH_RAM_RANGES 96u
struct switch_ram_range { unsigned long long start, end; };
struct switch_ram_ledger {
    unsigned int magic, version, count, committed;
    struct switch_ram_range banks[2];
    struct switch_ram_range reserved[SWITCH_RAM_RANGES];
};
#endif
