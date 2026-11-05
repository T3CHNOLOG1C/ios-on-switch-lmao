// SPDX-License-Identifier: BSD-2-Clause-Patent
// The radio services the AWDL stack uses, exactly as agreed in out/agent-scratch/r12-awdl/RADIO-API.md (r12-awdl owns
// the radio side and implements these in SwitchWLAN; r12-awdl-stack's SwitchWLANAwdl.cpp calls them). Until the radio
// side lands, SwitchWLANAwdl.cpp carries weak defaults that answer "unavailable", so AWDL stays off.
// Plain C types only. Every function: any thread, never blocks; completions come through the callbacks.
#ifndef SWITCH_WLAN_AWDL_RADIO_H
#define SWITCH_WLAN_AWDL_RADIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SWLAN_AWDL_ABI_VERSION 1

struct swlan_awdl_caps {
    uint32_t abi_version;        // SWLAN_AWDL_ABI_VERSION
    uint8_t can_monitor;         // rx of 802.11 frames with metadata (§2)
    uint8_t can_inject;          // tx of caller-built 802.11 frames (§3)
    uint8_t can_inject_timed;    // tx honours tx_tsf
    uint8_t can_scheduled_hops;  // runs a TSF-locked channel sequence (§4)
    uint8_t can_stay_assoc;      // hops while en0 stays associated (mchan); else time-shared
    uint8_t has_native_awdl;     // the firmware's own awdl iovars (§6)
    uint8_t _pad;
    uint32_t tsf_read_us_err;
    uint8_t self_mac[6];
    uint16_t max_inject_len;
};
// <0 (negative errno): AWDL unavailable; the stack says so to wifip2pd and does not retry.
int swlan_awdl_open(const struct swlan_awdl_caps **caps_out);
void swlan_awdl_close(void);

struct swlan_rx_meta {
    uint64_t tsf;  // local TSF at rx, us (0: not stamped)
    uint16_t freq_mhz;
    int8_t rssi_dbm;
    uint8_t flags;  // bit0 FCS_OK, bit1 FCS_PRESENT, bit2 AMPDU, bit3 SHORT_GI
    uint8_t mcs;
    uint8_t _pad[3];
};
enum { SWLAN_RX_FCS_OK = 1, SWLAN_RX_FCS_PRESENT = 2 };
typedef void (*swlan_rx_fn)(void *ctx, const uint8_t *frame, size_t len, const struct swlan_rx_meta *meta);
int swlan_awdl_rx_start(swlan_rx_fn fn, void *ctx);
int swlan_awdl_rx_stop(void);
int swlan_awdl_rx_filter(const uint8_t dst[6], const uint8_t bssid[6], uint16_t ethertype_or_0);

struct swlan_tx_req {
    const uint8_t *frame;  // whole 802.11 frame, no FCS
    size_t len;
    uint16_t freq_mhz;
    uint8_t rate_mcs;  // 0xff: firmware default
    uint8_t flags;     // bit0 NO_ACK, bit1 fail-if-off-channel
    uint64_t tx_tsf;   // 0: asap
};
enum { SWLAN_TX_NO_ACK = 1, SWLAN_TX_ON_CHANNEL_ONLY = 2 };
int swlan_awdl_tx(const struct swlan_tx_req *req);  // >= 0 request id, < 0 errno

struct swlan_chan_slot {
    uint16_t freq_mhz;
    uint16_t dwell_us;
};
struct swlan_chan_seq {
    uint64_t anchor_tsf;
    uint32_t period_us;
    uint16_t n_slots;
    uint16_t infra_freq_mhz;
    const struct swlan_chan_slot *slots;
};
int swlan_awdl_chanseq_set(const struct swlan_chan_seq *seq);
int swlan_awdl_chanseq_clear(void);

enum swlan_awdl_evt { SWLAN_AWDL_TXDONE, SWLAN_AWDL_SEQ_WRAP, SWLAN_AWDL_CHIP_LOST, SWLAN_AWDL_OFFCHAN_DROP };
struct swlan_awdl_event {
    uint32_t type;
    uint32_t id;
    uint16_t status;
    uint64_t tsf;
};
typedef void (*swlan_evt_fn)(void *ctx, const struct swlan_awdl_event *e);
int swlan_awdl_events(swlan_evt_fn fn, void *ctx);
int swlan_awdl_tsf_read(uint64_t *tsf_us);

// ---- §6 native AWDL: the firmware's own AWDL (cap "awdl", r7p8). PROPOSED by r12-awdl-stack (AWDL-NATIVE.md) --------
// Set or get one firmware iovar (bsscfg < 0: plain; >= 0: "bsscfg:<name>" with that index). Thread context only
// (never from a callback): waits for the firmware's answer, bounded like every SwitchWLAN ioctl. 0 or < 0 errno.
int swlan_awdl_iovar_set(int32_t bsscfg, const char *name, const void *buf, size_t len);
int swlan_awdl_iovar_get(int32_t bsscfg, const char *name, void *buf, size_t len, size_t *returned);
// The firmware events of an AWDL session (WLC_E_IF for the AWDL bsscfg and WLC_E 96-98, 111-120; awdl_bcm.h): set
// in "event_msgs" and delivered from the work loop, payload after the wl_event_msg_t.
struct swlan_fw_event {
    uint32_t type, status, reason;
    uint8_t addr[6];
    uint8_t ifidx, bsscfgidx;
};
typedef void (*swlan_fw_event_fn)(void *ctx, const struct swlan_fw_event *e, const uint8_t *data, size_t len);
int swlan_awdl_fw_events(swlan_fw_event_fn fn, void *ctx);
// Ethernet frames on the firmware's AWDL interface (the ifidx its WLC_E_IF ADD names; its own msgbuf flow ring).
int swlan_awdl_eth_tx(const uint8_t *eth, size_t len);  // 0 queued
typedef void (*swlan_eth_rx_fn)(void *ctx, const uint8_t *eth, size_t len);
int swlan_awdl_eth_rx(swlan_eth_rx_fn fn, void *ctx);

// ---- SwitchWLANAwdl.cpp (r12-awdl-stack) -> SwitchWLANNet.cpp ---------------------------------------------------
// Whether en0 advertises AWDL (CARD_CAPABILITIES bit 32): the Data flag /private/var/ioswitch/awdl-enable exists and
// the radio opened with what the stack needs. Evaluated once userland runs; cheap after that.
int switch_wlan_awdl_capable(void);
// en0's VIRTUAL_IF_CREATE (94) / VIRTUAL_IF_DELETE (95), the payload already copied in; create may write the new
// interface's name back into it (the caller copies it out). 0 or an errno.
int switch_wlan_awdl_vif_create(void *payload, size_t length);
int switch_wlan_awdl_vif_delete(const void *payload, size_t length);

#ifdef __cplusplus
}
#endif

#endif
