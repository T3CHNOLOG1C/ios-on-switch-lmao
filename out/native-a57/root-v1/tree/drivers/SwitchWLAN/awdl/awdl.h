// SPDX-License-Identifier: BSD-2-Clause-Patent
// AWDL (Apple Wireless Direct Link) protocol core: the on-air frame format, the availability-window timing model,
// master election, the peer table and the data path. Portable C11 with no OS calls: the kernel (SwitchWLANAwdl.cpp)
// and the host tests (awdl/tests) drive it through struct awdl_host and the entry points below.
//
// Written from the published descriptions of the protocol: Stute, Kreitschmann, Hollick, "One Billion Apples' Secret
// Sauce: Recipe for the Apple Wireless Direct Link Ad hoc Protocol" (MobiCom 2018) and the AWDL field definitions of
// Wireshark's dissector, cross-checked against the field lists iPadOS 21H461's own wifip2pd carries in its Swift
// metadata (AWDL-CONTRACT.md §4). No code from OWL (GPLv3) is used.
//
// Not thread safe: the caller serialises every call on one lock. No allocation: struct awdl_core is one block the
// caller provides (awdl_core_size()). No floating point (kernel code is -mgeneral-regs-only).
#ifndef AWDL_H
#define AWDL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- constants of the protocol -------------------------------------------------------------------------------------
#define AWDL_TU_US 1024u          // one time unit
#define AWDL_AW_TU 16u            // availability window
#define AWDL_EAW_AWS 4u           // one channel-sequence slot (extended AW) = 4 AWs = 64 TU
#define AWDL_SEQ_SLOTS 16u        // channel sequence length; one sequence = 1024 TU
#define AWDL_PSF_INTERVAL_TU 110u // period of PSFs
#define AWDL_MAX_PEERS 32u
#define AWDL_PEER_TIMEOUT_US 3000000u   // a peer silent this long is lost
#define AWDL_ELECTION_MAX_HOPS 10u
#define AWDL_TXQ_FRAMES 32u
#define AWDL_MAX_FRAME 1600u

extern const uint8_t awdl_bssid[6];  // 00:25:00:ff:94:73, address 3 of every AWDL frame
extern const uint8_t awdl_oui[3];    // 00:17:f2 (Apple)

// Action frame subtypes and TLV types (Stute et al. §4; Wireshark's awdl dissector)
enum { AWDL_SUBTYPE_PSF = 0, AWDL_SUBTYPE_MIF = 3 };
enum {
    AWDL_TLV_SERVICE_REQUEST = 1,
    AWDL_TLV_SERVICE_RESPONSE = 2,
    AWDL_TLV_SYNC_PARAMS = 4,
    AWDL_TLV_ELECTION_PARAMS = 5,
    AWDL_TLV_SERVICE_PARAMS = 6,
    AWDL_TLV_HT_CAPS = 7,
    AWDL_TLV_DATA_PATH_STATE = 12,
    AWDL_TLV_ARPA = 16,
    AWDL_TLV_VHT_CAPS = 17,
    AWDL_TLV_CHANNEL_SEQUENCE = 18,
    AWDL_TLV_SYNC_TREE = 20,
    AWDL_TLV_VERSION = 21,
    AWDL_TLV_ELECTION_PARAMS_V2 = 24,
};
// Channel sequence encodings
enum { AWDL_CHAN_ENC_SIMPLE = 0, AWDL_CHAN_ENC_LEGACY = 1, AWDL_CHAN_ENC_OPCLASS = 3 };
// Data path state flags (subset this stack writes and reads)
enum {
    AWDL_DPS_INFRA_INFO = 0x0001,
    AWDL_DPS_INFRA_ADDRESS = 0x0002,
    AWDL_DPS_AWDL_ADDRESS = 0x0004,
    AWDL_DPS_UMI = 0x0010,
    AWDL_DPS_COUNTRY_CODE = 0x0100,
    AWDL_DPS_SOCIAL_CHANNEL_MAP = 0x0200,
    AWDL_DPS_EXTENSION = 0x8000,
};

// One channel: the channel number and the operating class (0x51: 2.4 GHz 20 MHz; 0x73: 5 GHz 20 MHz; 0x80: 5 GHz
// 80 MHz, where the number is the block's centre, 42 for 36-48, 155 for 149-161).
struct awdl_chan {
    uint8_t number;
    uint8_t opclass;
};
uint16_t awdl_chan_freq(struct awdl_chan c);  // MHz of the primary 20 MHz channel; 0 for an empty slot
uint8_t awdl_chan_primary(struct awdl_chan c);

