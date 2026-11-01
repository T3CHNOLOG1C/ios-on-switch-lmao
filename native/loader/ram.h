#ifndef IOSWITCH_RAM_LEDGER_H
#define IOSWITCH_RAM_LEDGER_H
/* Native little-endian ADT wire format. Filled only after live FDT/MC gates. */
#define IOSW_RAM_MAGIC 0x324d4152u
#define IOSW_RAM_RANGES 96u
struct iosw_ram_range { unsigned long long start, end; };
struct iosw_ram_ledger {
    unsigned int magic, version, count, committed;
    struct iosw_ram_range banks[2];
    struct iosw_ram_range reserved[IOSW_RAM_RANGES];
};
#endif
