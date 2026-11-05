#include "export.h"

static int copy_ram(void *context,uint64_t address,uint8_t *out,uint32_t bytes) {
    (void)context;
    const volatile uint8_t *source=(const volatile uint8_t *)(uintptr_t)address;
    for (uint32_t i=0;i<bytes;++i)out[i]=source[i];
    return 0;
}
/* Called on the exporter's own stack. Writes only the fixed result record;
 * capture itself only reads the active firmware's cached RAM objects. */
int switch_fw_native(uint64_t gd) {
    volatile uint8_t *target=(volatile uint8_t *)(uintptr_t)SWITCH_FW_EXPORT_PA;
    volatile uint32_t *commit=(volatile uint32_t *)(uintptr_t)SWITCH_FW_EXPORT_PA;
    *commit=0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint8_t record[256];
    const struct switch_fw_reader reader={0,copy_ram};
    int result=switch_fw_capture(&reader,gd,record);
    if (result) return result;
    for (unsigned i=4;i<256;++i)target[i]=record[i];
    __asm__ volatile("dsb sy" ::: "memory");
    *commit=SWITCH_FW_EXPORT_MAGIC;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
