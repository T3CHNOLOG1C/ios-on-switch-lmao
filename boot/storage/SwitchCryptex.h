// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_CRYPTEX_H
#define SWITCH_CRYPTEX_H
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#define SWITCH_CRYPTEX_COUNT       2
#define SWITCH_PREBOOT_UUID        "4AA4A761-5138-439D-9A30-D4138F6887FA"
#define SWITCH_SYSTEM_CRYPTEX_UUID "8846FE78-2910-4297-AF97-D59BB6F8FC16"
#define SWITCH_APP_CRYPTEX_UUID    "C0DCFAA8-4E81-49CD-807D-BED3B9B6AE54"
#ifdef __cplusplus
extern "C" {
#endif
/* Boot-only serialized original DiskImages attach. index0=System,1=App.
 * Fixed root-owned files under /private/preboot/Switch-Staging; exact native
 * RW Preboot UUID/device, backing vnode and retained RO provider are required.
 * Returns BSD errno (0 success), not switch_block_status. An attachment failure
 * latches for this index: the original hook has no transactional detach API. */
int switch_cryptex_attach(unsigned index, dev_t expectedPrebootDevice, dev_t *volume, char *devicePath,
                          size_t capacity);
/* Revalidates the retained binding and unique exact-UUID RO APFS descendant.
 * Returns BSD errno; can be called after mounting to compare actual dev_t. */
int switch_cryptex_volume_status(unsigned index, dev_t *volume, char *devicePath, size_t capacity);
/* Only retained, valid System binding. Existing fixed development identity;
 * this is neither an image hash calculation nor Apple root authentication. */
int switch_cryptex_manifest_sha384(dev_t volume, const char *uuid, uint8_t digest[48]);
#ifdef __cplusplus
}
#endif
#endif
