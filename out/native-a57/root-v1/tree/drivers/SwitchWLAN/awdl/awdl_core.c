// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AWDL core: timing model (AW / extended AW / channel sequence on the local TSF), synchronisation to the parent,
// master election, the peer table, PSF/MIF transmission and the data path's queue. See awdl.h.
//
// Timing model (Stute et al. §4.2): AWs of 16 TU are numbered by a 16-bit sequence counter; 4 AWs make one slot of
// the 16-slot channel sequence (1024 TU). base_tsf is the local TSF at which AW number base_seq starts; it moves only
// when we synchronise to a parent. Senders put in their sync parameters the TU left until their next AW starts
// ("down counter") and their current AW number, at a target transmit time aligned to a whole TU before that start, and
// the time they meant to send (target_tx_time) and actually sent (phy_tx_time). A receiver places the sender's next AW
// at rx_tsf - (phy_tx_time - target_tx_time) + down_counter TU.
#include "awdl_internal.h"
#include "awdl_bcm.h"

void awdl_config_default(struct awdl_config *c, const uint8_t addr[6])
{
    memset(c, 0, sizeof *c);
    memcpy(c->addr, addr, 6);
    memcpy(c->hostname, "switch", 7);
    // One AWDL channel all the way round: 149 in its 80 MHz block (operating class 128, centre 155), the social
    // channel iPhones use in most regions. The integration may replace it (time-sharing with en0's channel).
    for (unsigned i = 0; i < AWDL_SEQ_SLOTS; ++i) c->seq[i] = (struct awdl_chan){155, 0x80};
    c->self_metric = 60;
    c->awdl_version = 0x34;
    c->device_class = 2;
    c->country[0] = 'X';
    c->country[1] = 'X';
    c->backend = AWDL_BACKEND_NATIVE;  // this chip's firmware has AWDL (cap "awdl", r7p8)
    c->bsscfg = 1;
}

size_t awdl_core_size(void)
{
    return sizeof(struct awdl_core);
}

void awdl_log(struct awdl_core *c, const char *fmt, ...)
{
    if (!c->host.log) return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    c->host.log(c->host.ctx, line);
}

static int is_group(const uint8_t *a)
{
    return a[0] & 1;
}
static int addr_eq(const uint8_t *a, const uint8_t *b)
{
    return !memcmp(a, b, 6);
}
static int native(const struct awdl_core *c)
{
    return c->cfg.backend == AWDL_BACKEND_NATIVE;
}
// Native backend: one iovar to the firmware, through the integration's ordered queue.
static void fw_set(struct awdl_core *c, const char *name, const void *buf, size_t len)
{
    c->stats.iovars++;
    if (!c->host.iovar || c->host.iovar(c->host.ctx, -1, name, buf, len)) {
        c->stats.iovar_refused++;
        awdl_log(c, "AWDL: iovar %s not queued", name);
    }
}
static void fw_peer_op(struct awdl_core *c, uint8_t op, const uint8_t addr[6])
{
    uint8_t b[BCM_AWDL_PEER_OP_LEN];
    fw_set(c, "awdl_peer_op", b, bcm_awdl_peer_op(b, op, addr, 0));
    if (op == BCM_AWDL_PEER_OP_ADD)
        c->stats.peer_adds++;
    else
        c->stats.peer_dels++;
}

// ---- timing ------------------------------------------------------------------------------------------------------
uint16_t awdl_core_aw_seq(const struct awdl_core *c, uint64_t now)
{
    if (now >= c->base_tsf) return (uint16_t)(c->base_seq + (now - c->base_tsf) / AWDL_AW_US);
    const uint64_t back = c->base_tsf - now;
    return (uint16_t)(c->base_seq - (back + AWDL_AW_US - 1) / AWDL_AW_US);
}
uint64_t awdl_core_aw_start(const struct awdl_core *c, uint16_t seq, uint64_t now)
{
    (void)now;
    const int64_t d = (int16_t)(uint16_t)(seq - c->base_seq);
    return (uint64_t)((int64_t)c->base_tsf + d * (int64_t)AWDL_AW_US);
}
static unsigned slot_of(uint16_t aw)
{
    return (unsigned)(aw / AWDL_EAW_AWS) % AWDL_SEQ_SLOTS;
}
struct awdl_chan awdl_core_channel_at(const struct awdl_core *c, uint64_t now)
{
    return c->cfg.seq[slot_of(awdl_core_aw_seq(c, now))];
}
// Keep the base near `now` so 16-bit differences never wrap (it moves by whole AWs: the schedule is unchanged).
static void rebase(struct awdl_core *c, uint64_t now)
{
    const uint16_t aw = awdl_core_aw_seq(c, now);
    c->base_tsf = awdl_core_aw_start(c, aw, now);
    c->base_seq = aw;
}
// Start of the next slot (extended AW) after `now`, and the TSF where slot 0 of the sequence containing `now` began.
static uint64_t next_slot_start(const struct awdl_core *c, uint64_t now)
{
    const uint16_t aw = awdl_core_aw_seq(c, now);
    const uint16_t first = (uint16_t)((aw / AWDL_EAW_AWS + 1) * AWDL_EAW_AWS);
    return awdl_core_aw_start(c, first, now);
}
static uint64_t sequence_anchor(const struct awdl_core *c, uint64_t now)
{
    const uint16_t aw = awdl_core_aw_seq(c, now);
    const uint16_t first = (uint16_t)(aw - aw % (AWDL_EAW_AWS * AWDL_SEQ_SLOTS));
    return awdl_core_aw_start(c, first, now);
}
static void install_schedule(struct awdl_core *c, uint64_t now)
{
    c->anchor = sequence_anchor(c, now);
    if (c->host.schedule) c->host.schedule(c->host.ctx, c->cfg.seq, c->anchor);
}

