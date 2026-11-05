#ifndef SWITCH_SD_REFERENCE_H
#define SWITCH_SD_REFERENCE_H
#include <stdint.h>

#define SWITCH_SD_REFERENCE_PA UINT64_C(0xa001f000)
#define SWITCH_SD_REFERENCE_BYTES 0x1000u
#define SWITCH_SD_REFERENCE_MAGIC 0x52445349u
#define SWITCH_SD_REFERENCE_DATA_OFFSET 0x200u
#define SWITCH_SD_REFERENCE_COMMIT_OFFSET 0x2cu
#define SWITCH_SD_CONTROLLER_PA UINT64_C(0x700b0000)
#define SWITCH_SD_CARD_OFFSET 0x400u
#define SWITCH_SD_CARD_BYTES 0x100u
#define SWITCH_SD_CARD_MAGIC 0x4d435349u

/* Same-boot reference page: LE32 fields at 00..24, BE32 data CRC at28,
 * LE32 loader commit at2c. Bytes30..1ff and400..fff are zero. Sector0
 * occupies200..3ff. All addresses and lengths are fixed, never dereferenced
 * from page contents. U-Boot clears/fills the page and writes magic last;
 * the loader changes commit0 to1 only after its other image/memory checks. */
struct switch_sd_reference {
    uint32_t crc32;
    uint8_t sector[512];
};
/* Version2 reference pages additionally carry a checked snapshot of the
 * active pinned firmware's SD descriptor. Pointers are provenance only;
 * neither this validator nor the kernel dereferences them. */
struct switch_sd_card {
    uint64_t capacity_sectors, capacity_bytes;
    uint32_t ocr, rca, version;
    uint32_t csd[4], cid[4];
};
enum switch_sd_reference_error {
    SWITCH_SD_REF_OK=0, SWITCH_SD_REF_ARGUMENT=1, SWITCH_SD_REF_HEADER=2,
    SWITCH_SD_REF_PADDING=3, SWITCH_SD_REF_CRC=4, SWITCH_SD_REF_CHANGED=5,
    SWITCH_SD_REF_CARD=6
};
uint32_t switch_sd_crc32(const uint8_t *, uint32_t);
/* No MMIO and no writes to the page. Output changes only on full success. */
int switch_sd_reference_validate(const volatile uint8_t *page, uint32_t bytes,
                               uint32_t expected_commit,
                               struct switch_sd_reference *output);
/* Explicit version2-only entry. Both outputs change only after the complete
 * sector and card evidence pass. Legacy version1 cannot qualify block reads. */
int switch_sd_card_reference_validate(const volatile uint8_t *page, uint32_t bytes,
                                    uint32_t expected_commit,
                                    struct switch_sd_reference *output,
                                    struct switch_sd_card *card);
#endif
