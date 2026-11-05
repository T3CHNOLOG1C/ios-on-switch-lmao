// switch_nvram_store_harness.c's exported wrappers of drivers/SwitchNVRAM/SwitchNVRAMStore.h, for
// switch_nvram_store_test.cpp.
#ifndef IOSWITCH_SWITCH_NVRAM_STORE_HARNESS_H
#define IOSWITCH_SWITCH_NVRAM_STORE_HARNESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t nv_crc32(const uint8_t *data, size_t length);
uint32_t nv_stride(uint32_t bytes);
int nv_chrp_valid(const uint8_t *image, uint32_t bytes);
void nv_fill(uint8_t *slot, const uint8_t *image, uint32_t bytes, uint64_t sequence);
int nv_pick(const uint8_t *file, size_t length, uint32_t bytes, uint64_t *sequence);

#ifdef __cplusplus
}
#endif

#endif  // IOSWITCH_SWITCH_NVRAM_STORE_HARNESS_H