// ---- election -------------------------------------------------------------------------------------------------------
struct candidate {
    const uint8_t *root, *parent;
    uint32_t metric, counter, hops;
};
static int better(const struct candidate *a, const struct candidate *b)
{
    if (a->metric != b->metric) return a->metric > b->metric;
    if (a->counter != b->counter) return a->counter > b->counter;
    const int cmp = memcmp(a->root, b->root, 6);
    if (cmp) return cmp > 0;
    return a->hops < b->hops;  // same root: the shorter path
}
static void run_election(struct awdl_core *c, uint64_t now)
{
    const uint8_t *self = c->cfg.addr;
    struct candidate best = {self, self, c->cfg.self_metric, c->self_counter, 0};
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        const struct awdl_peer *p = &c->peers[i];
        if (!p->used || !p->has_v2 || now - p->last_seen > AWDL_ELECTION_FRESH_US) continue;
        const struct awdl_election_v2 *e = &p->election;
        if (addr_eq(e->root, self) || addr_eq(e->parent, self)) continue;  // our own subtree: no loops
        if (e->hops + 1 > AWDL_ELECTION_MAX_HOPS) continue;
        struct candidate k = {e->root, p->addr, e->root_metric, e->root_counter, e->hops + 1};
        if (better(&k, &best)) best = k;
    }
    const enum awdl_role was = c->role;
    uint8_t old_parent[6], old_root[6];
    memcpy(old_parent, c->adv.parent, 6);
    memcpy(old_root, c->adv.root, 6);
    c->role = best.root == self ? AWDL_ROLE_ROOT : AWDL_ROLE_FOLLOWER;
    memcpy(c->adv.root, best.root, 6);
    memcpy(c->adv.parent, best.parent, 6);
    c->adv.root_counter = best.counter;
    c->adv.hops = best.hops;
    c->adv.root_metric = best.metric;
    c->adv.self_metric = c->cfg.self_metric;
    c->adv.self_counter = c->self_counter;
    if (was != c->role || !addr_eq(old_parent, c->adv.parent) || !addr_eq(old_root, c->adv.root))
        awdl_log(c, "AWDL: %s, root %02x:%02x:%02x:%02x:%02x:%02x metric %u, %u hops",
                 c->role == AWDL_ROLE_ROOT ? "root" : "follower", best.root[0], best.root[1], best.root[2],
                 best.root[3], best.root[4], best.root[5], best.metric, best.hops);
}

// ---- peers ----------------------------------------------------------------------------------------------------------
const struct awdl_peer *awdl_core_find(const struct awdl_core *c, const uint8_t addr[6])
{
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i)
        if (c->peers[i].used && addr_eq(c->peers[i].addr, addr)) return &c->peers[i];
    return NULL;
}
static struct awdl_peer *peer_get(struct awdl_core *c, const uint8_t addr[6], uint64_t now)
{
    struct awdl_peer *free_slot = NULL, *oldest = NULL;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i) {
        struct awdl_peer *p = &c->peers[i];
        if (p->used && addr_eq(p->addr, addr)) return p;
        if (!p->used && !free_slot) free_slot = p;
        if (p->used && !p->reachable && (!oldest || p->last_seen < oldest->last_seen)) oldest = p;
    }
    struct awdl_peer *p = free_slot ? free_slot : oldest;  // a full table recycles the stalest unreachable peer
    if (!p) return NULL;
    memset(p, 0, sizeof *p);
    p->used = 1;
    memcpy(p->addr, addr, 6);
    p->first_seen = now;
    return p;
}
static void peer_lost(struct awdl_core *c, struct awdl_peer *p, const char *why)
{
    if (p->reachable && native(c)) fw_peer_op(c, BCM_AWDL_PEER_OP_DEL, p->addr);
    if (p->reachable && c->host.peer) c->host.peer(c->host.ctx, p->addr, 0, p->rssi);
    awdl_log(c, "AWDL: peer %02x:%02x:%02x:%02x:%02x:%02x lost (%s)", p->addr[0], p->addr[1], p->addr[2], p->addr[3],
             p->addr[4], p->addr[5], why);
    memset(p, 0, sizeof *p);
}
static void expire_peers(struct awdl_core *c, uint64_t now)
{
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i)
        if (c->peers[i].used && now - c->peers[i].last_seen > AWDL_PEER_TIMEOUT_US)
            peer_lost(c, &c->peers[i], "timeout");
}
const struct awdl_peer *awdl_core_peer(const struct awdl_core *c, unsigned i)
{
    return i < AWDL_MAX_PEERS && c->peers[i].used ? &c->peers[i] : NULL;
}

