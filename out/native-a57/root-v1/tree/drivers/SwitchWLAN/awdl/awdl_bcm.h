// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Broadcom FullMAC firmware's own AWDL (the "awdl" capability the BCM4356A3's 7.35.252.108 reports): the iovars
// a host driver sets and the events it receives, as byte-exact encoders and decoders. Layouts follow Broadcom's
// wlioctl.h of the same 7.x generation (2014, permissive licence; excerpt and notice in
// out/agent-scratch/r12-awdl-stack/sources/) and the event numbers Infineon's Apache-2.0 WHD gives; the iovar names
// are the ones Apple's own BCM driver sends (AWDL-NATIVE.md). Written here from those layouts, not copied.
// Little endian; "packed" where Broadcom packs the struct, natural alignment where it does not.
#ifndef AWDL_BCM_H
#define AWDL_BCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- firmware events (wl_event_msg_t.event_type) ---------------------------------------------------------------------
enum {
    WLC_E_IF = 54,
    WLC_E_AWDL_AW = 96,     // aw_start_t
    WLC_E_AWDL_ROLE = 97,   // awdl_sync_state_changed_t
    WLC_E_AWDL_EVENT = 98,  // generic
    WLC_E_AWDL_RX_PRB_RESP = 111,
    WLC_E_AWDL_RX_ACT_FRAME = 112,  // wl_event_rx_frame_data_t + the action frame
    WLC_E_AWDL_WOWL_NULLPKT = 113,
    WLC_E_AWDL_PHYCAL_STATUS = 114,
    WLC_E_AWDL_OOB_AF_STATUS = 115,
    WLC_E_AWDL_SCAN_STATUS = 116,
    WLC_E_AWDL_AW_START = 117,
    WLC_E_AWDL_AW_END = 118,
    WLC_E_AWDL_AW_EXT = 119,
    WLC_E_AWDL_PEER_CACHE_CONTROL = 120,
};
// The event mask bits the host must set in "event_msgs" for AWDL (plus WLC_E_IF, already set for en0).
extern const uint8_t bcm_awdl_events[];
extern const unsigned bcm_awdl_event_count;

// One firmware event as the radio side hands it over (the fields of wl_event_msg_t the stack uses).
struct awdl_fw_event {
    uint32_t type, status, reason;
    uint8_t addr[6];
    uint8_t ifidx, bsscfgidx;
};

// ---- iovar payloads ------------------------------------------------------------------------------------------------
// Sizes of the fixed parts.
enum {
    BCM_AWDL_IF2_LEN = 20,            // wl_awdl_if2_t {s32 cfg_idx; s32 up; ea bssid; ea if_addr}
    BCM_AWDL_CONFIG_LEN = 28,         // awdl_config_params_t (natural alignment)
    BCM_AWDL_AF_HDR_LEN = 10,         // awdl_af_hdr_t {ea dst; u8 action_hdr[4]}
    BCM_AWDL_CHAN_SEQ_FIXED = 6,      // awdl_channel_sequence_t (packed) before the channels
    BCM_AWDL_EXTCOUNT_LEN = 4,        // awdl_extcount_t
    BCM_AWDL_OPMODE_LEN = 12,         // awdl_opmode_t (packed)
    BCM_AWDL_ELECTION_TREE_LEN = 42,  // awdl_election_tree_info_t (packed)
    BCM_AWDL_PEER_OP_LEN = 9,         // awdl_peer_op_t {u8 version, opcode; ea addr; u8 mode}
    BCM_AWDL_PAYLOAD_FIXED = 4,       // awdl_payload_t {u32 len; u8 payload[]}
    BCM_AWDL_STATS_LEN = 76,          // awdl_stats_t (packed)
    BCM_AWDL_PEER_NODE_LEN = 60,      // awdl_peer_node_t (natural)
    BCM_RX_FRAME_DATA_LEN = 16,       // wl_event_rx_frame_data_t
};
enum { BCM_AWDL_PEER_OP_ADD = 0, BCM_AWDL_PEER_OP_DEL = 1, BCM_AWDL_PEER_OP_INFO = 2, BCM_AWDL_PEER_OP_UPD = 3 };
enum { BCM_AWDL_ROLE_SLAVE = 0, BCM_AWDL_ROLE_NE_MASTER = 1, BCM_AWDL_ROLE_MASTER = 2 };
enum { BCM_AWDL_OPMODE_AUTO = 0, BCM_AWDL_OPMODE_FIXED = 1 };
enum { BCM_AWDL_AFTXMODE_AUTO = 0, BCM_AWDL_AFTXMODE_SUPPRESS = 3, BCM_AWDL_AFTXMODE_SYNC_PREAW = 4 };

