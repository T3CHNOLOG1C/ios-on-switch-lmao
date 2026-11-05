// SPDX-License-Identifier: BSD-2-Clause-Patent
// Byte-exact encoders and decoders for the Broadcom firmware's AWDL iovars and events (awdl_bcm.h).
#include "awdl_bcm.h"
#include "awdl_internal.h"

const uint8_t bcm_awdl_events[] = {
    WLC_E_AWDL_AW,           WLC_E_AWDL_ROLE,          WLC_E_AWDL_EVENT,         WLC_E_AWDL_RX_PRB_RESP,
    WLC_E_AWDL_RX_ACT_FRAME, WLC_E_AWDL_PHYCAL_STATUS, WLC_E_AWDL_OOB_AF_STATUS, WLC_E_AWDL_SCAN_STATUS,
    WLC_E_AWDL_AW_START,     WLC_E_AWDL_AW_END,        WLC_E_AWDL_AW_EXT,        WLC_E_AWDL_PEER_CACHE_CONTROL};
const unsigned bcm_awdl_event_count = sizeof bcm_awdl_events;

size_t bcm_u32(uint8_t *out, uint32_t v)
{
    awdl_put32(out, v);
    return 4;
}

size_t bcm_awdl_if2(uint8_t *out, int32_t cfg_idx, int up, const uint8_t bssid[6], const uint8_t addr[6])
{
    awdl_put32(out, (uint32_t)cfg_idx);
    awdl_put32(out + 4, up ? 1u : 0u);
    memcpy(out + 8, bssid, 6);
    memcpy(out + 14, addr, 6);
    return BCM_AWDL_IF2_LEN;
}

size_t bcm_awdl_config_encode(uint8_t *out, const struct bcm_awdl_config *c)
{
    memset(out, 0, BCM_AWDL_CONFIG_LEN);
    awdl_put32(out, 1);  // version
    out[4] = c->channel;
    out[5] = c->guard_time;
    awdl_put16(out + 6, c->aw_period);
    awdl_put16(out + 8, c->aw_cmn_length);
    awdl_put16(out + 10, c->action_frame_period);
    awdl_put16(out + 12, c->pkt_lifetime_ms);
    awdl_put16(out + 14, c->max_no_master);
    awdl_put16(out + 16, c->ext_count);
    awdl_put16(out + 18, c->aw_ext_length);
    awdl_put16(out + 20, c->nmode);
    memcpy(out + 22, c->dest, 6);
    return BCM_AWDL_CONFIG_LEN;
}

size_t bcm_awdl_af_hdr(uint8_t *out, const uint8_t dest[6])
{
    memcpy(out, dest, 6);
    out[6] = 0x7f;  // vendor specific action, then Apple's OUI
    memcpy(out + 7, awdl_oui, 3);
    return BCM_AWDL_AF_HDR_LEN;
}

size_t bcm_awdl_chan_seq(uint8_t *out, size_t size, const struct awdl_chan *seq, unsigned slots)
{
    const size_t n = BCM_AWDL_CHAN_SEQ_FIXED + 2u * slots;
    if (!slots || slots > AWDL_SEQ_SLOTS || n > size) return 0;
    out[0] = (uint8_t)(slots - 1);  // as on the air: entries - 1
    out[1] = AWDL_CHAN_ENC_OPCLASS;
    out[2] = 0;
    out[3] = 3;
    awdl_put16(out + 4, 0xffff);
    for (unsigned i = 0; i < slots; ++i) {
        out[6 + 2 * i] = seq[i].number;
        out[7 + 2 * i] = seq[i].opclass;
    }
    return n;
}

size_t bcm_awdl_extcounts(uint8_t *out, uint8_t min_ext, uint8_t max_mcast, uint8_t max_ucast, uint8_t max_af)
{
    out[0] = min_ext;
    out[1] = max_mcast;
    out[2] = max_ucast;
    out[3] = max_af;
    return BCM_AWDL_EXTCOUNT_LEN;
}

size_t bcm_awdl_opmode(uint8_t *out, uint8_t mode, uint8_t role, uint16_t bcast_tu, const uint8_t master[6])
{
    memset(out, 0, BCM_AWDL_OPMODE_LEN);
    out[0] = mode;
    out[1] = role;
    awdl_put16(out + 2, bcast_tu);
    if (master) memcpy(out + 4, master, 6);
    return BCM_AWDL_OPMODE_LEN;  // cur_bcast_tu (+10) is read only
}