// The sender's next AW start in our clock, from its sync parameters as received at rx_tsf.
static uint64_t sender_next_aw(const struct awdl_af *a, uint64_t rx_tsf)
{
    int32_t delay = (int32_t)(a->phy_tx_time - a->target_tx_time);
    if (delay < -100000 || delay > 100000) delay = 0;  // nonsense: ignore it
    return (uint64_t)((int64_t)rx_tsf - delay) + (uint64_t)a->sync.tx_counter * AWDL_TU_US;
}

static void sync_to(struct awdl_core *c, const struct awdl_peer *p, uint64_t now)
{
    const uint64_t predicted = awdl_core_aw_start(c, p->next_aw_seq, now);
    const int64_t err = (int64_t)(p->next_aw_tsf - predicted);
    c->base_tsf = p->next_aw_tsf;
    c->base_seq = p->next_aw_seq;
    c->stats.last_sync_error_us = (int32_t)(err > INT32_MAX ? INT32_MAX : err < INT32_MIN ? INT32_MIN : err);
    if (err > AWDL_RESYNC_US || err < -AWDL_RESYNC_US) {
        c->stats.resyncs++;
        install_schedule(c, now);
        c->next_slot = next_slot_start(c, now);
        if (c->next_psf > now + AWDL_PSF_US) c->next_psf = now + AWDL_PSF_US;
    }
}

static void handle_af(struct awdl_core *c, const struct awdl_af *a, const struct awdl_rx_meta *m)
{
    if (addr_eq(a->src, c->cfg.addr) || is_group(a->src)) return;
    struct awdl_peer *p = peer_get(c, a->src, m->tsf);
    if (!p) return;
    p->last_seen = m->tsf;
    p->rssi = p->frames ? (int8_t)((3 * p->rssi + m->rssi) / 4) : m->rssi;
    p->frames++;
    if (a->present & (1u << AWDL_TLV_ELECTION_PARAMS_V2)) {
        p->election = a->election2;
        p->has_v2 = 1;
    } else if (a->present & (1u << AWDL_TLV_ELECTION_PARAMS)) {  // a v1-only peer: its master is the root
        memset(&p->election, 0, sizeof p->election);
        memcpy(p->election.root, a->election.master, 6);
        memcpy(p->election.parent, a->src, 6);
        p->election.hops = a->election.distance;
        p->election.root_metric = a->election.master_metric;
        p->election.self_metric = a->election.self_metric;
        p->has_v2 = 1;
    }
    if (a->present & (1u << AWDL_TLV_CHANNEL_SEQUENCE)) {
        p->seq = a->chanseq;
        p->has_seq = 1;
    } else if ((a->present & (1u << AWDL_TLV_SYNC_PARAMS)) && a->sync.has_seq) {
        p->seq = a->sync.seq;
        p->has_seq = 1;
    }
    if (a->present & (1u << AWDL_TLV_VERSION)) {
        p->awdl_version = a->awdl_version;
        p->device_class = a->device_class;
    }
    if ((a->present & (1u << AWDL_TLV_ARPA)) && a->hostname[0]) memcpy(p->hostname, a->hostname, sizeof p->hostname);
    const int timed = (a->present & (1u << AWDL_TLV_SYNC_PARAMS)) != 0;
    if (timed) {
        p->sync = a->sync;
        p->next_aw_tsf = sender_next_aw(a, m->tsf);
        p->next_aw_seq = (uint16_t)(a->sync.aw_seq + 1);
    }
    if (!native(c)) {  // the firmware does election and synchronisation in the native backend
        run_election(c, m->tsf);
        if (timed && c->role == AWDL_ROLE_FOLLOWER && addr_eq(p->addr, c->adv.parent)) sync_to(c, p, m->tsf);
    }
    if (!p->reachable && timed && p->has_seq &&
        (a->subtype == AWDL_SUBTYPE_MIF || (a->present & (1u << AWDL_TLV_DATA_PATH_STATE)))) {
        p->reachable = 1;
        awdl_log(c, "AWDL: peer %02x:%02x:%02x:%02x:%02x:%02x \"%s\" reachable, %d dBm, version %#x class %u",
                 p->addr[0], p->addr[1], p->addr[2], p->addr[3], p->addr[4], p->addr[5], p->hostname, p->rssi,
                 p->awdl_version, p->device_class);
        if (native(c)) fw_peer_op(c, BCM_AWDL_PEER_OP_ADD, p->addr);  // the firmware sends data to it from now on
        if (c->host.peer) c->host.peer(c->host.ctx, p->addr, 1, p->rssi);
    }
}