struct bcm_awdl_config {
    uint8_t channel, guard_time;
    uint16_t aw_period, aw_cmn_length, action_frame_period, pkt_lifetime_ms, max_no_master, ext_count, aw_ext_length;
    uint16_t nmode;   // 0 legacy, 1 11n rates only, 2 11n + A-MPDU
    uint8_t dest[6];  // where the firmware sends PSF/MIF (broadcast)
};
struct bcm_awdl_election_tree {
    uint8_t flags;
    uint16_t id;
    uint32_t self_metric;
    int8_t close_sync_rssi, master_rssi_boost, edge_sync_rssi, close_range_rssi, mid_range_rssi;
    uint8_t max_higher_masters_close, max_higher_masters_mid, max_tree_depth;
};

size_t bcm_awdl_if2(uint8_t *out, int32_t cfg_idx, int up, const uint8_t bssid[6], const uint8_t addr[6]);
size_t bcm_awdl_config_encode(uint8_t *out, const struct bcm_awdl_config *c);
size_t bcm_awdl_af_hdr(uint8_t *out, const uint8_t dest[6]);
// The 16-slot sequence in the firmware's form: length, encoding, duplicates, step count, fill channel, entries
// (operating-class encoding: 2 bytes per slot).
struct awdl_chan;
size_t bcm_awdl_chan_seq(uint8_t *out, size_t size, const struct awdl_chan *seq, unsigned slots);
size_t bcm_awdl_extcounts(uint8_t *out, uint8_t min_ext, uint8_t max_mcast, uint8_t max_ucast, uint8_t max_af);
size_t bcm_awdl_opmode(uint8_t *out, uint8_t mode, uint8_t role, uint16_t bcast_tu, const uint8_t master[6]);
size_t bcm_awdl_election_tree_encode(uint8_t *out, const struct bcm_awdl_election_tree *e);
size_t bcm_awdl_peer_op(uint8_t *out, uint8_t opcode, const uint8_t addr[6], uint8_t mode);
size_t bcm_awdl_payload(uint8_t *out, size_t size, const uint8_t *tlvs, size_t length);
size_t bcm_u32(uint8_t *out, uint32_t v);

// ---- event payloads ----------------------------------------------------------------------------------------------------
struct bcm_awdl_aw {  // WLC_E_AWDL_AW: aw_start_t {u8 role; ea master; u8 aw_seq_num}
    uint8_t role;
    uint8_t master[6];
    uint8_t aw_seq;
};
struct bcm_awdl_role {  // WLC_E_AWDL_ROLE: awdl_sync_state_changed_t {u8 new_role; ea master}
    uint8_t role;
    uint8_t master[6];
};
struct bcm_if_event {  // WLC_E_IF: wl_event_data_if_t
    uint8_t ifidx, opcode, flags, bssidx, role;
};
struct bcm_rx_frame {  // wl_event_rx_frame_data_t
    uint16_t version, chanspec;
    int32_t rssi;
    uint32_t mactime, rate;
};
int bcm_awdl_aw_decode(const uint8_t *data, size_t len, struct bcm_awdl_aw *out);
int bcm_awdl_role_decode(const uint8_t *data, size_t len, struct bcm_awdl_role *out);
int bcm_if_event_decode(const uint8_t *data, size_t len, struct bcm_if_event *out);
// WLC_E_AWDL_RX_ACT_FRAME: the frame after an optional wl_event_rx_frame_data_t header (told apart by the 802.11
// frame control of an action frame). *frame/*frame_len point into data. 0 or -1.
int bcm_awdl_rx_frame_decode(const uint8_t *data, size_t len, struct bcm_rx_frame *meta, const uint8_t **frame,
                             size_t *frame_len);
// The primary channel number of a Broadcom chanspec (bits 0-7).
static inline uint8_t bcm_chanspec_channel(uint16_t chanspec)
{
    return (uint8_t)(chanspec & 0xff);
}

#ifdef __cplusplus
}
#endif

#endif
