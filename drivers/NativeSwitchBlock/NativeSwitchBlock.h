#ifndef NATIVE_SWITCH_BLOCK_H
#define NATIVE_SWITCH_BLOCK_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Unique exact-UUID APFS volume under this boot's persistent Data child only.
 * Returns boolean success, observed device/path and immutable logical capacity.
 * Mutable Data has no fixed digest and cannot authorize an OS root. */
int switch_data_volume_path(const char *, dev_t *, char *, size_t, uint64_t *);
/* Same snapshot, returning switch_block_status: NOT_READY is absent owned media
 * or pending BSD properties; CORRUPT is invalid/duplicate identity, IO offline. */
int switch_data_volume_status(const char *, dev_t *, char *, size_t, uint64_t *);
/* Raw profile only: exact caller-supplied OS/Data/Preboot UUID under the one
 * qualified container child. Same snapshot/status contract as Data above.
 * bytes reports physical-store capacity, not a volume quota. Identity only;
 * this query cannot authorize OS root or image-backed media outside the child. */
int switch_raw_volume_status(const char *, dev_t *, char *, size_t, uint64_t *);
/* Failure-only raw root registry/counter snapshot; never submits storage I/O. */
void switch_raw_root_wait_diagnostic(void);
#ifdef __cplusplus
}
#endif
#endif