// ---- frames we send ------------------------------------------------------------------------------------------------
// Service params, and for a MIF HT caps, data path state, arpa and version: what our frames carry beyond sync and
// election (in the native backend, the "awdl_payload" the firmware appends).
static void write_extras(struct awdl_core *c, struct awdl_writer *w, int mif)
{
    awdl_w_begin(w, AWDL_TLV_SERVICE_PARAMS);
    awdl_w_u8(w, 0);
    awdl_w_u16(w, 0);
    awdl_w_u16(w, c->service_update_indicator);
    awdl_w_u8(w, 0);  // no SSTH values
    awdl_w_end(w);
    if (!mif) return;

    awdl_w_begin(w, AWDL_TLV_HT_CAPS);
    awdl_w_u16(w, 0);
    awdl_w_u16(w, 0x11ce);  // HT20/40, SM power save off, short GI 20, greenfield off
    awdl_w_u8(w, 0x17);     // A-MPDU: 64 KiB, 8 us spacing
    awdl_w_u8(w, 0xff);     // MCS 0-7
    awdl_w_u16(w, 0);
    awdl_w_end(w);

    awdl_w_begin(w, AWDL_TLV_DATA_PATH_STATE);
    awdl_w_u16(w, AWDL_DPS_COUNTRY_CODE | AWDL_DPS_SOCIAL_CHANNEL_MAP | AWDL_DPS_AWDL_ADDRESS);
    awdl_w_u8(w, (uint8_t)c->cfg.country[0]);
    awdl_w_u8(w, (uint8_t)c->cfg.country[1]);
    awdl_w_u8(w, 0);
    awdl_w_u16(w, 0x0004);  // social channels: 149
    awdl_w_bytes(w, c->cfg.addr, 6);
    awdl_w_end(w);

    awdl_w_begin(w, AWDL_TLV_ARPA);
    awdl_w_u8(w, 3);
    const size_t n = strnlen(c->cfg.hostname, sizeof c->cfg.hostname);
    awdl_w_u8(w, (uint8_t)n);
    awdl_w_bytes(w, c->cfg.hostname, n);
    awdl_w_u8(w, 0xc0);  // ".local"
    awdl_w_u8(w, 0x0c);
    awdl_w_end(w);

    awdl_w_begin(w, AWDL_TLV_VERSION);
    awdl_w_u8(w, c->cfg.awdl_version);
    awdl_w_u8(w, c->cfg.device_class);
    awdl_w_end(w);
}

size_t awdl_core_build_payload(struct awdl_core *c, uint8_t *buf, size_t size)
{
    struct awdl_writer w = {buf, size, 0, 0, 0};
    write_extras(c, &w, 1);
    return w.overflow ? 0 : w.at;
}

// A frame built at `now` is aimed at a whole number of TU before the next AW starts, at or after now: *counter gets
// that number (the sync parameters' down counter).
static uint64_t af_target(const struct awdl_core *c, uint64_t now, uint16_t *counter)
{
    const uint16_t aw = awdl_core_aw_seq(c, now);
    const uint64_t next = awdl_core_aw_start(c, (uint16_t)(aw + 1), now);
    const uint16_t n = (uint16_t)((next - now) / AWDL_TU_US);
    if (counter) *counter = n;
    return next - (uint64_t)n * AWDL_TU_US;
}

size_t awdl_core_build_af(struct awdl_core *c, uint8_t subtype, uint64_t now, uint8_t *buf, size_t size)
{
    uint16_t counter;
    const uint64_t target = af_target(c, now, &counter);
    const uint16_t aw_at_target = awdl_core_aw_seq(c, target);
    const struct awdl_chan ch = awdl_core_channel_at(c, target);
    const uint8_t *sync_master = c->role == AWDL_ROLE_FOLLOWER ? c->adv.parent : c->cfg.addr;

    struct awdl_writer w = {buf, size, 0, 0, 0};
    uint8_t hdr[24] = {0xd0, 0x00};
    memset(hdr + 4, 0xff, 6);
    memcpy(hdr + 10, c->cfg.addr, 6);
    memcpy(hdr + 16, awdl_bssid, 6);
    awdl_put16(hdr + 22, (uint16_t)((c->mgmt_seq++ & 0xfff) << 4));
    awdl_w_bytes(&w, hdr, 24);
    const uint8_t fixed[8] = {0x7f, awdl_oui[0], awdl_oui[1], awdl_oui[2], 0x08, 0x10, subtype, 0};
    awdl_w_bytes(&w, fixed, 8);
    awdl_w_u32(&w, (uint32_t)target);  // phy_tx_time: the radio sends at tx_tsf = target
    awdl_w_u32(&w, (uint32_t)target);  // target_tx_time

    awdl_w_begin(&w, AWDL_TLV_SYNC_PARAMS);
    awdl_w_u8(&w, awdl_chan_primary(ch));
    awdl_w_u16(&w, counter);
    awdl_w_u8(&w, awdl_chan_primary(ch));
    awdl_w_u8(&w, 0);                      // guard time
    awdl_w_u16(&w, AWDL_AW_TU);            // AW period
    awdl_w_u16(&w, AWDL_PSF_INTERVAL_TU);  // action frame period
    awdl_w_u16(&w, 0x1800);                // flags
    awdl_w_u16(&w, AWDL_AW_TU);            // AW extension length
    awdl_w_u16(&w, AWDL_AW_TU);            // AW common length
    awdl_w_u16(&w, 0);                     // remaining AW length
    awdl_w_u8(&w, 3);                      // minimum extensions
    awdl_w_u8(&w, 3);                      // maximum multicast extensions
    awdl_w_u8(&w, 3);                      // maximum unicast extensions
    awdl_w_u8(&w, 3);                      // maximum action frame extensions
    awdl_w_bytes(&w, sync_master, 6);
    awdl_w_u8(&w, 4);  // presence mode
    awdl_w_u8(&w, 0);
    awdl_w_u16(&w, aw_at_target);
    awdl_w_u16(&w, 0);  // AP beacon alignment delta
    awdl_w_chanseq(&w, c->cfg.seq);
    awdl_w_u16(&w, 0);  // padding
    awdl_w_end(&w);

    awdl_w_begin(&w, AWDL_TLV_ELECTION_PARAMS);
    awdl_w_u8(&w, 0);
    awdl_w_u16(&w, 0);
    awdl_w_u8(&w, (uint8_t)c->adv.hops);
    awdl_w_u8(&w, 0);
    awdl_w_bytes(&w, c->adv.root, 6);
    awdl_w_u32(&w, c->adv.root_metric);
    awdl_w_u32(&w, c->adv.self_metric);
    awdl_w_u16(&w, 0);  // padding
    awdl_w_end(&w);

    awdl_w_begin(&w, AWDL_TLV_CHANNEL_SEQUENCE);
    awdl_w_chanseq(&w, c->cfg.seq);
    awdl_w_u8(&w, 0);  // padding
    awdl_w_u8(&w, 0);
    awdl_w_u8(&w, 0);
    awdl_w_end(&w);

    awdl_w_begin(&w, AWDL_TLV_ELECTION_PARAMS_V2);
    awdl_w_bytes(&w, c->adv.root, 6);
    awdl_w_bytes(&w, c->adv.parent, 6);
    awdl_w_u32(&w, c->adv.root_counter);
    awdl_w_u32(&w, c->adv.hops);
    awdl_w_u32(&w, c->adv.root_metric);
    awdl_w_u32(&w, c->adv.self_metric);
    awdl_w_u32(&w, 0);
    awdl_w_u32(&w, 0);
    awdl_w_u32(&w, c->self_counter);
    awdl_w_end(&w);

    write_extras(c, &w, subtype == AWDL_SUBTYPE_MIF);
    return w.overflow ? 0 : w.at;
}

