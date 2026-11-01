#ifndef IOSW_FIRMWARE_EXPORT_H
#define IOSW_FIRMWARE_EXPORT_H
#include <stdint.h>

#define IOSW_FW_EXPORT_PA UINT64_C(0xa001f400)
#define IOSW_FW_EXPORT_BYTES 256u
#define IOSW_FW_EXPORT_MAGIC 0x4d435349u
#define IOSW_FW_IMAGE_LINK UINT64_C(0x88110000)

/* This is an internal-layout reader for one pinned U-Boot binary, not a
 * general U-Boot API. The callback copies RAM into a caller-owned buffer.
 * Neither the parser nor its native adapter invokes any firmware function. */
struct iosw_fw_reader {
    void *context;
    int (*copy)(void *, uint64_t address, uint8_t *output, uint32_t bytes);
};
enum iosw_fw_error {
    IOSW_FW_OK=0, IOSW_FW_ARGUMENT=1, IOSW_FW_GLOBAL_DATA=2,
    IOSW_FW_SIGNATURE=3, IOSW_FW_HEAP=4, IOSW_FW_CLASSES=5,
    IOSW_FW_DEVICES=6, IOSW_FW_CARD_LINKS=7, IOSW_FW_CARD_STATE=8,
    IOSW_FW_CAPACITY=9, IOSW_FW_CHANGED=10
};
/* Two complete, separately validated snapshots must agree. The 256-byte
 * result remains unchanged on failure. Integers in the record are LE,
 * including its CRC32 at 0xf0; its static SHA256 identifies the pinned ABI
 * artifact, and is not a measurement of the live relocated image. */
int iosw_fw_capture(const struct iosw_fw_reader *, uint64_t global_data,
                    uint8_t output[IOSW_FW_EXPORT_BYTES]);
#endif
