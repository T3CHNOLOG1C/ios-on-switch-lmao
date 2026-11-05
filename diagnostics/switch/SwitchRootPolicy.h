#ifndef SWITCH_ROOT_POLICY_H
#define SWITCH_ROOT_POLICY_H
#include "verified_root.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The loader publishes both descriptors together only after exact role/size/
 * image identity, signature and complete table authentication, with envelope
 * and descriptor memory permanently read-only. No untrusted key is accepted. */
struct switch_root_policy {
    struct vr_view view;
    const uint8_t *envelope;
    uint64_t envelope_bytes;
};
const struct switch_root_policy *switch_root_policy_get(unsigned disk);
/* The block owner calls this after releasing its gate, at most each MiB of
 * successful verified requests or on an error. It may render diagnostic state. */
void switch_root_io_progress(uint64_t verified_bytes, unsigned error);
extern const struct vr_crypto vr_xnu_crypto;
extern const uint8_t verified_root_key[VR_KEY_BYTES];
#ifdef __cplusplus
}
#endif
#endif