static void send_af(struct awdl_core *c, uint8_t subtype, uint64_t now)
{
    const size_t n = awdl_core_build_af(c, subtype, now, c->scratch, sizeof c->scratch);
    if (!n) return;
    const uint64_t at = af_target(c, now, NULL);
    const uint16_t freq = awdl_chan_freq(awdl_core_channel_at(c, at));
    if (!freq) return;  // an empty slot: we are not on an AWDL channel then
    if (c->host.tx(c->host.ctx, c->scratch, n, freq, at, 1)) {
        c->stats.tx_refused++;
        return;
    }
    c->stats.af_tx++;
    if (subtype == AWDL_SUBTYPE_PSF)
        c->stats.psf_tx++;
    else
        c->stats.mif_tx++;
}

// ---- data path --------------------------------------------------------------------------------------------------------
// Is `dst` (a peer, or any group address) listening on our channel in the slot containing `t`?
static int listening(const struct awdl_core *c, const uint8_t *dst, uint64_t t)
{
    const struct awdl_chan ours = awdl_core_channel_at(c, t);
    if (!awdl_chan_freq(ours)) return 0;
    if (is_group(dst)) return 1;
    const struct awdl_peer *p = awdl_core_find(c, dst);
    if (!p || !p->reachable) return 0;
    if (!p->has_seq || !p->seq.count) return 1;
    const unsigned slot = slot_of(awdl_core_aw_seq(c, t)) % p->seq.count;
    return awdl_chan_primary(p->seq.chan[slot]) == awdl_chan_primary(ours);
}

static void flush(struct awdl_core *c, uint64_t now)
{
    unsigned kept = 0;
    const unsigned n = c->txq_count;
    for (unsigned i = 0; i < n; ++i) {
        struct awdl_txq_entry *e = &c->txq[(c->txq_head + i) % AWDL_TXQ_FRAMES];
        if (listening(c, e->dst, now)) {
            const uint16_t freq = awdl_chan_freq(awdl_core_channel_at(c, now));
            if (c->host.tx(c->host.ctx, e->frame, e->len, freq, 0, e->group))
                c->stats.tx_refused++;
            else
                c->stats.data_tx++;
            continue;
        }
        if (!e->group && !awdl_core_find(c, e->dst)) {
            c->stats.data_drop++;  // the peer went away
            continue;
        }
        // still waiting: compact it to the front of the kept run
        struct awdl_txq_entry *k = &c->txq[(c->txq_head + kept) % AWDL_TXQ_FRAMES];
        if (k != e) *k = *e;
        kept++;
    }
    c->txq_count = kept;
}