// ---- decoded TLVs ----------------------------------------------------------------------------------------------------
struct awdl_chanseq {
    uint8_t count;  // number of entries (1..16)
    uint8_t encoding, duplicate_count, step_count;
    uint16_t fill_channel;
    struct awdl_chan chan[AWDL_SEQ_SLOTS];
};
struct awdl_sync_params {
    uint8_t tx_channel;
    uint16_t tx_counter;  // TU until the sender's next AW starts (wifip2pd: "downCounter")
    uint8_t master_channel, guard_time;
    uint16_t aw_period, af_period, flags, aw_ext_length, aw_cmn_length, aw_remaining;
    uint8_t min_ext, max_ext_mcast, max_ext_ucast, max_ext_af;
    uint8_t master[6];
    uint8_t presence_mode;
    uint16_t aw_seq;  // the sender's current AW sequence number
    int16_t ap_beacon_delta;
    struct awdl_chanseq seq;
    uint8_t has_seq;
};
struct awdl_election_v1 {
    uint8_t flags;
    uint16_t id;
    uint8_t distance;
    uint8_t master[6];
    uint32_t master_metric, self_metric;
};
struct awdl_election_v2 {
    uint8_t root[6], parent[6];
    uint32_t root_counter, hops, root_metric, self_metric, id, flags, self_counter;
};
struct awdl_data_path_state {
    uint16_t flags;
    char country[3];
    uint16_t social_channel_map;
    uint8_t infra_bssid[6];
    uint16_t infra_channel;
    uint8_t infra_address[6], awdl_address[6];
};

// Everything one action frame carried that the stack uses.
struct awdl_af {
    uint8_t subtype;
    uint8_t version;
    uint32_t phy_tx_time, target_tx_time;
    uint8_t src[6], dst[6];
    uint32_t present;  // bit n: TLV type n was decoded (types < 32)
    struct awdl_sync_params sync;
    struct awdl_election_v1 election;
    struct awdl_election_v2 election2;
    struct awdl_chanseq chanseq;
    struct awdl_data_path_state dps;
    uint16_t service_update_indicator;
    uint8_t awdl_version, device_class;
    char hostname[64];
    uint16_t ht_caps;
};

// Parse an 802.11 frame (header + body, no FCS). 0: an AWDL action frame, decoded into *out. Negative: not AWDL or
// malformed (each TLV is bounds-checked; unknown TLVs are skipped).
int awdl_af_parse(const uint8_t *frame, size_t len, struct awdl_af *out);

// ---- the stack -------------------------------------------------------------------------------------------------------
struct awdl_rx_meta {
    uint64_t tsf;       // local TSF at the start of the frame, µs
    uint16_t freq_mhz;
    int8_t rssi;
};

// What the integration provides. All calls are made with the caller's lock held and must not block or re-enter the
// stack; queue work for later if needed.
struct awdl_host {
    void *ctx;
    // One 802.11 frame (no FCS) to send on freq_mhz at or after tx_tsf (0: now). no_ack: group/action frames. Returns 0
    // when queued.
    int (*tx)(void *ctx, const uint8_t *frame, size_t len, uint16_t freq_mhz, uint64_t tx_tsf, int no_ack);
    // The radio runs this sequence of 16 slots of 64 TU, slot 0 starting at anchor_tsf (a multiple of 1024 TU after
    // it). Called when the sequence or the synchronisation changes.
    void (*schedule)(void *ctx, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t anchor_tsf);
    // A peer became reachable for data (present != 0) or was lost.
    void (*peer)(void *ctx, const uint8_t addr[6], int present, int8_t rssi);
    // A received data frame for awdl0, as an Ethernet frame (destination, source, ethertype, payload).
    void (*deliver)(void *ctx, const uint8_t *eth, size_t len);
    // One line of log, already formatted. May be NULL.
    void (*log)(void *ctx, const char *line);
};

struct awdl_config {
    uint8_t addr[6];          // our AWDL address (awdl0's link-layer address)
    char hostname[32];        // for the arpa TLV, without ".local"
    struct awdl_chan seq[AWDL_SEQ_SLOTS];
    uint32_t self_metric;     // election metric; low so a nearby Apple device stays master
    uint8_t awdl_version;     // e.g. 0x34 = 3.4
    uint8_t device_class;     // 1 macOS, 2 iOS
    char country[3];
};
void awdl_config_default(struct awdl_config *c, const uint8_t addr[6]);

enum awdl_role { AWDL_ROLE_OFF, AWDL_ROLE_ROOT, AWDL_ROLE_FOLLOWER };

