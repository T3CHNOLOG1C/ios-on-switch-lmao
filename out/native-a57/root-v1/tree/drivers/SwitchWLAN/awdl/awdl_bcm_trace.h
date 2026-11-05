// SPDX-License-Identifier: GPL-2.0-only
// The bytes awdl_bcm_trace.c carries (see its notice): what macOS 15.4.1 sets on a BCM4377, as the brcmfmac series
// traced them.
#ifndef AWDL_BCM_TRACE_H
#define AWDL_BCM_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const uint8_t bcm_trace_sync_params[36];
extern const uint8_t bcm_trace_psf_dwell[8];
extern const uint8_t bcm_trace_oob_hdr[44];
extern const uint8_t bcm_trace_opmode[28];
extern const uint8_t bcm_trace_election_tree[42];
extern const uint8_t bcm_trace_slot_bss[158];

#ifdef __cplusplus
}
#endif

#endif
