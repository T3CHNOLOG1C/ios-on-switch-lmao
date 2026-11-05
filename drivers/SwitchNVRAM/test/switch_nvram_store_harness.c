// Host harness for drivers/SwitchNVRAM/SwitchNVRAMStore.h: the header's static inline functions as exported C ones
// (C11, -Wconversion -Wsign-conversion, ASan/UBSan), driven by switch_nvram_store_test.cpp (which also runs the slot
// picker on files of every length, each in an allocation of exactly that size).
#include "../SwitchNVRAMStore.h"
#include "switch_nvram_store_harness.h"

uint32_t nv_crc32(const uint8_t *data, size_t length)
{
    return switch_nvram_crc32(0, data, length);
}
uint32_t nv_stride(uint32_t bytes)
{
    return switch_nvram_store_stride(bytes);
}
int nv_chrp_valid(const uint8_t *image, uint32_t bytes)
{
    return switch_nvram_chrp_valid(image, bytes);
}

void nv_fill(uint8_t *slot, const uint8_t *image, uint32_t bytes, uint64_t sequence)
{
    switch_nvram_store_fill(slot, image, bytes, sequence);
}

int nv_pick(const uint8_t *file, size_t length, uint32_t bytes, uint64_t *sequence)
{
    return switch_nvram_store_pick(file, length, bytes, sequence);
}
