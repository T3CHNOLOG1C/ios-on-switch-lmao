// Host harness for drivers/SwitchNVRAM/SwitchNVRAMStore.h (tests/test_switch_nvram_store.py): the header's static
// inline functions as exported ones for ctypes, and, built with SWITCH_NVRAM_HARNESS_MAIN under ASan/UBSan, a bounds
// check of the slot picker on files of every length.
#include <stdio.h>
#include <stdlib.h>

#include "SwitchNVRAMStore.h"

uint32_t nv_crc32(const uint8_t *data, size_t length) { return switch_nvram_crc32(0, data, length); }
uint32_t nv_stride(uint32_t bytes) { return switch_nvram_store_stride(bytes); }
int nv_chrp_valid(const uint8_t *image, uint32_t bytes) { return switch_nvram_chrp_valid(image, bytes); }

void nv_fill(uint8_t *slot, const uint8_t *image, uint32_t bytes, uint64_t sequence)
{
    switch_nvram_store_fill(slot, image, bytes, sequence);
}

int nv_pick(const uint8_t *file, size_t length, uint32_t bytes, uint64_t *sequence)
{
    return switch_nvram_store_pick(file, length, bytes, sequence);
}

#ifdef SWITCH_NVRAM_HARNESS_MAIN
// A valid 8 KiB CHRP bank: the packaging's empty proxy ("nvram" header, 2 KiB "common", "free") with one variable.
static void bank(uint8_t *image, uint32_t bytes)
{
    static const char header[] = "nvram", common[] = "common", variable[] = "boot-args=-v";
    memset(image, 0, bytes);
    image[0] = 0x70;
    image[2] = 2; // 32 bytes in 16-byte blocks
    memcpy(image + 4, header, sizeof(header) - 1);
    image[32] = 0x70;
    image[32 + 2] = (uint8_t)((16 + 2048) / 16);
    image[32 + 3] = (uint8_t)((16 + 2048) / 16 >> 8);
    memcpy(image + 36, common, sizeof(common) - 1);
    memcpy(image + 48, variable, sizeof(variable));
    for (int at = 0; at <= 32; at += 32) {
        uint32_t sum = image[at];
        for (int i = 2; i < 16; ++i) sum += image[at + i];
        while (sum > 0xff) sum = (sum & 0xff) + (sum >> 8);
        image[at + 1] = (uint8_t)sum;
    }
    uint32_t low = 1, high = 0;
    for (uint32_t i = 20; i < bytes; ++i) {
        low = (low + image[i]) % 65521u;
        high = (high + low) % 65521u;
    }
    switch_nvram_put32(image + 16, high << 16 | low);
}

int main(void)
{
    enum { kBytes = 8192 };
    static uint8_t image[kBytes];
    bank(image, kBytes);
    if (!switch_nvram_chrp_valid(image, kBytes)) {
        puts("FAIL: the harness bank is not valid");
        return 1;
    }
    const uint32_t stride = switch_nvram_store_stride(kBytes);
    uint8_t *file = calloc(2, stride);
    if (!file) return 1;
    switch_nvram_store_fill(file, image, kBytes, 1);
    switch_nvram_store_fill(file + stride, image, kBytes, 2);
    unsigned cases = 0;
    // Every length of the two-slot file, each in an allocation of exactly that size (ASan catches a read past it).
    for (size_t length = 0; length <= 2u * stride; ++length) {
        uint8_t *copy = malloc(length ? length : 1);
        if (!copy) return 1;
        memcpy(copy, file, length);
        uint64_t sequence = 0;
        const int picked = switch_nvram_store_pick(copy, length, kBytes, &sequence);
        const int expected = length >= stride + SWITCH_NVRAM_STORE_HEADER + kBytes ? 1
                             : length >= SWITCH_NVRAM_STORE_HEADER + kBytes        ? 0
                                                                                   : -1;
        free(copy);
        if (picked != expected || (picked >= 0 && sequence != (uint64_t)picked + 1)) {
            printf("FAIL: length %zu picked %d (sequence %llu), expected %d\n", length, picked,
                   (unsigned long long)sequence, expected);
            return 1;
        }
        ++cases;
    }
    free(file);
    printf("PASS: %u file lengths\n", cases);
    return 0;
}
#endif