int awdl_core_output(struct awdl_core *c, const uint8_t *eth, size_t len, uint64_t now)
{
    if (!c->running || len < 14 || len > AWDL_MAX_FRAME - 26) {
        c->stats.data_drop++;
        return -1;
    }
    const int group = is_group(eth);
    const struct awdl_peer *p = group ? NULL : awdl_core_find(c, eth);
    if (!group && (!p || !p->reachable)) {
        c->stats.data_drop++;  // no such peer: no route (the kernel only knows peers we announced)
        return -2;
    }
    if (native(c)) {  // the firmware's AWDL interface encapsulates and schedules
        if (!c->host.tx_eth || c->host.tx_eth(c->host.ctx, eth, len)) {
            c->stats.tx_refused++;
            return -3;
        }
        c->stats.data_tx++;
        return 0;
    }
    const uint16_t seq = c->data_seq++;
    if (listening(c, eth, now) && !c->txq_count) {
        const size_t n = awdl_data_encap(eth, len, c->cfg.addr, seq, c->scratch, sizeof c->scratch);
        const uint16_t freq = awdl_chan_freq(awdl_core_channel_at(c, now));
        if (!n || c->host.tx(c->host.ctx, c->scratch, n, freq, 0, group)) {
            c->stats.tx_refused++;
            return -3;
        }
        c->stats.data_tx++;
        return 0;
    }
    if (c->txq_count == AWDL_TXQ_FRAMES) {
        c->stats.data_drop++;
        return -4;
    }
    struct awdl_txq_entry *e = &c->txq[(c->txq_head + c->txq_count) % AWDL_TXQ_FRAMES];
    const size_t n = awdl_data_encap(eth, len, c->cfg.addr, seq, e->frame, sizeof e->frame);
    if (!n) return -5;
    e->len = (uint16_t)n;
    e->group = (uint8_t)group;
    memcpy(e->dst, eth, 6);
    c->txq_count++;
    return 0;
}

static void handle_data(struct awdl_core *c, const uint8_t *f, size_t len, const struct awdl_rx_meta *m)
{
    uint16_t seq = 0;
    int retry = 0;
    const size_t n = awdl_data_decap(f, len, c->scratch, sizeof c->scratch, &seq, &retry);
    if (!n) {
        c->stats.data_bad++;
        return;
    }
    if (!is_group(c->scratch) && !addr_eq(c->scratch, c->cfg.addr)) return;  // someone else's
    struct awdl_peer *p = NULL;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i)
        if (c->peers[i].used && addr_eq(c->peers[i].addr, c->scratch + 6)) p = &c->peers[i];
    if (p) {
        p->last_seen = m->tsf;
        if (retry && p->has_data_seq && p->last_data_seq == seq) return;  // a retransmission we already took
        p->last_data_seq = seq;
        p->has_data_seq = 1;
    }
    c->stats.data_rx++;
    if (c->host.deliver) c->host.deliver(c->host.ctx, c->scratch, n);
}

// ---- entry points -------------------------------------------------------------------------------------------------------
int awdl_core_init(struct awdl_core *c, const struct awdl_host *host, const struct awdl_config *cfg)
{
    if (!host || !host->tx) return -1;
    memset(c, 0, sizeof *c);
    c->host = *host;
    c->cfg = *cfg;
    c->role = AWDL_ROLE_OFF;
    return 0;
}

// Native bring-up, in the order a Mac's driver sends it (AWDL-NATIVE.md): the AWDL bsscfg up with our address, then
// its parameters, the frame header, the channel sequence, extension counts, election, our TLV payload, and the
// operating mode (auto: the firmware elects).
static void native_start(struct awdl_core *c)
{
    uint8_t b[512];
    fw_set(c, "awdl_if", b, bcm_awdl_if2(b, c->cfg.bsscfg, 1, awdl_bssid, c->cfg.addr));
    struct bcm_awdl_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.channel = awdl_chan_primary(c->cfg.seq[0]);
    cfg.aw_period = AWDL_AW_TU;
    cfg.aw_cmn_length = AWDL_AW_TU;
    cfg.action_frame_period = AWDL_PSF_INTERVAL_TU;
    cfg.pkt_lifetime_ms = 500;
    cfg.max_no_master = 8;
    cfg.ext_count = 3;
    cfg.aw_ext_length = AWDL_AW_TU;
    cfg.nmode = 2;  // 11n with A-MPDU
    memset(cfg.dest, 0xff, 6);
    fw_set(c, "awdl_config", b, bcm_awdl_config_encode(b, &cfg));
    fw_set(c, "awdl_af_hdr", b, bcm_awdl_af_hdr(b, cfg.dest));
    fw_set(c, "awdl_chan_seq", b, bcm_awdl_chan_seq(b, sizeof b, c->cfg.seq, AWDL_SEQ_SLOTS));
    fw_set(c, "awdl_extcounts", b, bcm_awdl_extcounts(b, 3, 3, 3, 3));
    struct bcm_awdl_election_tree e;
    memset(&e, 0, sizeof e);
    e.self_metric = c->cfg.self_metric;
    e.close_sync_rssi = -65;
    e.edge_sync_rssi = -80;
    e.close_range_rssi = -60;
    e.mid_range_rssi = -75;
    e.max_tree_depth = AWDL_ELECTION_MAX_HOPS;
    fw_set(c, "awdl_election_tree", b, bcm_awdl_election_tree_encode(b, &e));
    uint8_t tlvs[256];
    const size_t n = awdl_core_build_payload(c, tlvs, sizeof tlvs);
    if (n) fw_set(c, "awdl_payload", b, bcm_awdl_payload(b, sizeof b, tlvs, n));
    fw_set(c, "awdl_opmode", b, bcm_awdl_opmode(b, BCM_AWDL_OPMODE_AUTO, BCM_AWDL_ROLE_SLAVE, 0, NULL));
    c->role = AWDL_ROLE_OFF;  // until the firmware reports its role
    awdl_log(c, "AWDL: started (firmware AWDL), channel %u, bsscfg %d", awdl_chan_primary(c->cfg.seq[0]),
             (int)c->cfg.bsscfg);
}

void awdl_core_start(struct awdl_core *c, uint64_t now)
{
    if (c->running) return;
    c->running = 1;
    if (native(c)) {
        c->next_slot = now + 1000000;  // peer expiry once a second
        native_start(c);
        return;
    }
    c->base_tsf = now - now % AWDL_AW_US;
    c->base_seq = 0;
    c->self_counter = 0;
    c->last_counter_tick = now;
    c->role = AWDL_ROLE_ROOT;
    run_election(c, now);
    install_schedule(c, now);
    c->next_slot = next_slot_start(c, now);
    c->next_psf = now;
    awdl_log(c, "AWDL: started, channel %u (%u MHz)", awdl_chan_primary(c->cfg.seq[0]), awdl_chan_freq(c->cfg.seq[0]));
}

void awdl_core_stop(struct awdl_core *c)
{
    if (!c->running) return;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i)
        if (c->peers[i].used) peer_lost(c, &c->peers[i], "stopped");
    if (native(c)) {
        uint8_t b[BCM_AWDL_IF2_LEN];
        fw_set(c, "awdl_if", b, bcm_awdl_if2(b, c->cfg.bsscfg, 0, awdl_bssid, c->cfg.addr));
    }
    c->txq_count = 0;
    c->running = 0;
    c->role = AWDL_ROLE_OFF;
    awdl_log(c, "AWDL: stopped");
}

int awdl_core_running(const struct awdl_core *c)
{
    return c->running;
}
enum awdl_role awdl_core_role(const struct awdl_core *c)
{
    return c->role;
}
const struct awdl_election_v2 *awdl_core_election(const struct awdl_core *c)
{
    return &c->adv;
}
const struct awdl_stats *awdl_core_stats(const struct awdl_core *c)
{
    return &c->stats;
}
const struct awdl_chan *awdl_core_sequence(const struct awdl_core *c)
{
    return c->cfg.seq;
}

void awdl_core_set_addr(struct awdl_core *c, const uint8_t addr[6])
{
    memcpy(c->cfg.addr, addr, 6);
    if (c->role == AWDL_ROLE_ROOT) {
        memcpy(c->adv.root, addr, 6);
        memcpy(c->adv.parent, addr, 6);
    }
}

void awdl_core_set_sequence(struct awdl_core *c, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t now)
{
    memcpy(c->cfg.seq, seq, sizeof c->cfg.seq);
    if (c->running) install_schedule(c, now);
}

void awdl_core_rx(struct awdl_core *c, const uint8_t *f, size_t len, const struct awdl_rx_meta *m)
{
    if (!c->running || len < 24) return;
    if ((f[0] & 0x0c) == 0x08) {
        handle_data(c, f, len, m);
        return;
    }
    if ((f[0] & 0xfc) != 0xd0) return;
    struct awdl_af a;
    if (awdl_af_parse(f, len, &a)) {
        // other action frames are not ours; a truncated AWDL one is counted
        if (len >= 24 + 5 && f[24] == 0x7f && !memcmp(f + 25, awdl_oui, 3)) c->stats.af_bad++;
        return;
    }
    c->stats.af_rx++;
    handle_af(c, &a, m);
}

uint64_t awdl_core_timer(struct awdl_core *c, uint64_t now)
{
    if (!c->running) return UINT64_MAX;
    if (native(c)) {
        if (now >= c->next_slot) {
            expire_peers(c, now);
            c->next_slot = now + 1000000;
        }
        return c->next_slot;
    }
    if (now - c->last_counter_tick >= AWDL_ROOT_COUNTER_US) {
        c->last_counter_tick = now;
        if (c->role == AWDL_ROLE_ROOT) c->self_counter++;
        run_election(c, now);
    }
    if (now >= c->next_slot) {
        expire_peers(c, now);
        run_election(c, now);
        rebase(c, now);
        if (now - c->anchor >= AWDL_SEQ_US) install_schedule(c, now);
        if (awdl_chan_freq(awdl_core_channel_at(c, now))) send_af(c, AWDL_SUBTYPE_MIF, now);
        flush(c, now);
        c->next_slot = next_slot_start(c, now);
    }
    if (now >= c->next_psf) {
        if (awdl_chan_freq(awdl_core_channel_at(c, now))) send_af(c, AWDL_SUBTYPE_PSF, now);
        c->next_psf = now + AWDL_PSF_US;
    }
    return c->next_psf < c->next_slot ? c->next_psf : c->next_slot;
}