struct awdl_peer {
    uint8_t addr[6];
    uint8_t used, reachable, has_v2, has_seq;
    uint64_t first_seen, last_seen;  // local TSF
    int8_t rssi;
    uint8_t awdl_version, device_class;
    struct awdl_election_v2 election;
    struct awdl_sync_params sync;
    struct awdl_chanseq seq;
    uint64_t next_aw_tsf;  // the peer's next AW start, local TSF, from its last sync params
    uint16_t next_aw_seq;
    uint16_t last_data_seq;
    uint8_t has_data_seq;
    char hostname[64];
    uint32_t frames;
};

struct awdl_stats {
    uint32_t af_rx, af_bad, af_tx, psf_tx, mif_tx, data_rx, data_tx, data_bad, data_drop, tx_refused, resyncs;
    int32_t last_sync_error_us;
};

struct awdl_core;
size_t awdl_core_size(void);
int awdl_core_init(struct awdl_core *c, const struct awdl_host *host, const struct awdl_config *cfg);
// Start at local TSF `now` (we are root until we hear a better one), stop (every reachable peer reported lost).
void awdl_core_start(struct awdl_core *c, uint64_t now);
void awdl_core_stop(struct awdl_core *c);
int awdl_core_running(const struct awdl_core *c);
void awdl_core_set_addr(struct awdl_core *c, const uint8_t addr[6]);
void awdl_core_set_sequence(struct awdl_core *c, const struct awdl_chan seq[AWDL_SEQ_SLOTS], uint64_t now);
// A received 802.11 frame (action or data).
void awdl_core_rx(struct awdl_core *c, const uint8_t *frame, size_t len, const struct awdl_rx_meta *m);
// An Ethernet frame from awdl0. 0: sent or queued for the next availability window; negative: dropped.
int awdl_core_output(struct awdl_core *c, const uint8_t *eth, size_t len, uint64_t now);
// Run what is due at `now`; returns the local TSF of the next deadline (the caller arms one timer for it).
uint64_t awdl_core_timer(struct awdl_core *c, uint64_t now);

// Inspection
enum awdl_role awdl_core_role(const struct awdl_core *c);
const struct awdl_election_v2 *awdl_core_election(const struct awdl_core *c);
const struct awdl_peer *awdl_core_peer(const struct awdl_core *c, unsigned index);  // NULL past the end / unused
const struct awdl_peer *awdl_core_find(const struct awdl_core *c, const uint8_t addr[6]);
const struct awdl_stats *awdl_core_stats(const struct awdl_core *c);
const struct awdl_chan *awdl_core_sequence(const struct awdl_core *c);  // AWDL_SEQ_SLOTS entries
// The timing model: AW sequence number at `now`, start of AW `seq`, channel at `now`.
uint16_t awdl_core_aw_seq(const struct awdl_core *c, uint64_t now);
uint64_t awdl_core_aw_start(const struct awdl_core *c, uint16_t seq, uint64_t now);
struct awdl_chan awdl_core_channel_at(const struct awdl_core *c, uint64_t now);

// ---- frame building (exposed for tests) ------------------------------------------------------------------------------
// Builds our PSF or MIF for transmission at `now` into buf; returns its length (0 if buf is too small).
size_t awdl_core_build_af(struct awdl_core *c, uint8_t subtype, uint64_t now, uint8_t *buf, size_t size);

// ---- data path -------------------------------------------------------------------------------------------------------
// Ethernet frame -> AWDL 802.11 data frame (src = our address). Returns the length or 0.
size_t awdl_data_encap(const uint8_t *eth, size_t len, const uint8_t self[6], uint16_t seq, uint8_t *out,
                       size_t size);
// AWDL 802.11 data frame -> Ethernet frame. Returns the length or 0 (not AWDL data / malformed). *seq gets the AWDL
// sequence number, *retry the 802.11 retry bit.
size_t awdl_data_decap(const uint8_t *frame, size_t len, uint8_t *out, size_t size, uint16_t *seq, int *retry);

// ---- TSF clock: the radio's TSF from the host clock between reads ----------------------------------------------------
struct awdl_tsf_clock {
    uint64_t tsf0, host0_ns;
    int32_t ppm_x16;  // radio TSF rate relative to the host clock, in 1/16 ppm
    uint8_t valid;
};
void awdl_tsf_clock_anchor(struct awdl_tsf_clock *k, uint64_t tsf, uint64_t host_ns);
uint64_t awdl_tsf_clock_now(const struct awdl_tsf_clock *k, uint64_t host_ns);
uint64_t awdl_tsf_clock_host(const struct awdl_tsf_clock *k, uint64_t tsf);  // host ns at which the TSF reaches tsf

#ifdef __cplusplus
}
#endif

#endif
