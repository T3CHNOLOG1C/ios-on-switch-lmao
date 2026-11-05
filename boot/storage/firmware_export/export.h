#ifndef SWITCH_FIRMWARE_EXPORT_H
#define SWITCH_FIRMWARE_EXPORT_H
#include <stdint.h>

#define SWITCH_FW_EXPORT_PA UINT64_C(0xa001f400)
#define SWITCH_FW_EXPORT_BYTES 256u
#define SWITCH_FW_EXPORT_MAGIC 0x4d435349u
#define SWITCH_FW_IMAGE_LINK UINT64_C(0x88110000)

/* This is an internal-layout reader for one pinned U-Boot binary, not a
 * general U-Boot API. The callback copies RAM into a caller-owned buffer.
 * Neither the parser nor its native adapter invokes any firmware function. */
struct switch_fw_reader {
    void *context;
    int (*copy)(void *, uint64_t address, uint8_t *output, uint32_t bytes);
};
enum switch_fw_error {
    SWITCH_FW_OK=0, SWITCH_FW_ARGUMENT=1, SWITCH_FW_GLOBAL_DATA=2,
    SWITCH_FW_SIGNATURE=3, SWITCH_FW_HEAP=4, SWITCH_FW_CLASSES=5,
    SWITCH_FW_DEVICES=6, SWITCH_FW_CARD_LINKS=7, SWITCH_FW_CARD_STATE=8,
    SWITCH_FW_CAPACITY=9, SWITCH_FW_CHANGED=10
};
/* Two complete, separately validated snapshots must agree. The 256-byte
 * result remains unchanged on failure. Integers in the record are LE,
 * including its CRC32 at 0xf0; its static SHA256 identifies the pinned ABI
 * artifact, and is not a measurement of the live relocated image. */
int switch_fw_capture(const struct switch_fw_reader *, uint64_t global_data,
                    uint8_t output[SWITCH_FW_EXPORT_BYTES]);
#endif
