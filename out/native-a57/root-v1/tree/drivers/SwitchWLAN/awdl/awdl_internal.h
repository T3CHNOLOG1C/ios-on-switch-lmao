// SPDX-License-Identifier: BSD-2-Clause-Patent
// Internals shared by the AWDL core's units. Not for the integration (awdl.h is the interface).
#ifndef AWDL_INTERNAL_H
#define AWDL_INTERNAL_H

#include "awdl.h"

#include <stdarg.h>
#include <string.h>
#ifdef KERNEL
extern int vsnprintf(char *, size_t, const char *, va_list) __attribute__((format(printf, 3, 0)));
#else
#include <stdio.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AWDL_AW_US             (AWDL_AW_TU * AWDL_TU_US)            // 16384
#define AWDL_EAW_US            (AWDL_AW_US * AWDL_EAW_AWS)          // 65536
#define AWDL_SEQ_US            (AWDL_EAW_US * AWDL_SEQ_SLOTS)       // 1048576
#define AWDL_PSF_US            (AWDL_PSF_INTERVAL_TU * AWDL_TU_US)  // 112640
#define AWDL_ROOT_COUNTER_US   1000000u                             // the root's loop-free counter ticks once a second
#define AWDL_ELECTION_FRESH_US 2000000u                             // election parameters older than this are ignored
#define AWDL_RESYNC_US         256                                  // re-install the radio schedule beyond this error

struct awdl_writer {
    uint8_t *buf;
    size_t size, at, tlv;
    int overflow;
};
void awdl_put16(uint8_t *p, uint16_t v);
void awdl_put32(uint8_t *p, uint32_t v);
int awdl_w_begin(struct awdl_writer *w, uint8_t type);
void awdl_w_end(struct awdl_writer *w);
void awdl_w_bytes(struct awdl_writer *w, const void *p, size_t n);
void awdl_w_u8(struct awdl_writer *w, uint8_t v);
void awdl_w_u16(struct awdl_writer *w, uint16_t v);
void awdl_w_u32(struct awdl_writer *w, uint32_t v);
void awdl_w_chanseq(struct awdl_writer *w, const struct awdl_chan seq[AWDL_SEQ_SLOTS]);

struct awdl_txq_entry {
    uint16_t len;
    uint8_t group;
    uint8_t dst[6];
    uint8_t frame[AWDL_MAX_FRAME];
};

struct awdl_core {
    struct awdl_host host;
    struct awdl_config cfg;
    int running;
    // timing: AW number base_seq starts at local TSF base_tsf
    uint64_t base_tsf;
    uint16_t base_seq;
    uint64_t next_psf, next_slot, last_counter_tick, anchor;
    // election
    enum awdl_role role;
    struct awdl_election_v2 adv;  // what we advertise (root, parent, hops, metrics, counters)
    uint32_t self_counter;
    // peers and transmit queue
    struct awdl_peer peers[AWDL_MAX_PEERS];
    struct awdl_txq_entry txq[AWDL_TXQ_FRAMES];
    unsigned txq_head, txq_count;
    uint16_t data_seq, mgmt_seq, service_update_indicator;
    struct awdl_stats stats;
    // native backend: the firmware's AWDL interface and its bring-up
    uint8_t nstate;        // AWDL_N_*
    uint8_t created_with;  // enum bcm_awdl_api used for the interface being made / made
    uint8_t payload_u16;   // awdl_payload's length form (u16: interface_create v3; u32: 7.x), switched once on refusal
    uint8_t payload_switched;
    uint8_t seq_no_empty;  // the 7.x awdl_chan_seq refused empty slots once: free slots take slot 0's channel
    uint8_t social5;       // 5 GHz social channel learnt from the master's sequence (44 or 149), 0 before
    int16_t fw_ifidx;      // the AWDL interface's firmware index (WLC_E_IF add), -1 before
    uint8_t fw_bsscfg;
    uint16_t infra_mhz;
    uint64_t create_deadline, aftx_at, settle_at;  // 0: not pending
    uint8_t pushed_role, pushed_parent[6], pushed_root[6];
    uint8_t scratch[AWDL_MAX_FRAME];
};
enum { AWDL_N_OFF, AWDL_N_CREATING, AWDL_N_UP };
#define AWDL_CREATE_TIMEOUT_US 2000000u  // the series' BRCMF_AWDL_IF_TIMEOUT
#define AWDL_AFTX_DELAY_US     500000u
#define AWDL_SETTLE_DELAY_US   5000000u

void awdl_log(struct awdl_core *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif
