/*
 * Embedded code signatures of translated files: Mach-O slices and dyld shared cache files. Translation changes
 * code pages only, so a signature is redone in place: every CodeDirectory's code slots are rehashed over the new
 * bytes; the special slots (Info.plist, requirements, entitlements, launch constraints) hash things that did not
 * change and stay. The blob keeps its size and place. Only ad hoc signatures with SHA-256 CodeDirectories are taken
 * (what iOS ships for everything a trust cache covers); anything else is refused, never re-signed differently.
 */
#ifndef A64X_CODESIGN_H
#define A64X_CODESIGN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A64X_CDHASH_SIZE 20

/*
 * buf[0, size) is the slice or cache file; its SuperBlob is at sig_off. Rehashes the code slots of every
 * CodeDirectory from buf and returns the cdhash of the SHA-256 one in cdhash. 0, or -1 with a message.
 */
int a64x_cs_rehash(uint8_t *buf, size_t size, uint64_t sig_off, uint64_t sig_size, uint8_t cdhash[A64X_CDHASH_SIZE],
                   char *err, size_t errlen);

/* Checks every code slot of every CodeDirectory against buf and gives the cdhash. 0, or -1 with the first mismatch. */
int a64x_cs_check(const uint8_t *buf, size_t size, uint64_t sig_off, uint64_t sig_size,
                  uint8_t cdhash[A64X_CDHASH_SIZE], char *err, size_t errlen);

/* LC_CODE_SIGNATURE of a thin 64-bit Mach-O: 0 and its blob's place, -1 if it has none. */
int a64x_cs_find(const uint8_t *slice, size_t size, uint64_t *sig_off, uint64_t *sig_size);

#ifdef __cplusplus
}
#endif

#endif /* A64X_CODESIGN_H */