size_t bcm_awdl_election_tree_encode(uint8_t *out, const struct bcm_awdl_election_tree *e)
{
    memset(out, 0, BCM_AWDL_ELECTION_TREE_LEN);
    out[0] = e->flags;
    awdl_put16(out + 1, e->id);
    awdl_put32(out + 3, e->self_metric);
    out[7] = (uint8_t)e->close_sync_rssi;
    out[8] = (uint8_t)e->master_rssi_boost;
    out[9] = (uint8_t)e->edge_sync_rssi;
    out[10] = (uint8_t)e->close_range_rssi;
    out[11] = (uint8_t)e->mid_range_rssi;
    out[12] = e->max_higher_masters_close;
    out[13] = e->max_higher_masters_mid;
    out[14] = e->max_tree_depth;
    return BCM_AWDL_ELECTION_TREE_LEN;  // +15.. top master, depth, private election: read only
}

size_t bcm_awdl_peer_op(uint8_t *out, uint8_t opcode, const uint8_t addr[6], uint8_t mode)
{
    out[0] = 0;  // AWDL_PEER_OP_CUR_VER
    out[1] = opcode;
    memcpy(out + 2, addr, 6);
    out[8] = mode;
    return BCM_AWDL_PEER_OP_LEN;
}

size_t bcm_awdl_payload(uint8_t *out, size_t size, const uint8_t *tlvs, size_t length)
{
    if (BCM_AWDL_PAYLOAD_FIXED + length > size) return 0;
    awdl_put32(out, (uint32_t)length);
    memcpy(out + 4, tlvs, length);
    return BCM_AWDL_PAYLOAD_FIXED + length;
}

int bcm_awdl_aw_decode(const uint8_t *d, size_t len, struct bcm_awdl_aw *o)
{
    if (len < 8) return -1;
    o->role = d[0];
    memcpy(o->master, d + 1, 6);
    o->aw_seq = d[7];
    return 0;
}

int bcm_awdl_role_decode(const uint8_t *d, size_t len, struct bcm_awdl_role *o)
{
    if (len < 7) return -1;
    o->role = d[0];
    memcpy(o->master, d + 1, 6);
    return 0;
}

int bcm_if_event_decode(const uint8_t *d, size_t len, struct bcm_if_event *o)
{
    if (len < 5) return -1;
    o->ifidx = d[0];
    o->opcode = d[1];
    o->flags = d[2];
    o->bssidx = d[3];
    o->role = d[4];
    return 0;
}

static int is_awdl_action(const uint8_t *f, size_t len)
{
    return len >= 24 + 5 && (f[0] & 0xfc) == 0xd0 && f[24] == 0x7f && !memcmp(f + 25, awdl_oui, 3);
}

int bcm_awdl_rx_frame_decode(const uint8_t *d, size_t len, struct bcm_rx_frame *meta, const uint8_t **frame,
                             size_t *frame_len)
{
    memset(meta, 0, sizeof *meta);
    if (len > BCM_RX_FRAME_DATA_LEN && is_awdl_action(d + BCM_RX_FRAME_DATA_LEN, len - BCM_RX_FRAME_DATA_LEN)) {
        meta->version = (uint16_t)(d[0] | (d[1] << 8));
        meta->chanspec = (uint16_t)(d[2] | (d[3] << 8));
        meta->rssi =
            (int32_t)((uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24));
        meta->mactime = (uint32_t)d[8] | ((uint32_t)d[9] << 8) | ((uint32_t)d[10] << 16) | ((uint32_t)d[11] << 24);
        meta->rate = (uint32_t)d[12] | ((uint32_t)d[13] << 8) | ((uint32_t)d[14] << 16) | ((uint32_t)d[15] << 24);
        *frame = d + BCM_RX_FRAME_DATA_LEN;
        *frame_len = len - BCM_RX_FRAME_DATA_LEN;
        return 0;
    }
    if (is_awdl_action(d, len)) {  // no header: the frame alone
        *frame = d;
        *frame_len = len;
        return 0;
    }
    return -1;
}