// ---- native backend: firmware events ------------------------------------------------------------------------------
void awdl_core_fw_event(struct awdl_core *c, const struct awdl_fw_event *e, const uint8_t *data, size_t len,
                        uint64_t now)
{
    if (!c->running || !native(c)) return;
    if (c->stats.fw_events++ == 0)
        awdl_log(c, "AWDL: first firmware AWDL event: %u (%u bytes)", (unsigned)e->type, (unsigned)len);
    switch (e->type) {
    case WLC_E_AWDL_RX_ACT_FRAME: {
        struct bcm_rx_frame meta;
        const uint8_t *frame;
        size_t n;
        struct awdl_af a;
        if (bcm_awdl_rx_frame_decode(data, len, &meta, &frame, &n) || awdl_af_parse(frame, n, &a)) {
            c->stats.af_bad++;
            return;
        }
        if (c->stats.af_rx++ == 0)  // the native backend's first question: do the firmware's AWDL frames reach us
            awdl_log(c, "AWDL: first AWDL action frame from the firmware: %02x:%02x:%02x:%02x:%02x:%02x, %s, %u bytes",
                     a.src[0], a.src[1], a.src[2], a.src[3], a.src[4], a.src[5],
                     a.subtype == AWDL_SUBTYPE_MIF ? "MIF" : "PSF", (unsigned)n);
        const int32_t r = meta.rssi < -128 ? -128 : meta.rssi > 0 ? 0 : meta.rssi;
        const struct awdl_rx_meta m = {now, 0, (int8_t)r};
        handle_af(c, &a, &m);
        return;
    }
    case WLC_E_AWDL_ROLE: {
        struct bcm_awdl_role r;
        if (bcm_awdl_role_decode(data, len, &r)) return;
        const enum awdl_role was = c->role;
        c->role = r.role == BCM_AWDL_ROLE_SLAVE ? AWDL_ROLE_FOLLOWER : AWDL_ROLE_ROOT;
        memcpy(c->adv.root, r.role == BCM_AWDL_ROLE_SLAVE ? r.master : c->cfg.addr, 6);
        memcpy(c->adv.parent, r.master, 6);
        if (was != c->role)
            awdl_log(c, "AWDL: firmware role %s, master %02x:%02x:%02x:%02x:%02x:%02x",
                     r.role == BCM_AWDL_ROLE_SLAVE    ? "slave"
                     : r.role == BCM_AWDL_ROLE_MASTER ? "master"
                                                      : "non-election master",
                     r.master[0], r.master[1], r.master[2], r.master[3], r.master[4], r.master[5]);
        return;
    }
    case WLC_E_IF: {  // the AWDL bsscfg the firmware made (or removed) for awdl_if
        struct bcm_if_event ie;
        if (bcm_if_event_decode(data, len, &ie)) return;
        if (ie.opcode == 1) c->cfg.bsscfg = ie.bssidx;
        awdl_log(c, "AWDL: firmware interface %s: ifidx %u bsscfg %u role %u",
                 ie.opcode == 1   ? "added"
                 : ie.opcode == 2 ? "removed"
                                  : "changed",
                 ie.ifidx, ie.bssidx, ie.role);
        return;
    }
    case WLC_E_AWDL_AW: {
        struct bcm_awdl_aw aw;
        if (!bcm_awdl_aw_decode(data, len, &aw)) c->stats.aws++;
        return;
    }
    default:
        // Payloads not settled yet (AWDL-NATIVE.md): the first few events of other kinds are logged with their size.
        if (c->stats.fw_unknown++ < 8)
            awdl_log(c, "AWDL: firmware event %u status %u reason %u, %u bytes", (unsigned)e->type, (unsigned)e->status,
                     (unsigned)e->reason, (unsigned)len);
        return;
    }
}

void awdl_core_rx_eth(struct awdl_core *c, const uint8_t *eth, size_t len, uint64_t now)
{
    if (!c->running || len < 14) return;
    for (unsigned i = 0; i < AWDL_MAX_PEERS; ++i)
        if (c->peers[i].used && addr_eq(c->peers[i].addr, eth + 6)) c->peers[i].last_seen = now;
    c->stats.data_rx++;
}

// ---- TSF clock ----------------------------------------------------------------------------------------------------------
void awdl_tsf_clock_anchor(struct awdl_tsf_clock *k, uint64_t tsf, uint64_t host_ns)
{
    if (k->valid && host_ns > k->host0_ns + 100000000ull) {  // rate from anchors at least 100 ms apart
        const int64_t dh = (int64_t)((host_ns - k->host0_ns) / 1000);
        const int64_t dt = (int64_t)(tsf - k->tsf0);
        int64_t ppm16 = (dt - dh) * 16000000 / dh;
        if (ppm16 > 16 * 500) ppm16 = 16 * 500;  // a crystal is within ±500 ppm; more is a TSF jump
        if (ppm16 < -16 * 500) ppm16 = -16 * 500;
        k->ppm_x16 = (int32_t)((3 * (int64_t)k->ppm_x16 + ppm16) / 4);
    }
    k->tsf0 = tsf;
    k->host0_ns = host_ns;
    k->valid = 1;
}
uint64_t awdl_tsf_clock_now(const struct awdl_tsf_clock *k, uint64_t host_ns)
{
    if (!k->valid) return 0;
    const int64_t us = (int64_t)(host_ns - k->host0_ns) / 1000;
    return (uint64_t)((int64_t)k->tsf0 + us + us * k->ppm_x16 / 16000000);
}
uint64_t awdl_tsf_clock_host(const struct awdl_tsf_clock *k, uint64_t tsf)
{
    if (!k->valid) return 0;
    const int64_t us = (int64_t)(tsf - k->tsf0);
    const int64_t host_us = us - us * k->ppm_x16 / 16000000;
    return (uint64_t)((int64_t)k->host0_ns + host_us * 1000);
}
