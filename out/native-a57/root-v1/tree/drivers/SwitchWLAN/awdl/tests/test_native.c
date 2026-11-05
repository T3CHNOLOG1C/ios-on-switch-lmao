// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the native backend (the firmware runs AWDL): the iovar payloads byte for byte against Broadcom's
// 7.x layouts, the bring-up order, peers learned from WLC_E_AWDL_RX_ACT_FRAME and added/removed with awdl_peer_op,
// the firmware's role, the data path handed to the firmware, and awdl0's requests driving it. A mock firmware records
// every iovar.
#include "../awdl.h"
#include "../awdl_apple80211.h"
#include "../awdl_bcm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int test_failures;
#define NCHECK(x)                                                        \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            test_failures++;                                             \
        }                                                                \
    } while (0)

static int bytes_eq(const uint8_t *got, size_t n, const char *hex)
{
    uint8_t want[256];
    size_t k = 0;
    while (*hex) {
        if (*hex == ' ') {
            hex++;
            continue;
        }
        unsigned v;
        if (sscanf(hex, "%2x", &v) != 1) return 0;
        want[k++] = (uint8_t)v;
        hex += 2;
    }
    if (k != n || memcmp(got, want, n)) {
        fprintf(stderr, "  got:");
        for (size_t i = 0; i < n; ++i) fprintf(stderr, " %02x", got[i]);
        fprintf(stderr, "\n");
        return 0;
    }
    return 1;
}

// ---- mock firmware -----------------------------------------------------------------------------------------------------
struct fw_call {
    char name[32];
    uint8_t buf[512];
    size_t len;
};
static struct fw_call calls[64];
static int ncalls, presents, absents, eths;
static uint8_t last_presence[6];
static int mock_iovar(void *ctx, int32_t bsscfg, const char *name, const void *buf, size_t len)
{
    (void)ctx, (void)bsscfg;
    if (ncalls == 64 || len > sizeof calls[0].buf) return -1;
    struct fw_call *c = &calls[ncalls++];
    snprintf(c->name, sizeof c->name, "%s", name);
    memcpy(c->buf, buf, len);
    c->len = len;
    return 0;
}
static int mock_tx(void *ctx, const uint8_t *f, size_t n, uint16_t freq, uint64_t tsf, int no_ack)
{
    (void)ctx, (void)f, (void)n, (void)freq, (void)tsf, (void)no_ack;
    return -1;  // the native backend never injects
}
static int mock_tx_eth(void *ctx, const uint8_t *eth, size_t len)
{
    (void)ctx, (void)eth, (void)len;
    eths++;
    return 0;
}
static struct a80211_awdl *facade;
static void mock_peer(void *ctx, const uint8_t addr[6], int present, int8_t rssi)
{
    (void)ctx;
    if (present)
        presents++;
    else
        absents++;
    memcpy(last_presence, addr, 6);
    if (facade) a80211_awdl_peer(facade, addr, present, rssi);
}
static const struct fw_call *find_call(const char *name, int nth)
{
    for (int i = 0; i < ncalls; ++i)
        if (!strcmp(calls[i].name, name) && nth-- == 0) return &calls[i];
    return NULL;
}

static void test_encoders(void)
{
    uint8_t b[512];
    const uint8_t me[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    NCHECK(bcm_awdl_if2(b, 1, 1, awdl_bssid, me) == 20 &&
           bytes_eq(b, 20, "01000000 01000000 002500ff9473 02aabbccddee"));
    struct bcm_awdl_config c;
    memset(&c, 0, sizeof c);
    c.channel = 149;
    c.aw_period = 16;
    c.aw_cmn_length = 16;
    c.action_frame_period = 110;
    c.pkt_lifetime_ms = 500;
    c.max_no_master = 8;
    c.ext_count = 3;
    c.aw_ext_length = 16;
    c.nmode = 2;
    memset(c.dest, 0xff, 6);
    // version, chan, guard, aw period, common length, AF period, lifetime, max no master, ext count, ext length,
    // nmode, destination (natural alignment: 28 bytes)
    NCHECK(bcm_awdl_config_encode(b, &c) == 28 &&
           bytes_eq(b, 28, "01000000 95 00 1000 1000 6e00 f401 0800 0300 1000 0200 ffffffffffff"));
    const uint8_t bc[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    NCHECK(bcm_awdl_af_hdr(b, bc) == 10 && bytes_eq(b, 10, "ffffffffffff 7f 0017f2"));
    struct awdl_chan seq[16];
    for (int i = 0; i < 16; ++i) seq[i] = (struct awdl_chan){155, 0x80};
    seq[1] = (struct awdl_chan){6, 0x51};
    NCHECK(
        bcm_awdl_chan_seq(b, sizeof b, seq, 16) == 38 &&
        bytes_eq(b, 38,
                 "0f 03 00 03 ffff 9b80 0651 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80 9b80"));
    NCHECK(bcm_awdl_chan_seq(b, 10, seq, 16) == 0);
    NCHECK(bcm_awdl_extcounts(b, 3, 3, 3, 3) == 4 && bytes_eq(b, 4, "03030303"));
    NCHECK(bcm_awdl_opmode(b, BCM_AWDL_OPMODE_AUTO, BCM_AWDL_ROLE_SLAVE, 0, NULL) == 12 &&
           bytes_eq(b, 12, "00 00 0000 000000000000 0000"));
    NCHECK(bcm_awdl_peer_op(b, BCM_AWDL_PEER_OP_ADD, me, 0) == 9 && bytes_eq(b, 9, "00 00 02aabbccddee 00"));
    NCHECK(bcm_awdl_peer_op(b, BCM_AWDL_PEER_OP_DEL, me, 0) == 9 && b[1] == 1);
    struct bcm_awdl_election_tree e;
    memset(&e, 0, sizeof e);
    e.self_metric = 60;
    e.close_sync_rssi = -65;
    e.max_tree_depth = 10;
    NCHECK(bcm_awdl_election_tree_encode(b, &e) == 42 && bytes_eq(b, 15, "00 0000 3c000000 bf 00 00 00 00 00 00 0a"));
    const uint8_t tlv[] = {0x15, 0x02, 0x00, 0x34, 0x02};
    NCHECK(bcm_awdl_payload(b, sizeof b, tlv, 5) == 9 && bytes_eq(b, 9, "05000000 15 0200 34 02"));
    NCHECK(bcm_u32(b, 4) == 4 && bytes_eq(b, 4, "04000000"));
    // events
    const uint8_t aw[] = {2, 1, 2, 3, 4, 5, 6, 0x2a};
    struct bcm_awdl_aw a;
    NCHECK(!bcm_awdl_aw_decode(aw, sizeof aw, &a) && a.role == 2 && a.master[5] == 6 && a.aw_seq == 0x2a);
    NCHECK(bcm_awdl_aw_decode(aw, 7, &a) < 0);
    struct bcm_awdl_role r;
    NCHECK(!bcm_awdl_role_decode(aw, 7, &r) && r.role == 2 && r.master[0] == 1);
    const uint8_t ife[] = {3, 1, 0, 2, 7};
    struct bcm_if_event ie;
    NCHECK(!bcm_if_event_decode(ife, 5, &ie) && ie.ifidx == 3 && ie.opcode == 1 && ie.bssidx == 2 && ie.role == 7);
}

static void test_native_flow(void)
{
    ncalls = presents = absents = eths = 0;
    // the peer: a soft core standing in for an iPhone, only to produce real MIFs
    struct awdl_core *phone = (struct awdl_core *)calloc(1, awdl_core_size());
    const uint8_t phone_addr[6] = {0x0a, 0x1b, 0x2c, 0x3d, 0x4e, 0x5f};
    struct awdl_config pc;
    awdl_config_default(&pc, phone_addr);
    pc.backend = AWDL_BACKEND_SOFT;
    pc.self_metric = 200;
    snprintf(pc.hostname, sizeof pc.hostname, "iPhone");
    const struct awdl_host ph = {NULL, mock_tx, NULL, NULL, NULL, NULL, NULL, NULL};
    NCHECK(awdl_core_init(phone, &ph, &pc) == 0);
    awdl_core_start(phone, 5000000);

    struct awdl_core *core = (struct awdl_core *)calloc(1, awdl_core_size());
    const uint8_t me[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    struct awdl_config cfg;
    awdl_config_default(&cfg, me);
    NCHECK(cfg.backend == AWDL_BACKEND_NATIVE);
    const struct awdl_host h = {NULL, mock_tx, NULL, mock_peer, NULL, NULL, mock_iovar, mock_tx_eth};
    NCHECK(awdl_core_init(core, &h, &cfg) == 0);

    // awdl0's requests drive it: AWDL_SYNC_ENABLED on
    static struct a80211_awdl f;
    const uint8_t caps[12] = {0x4c, 0x46, 0x0a, 0, 0x01};
    const struct a80211_awdl_ops ops = {NULL, NULL, NULL, NULL, NULL};
    a80211_awdl_init(&f, core, &ops, "awdl0", "en0", caps);
    facade = &f;
    struct a80211_awdl_sync_enabled on = {1, 0, 1};
    uint32_t len = sizeof on;
    int32_t val = 0;
    NCHECK(a80211_awdl_request(&f, 1, A80211_IOC_AWDL_SYNC_ENABLED, &val, &on, &len, sizeof on, 1000000) == 0);
    NCHECK(awdl_core_running(core));
    // the bring-up, in order
    static const char *const order[] = {"awdl_if",        "awdl_config",        "awdl_af_hdr",  "awdl_chan_seq",
                                        "awdl_extcounts", "awdl_election_tree", "awdl_payload", "awdl_opmode"};
    NCHECK(ncalls == 8);
    for (int i = 0; i < 8 && i < ncalls; ++i) NCHECK(!strcmp(calls[i].name, order[i]));
    NCHECK(calls[0].len == 20 && calls[0].buf[4] == 1 && !memcmp(calls[0].buf + 14, me, 6));
    NCHECK(calls[1].len == 28 && calls[1].buf[4] == 149);
    NCHECK(calls[3].len == 38);
    // the payload TLVs are well-formed AWDL TLVs with our host name and address
    {
        const struct fw_call *p = find_call("awdl_payload", 0);
        NCHECK(p && p->len > 4);
        uint8_t frame[600] = {0xd0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
        memcpy(frame + 16, awdl_bssid, 6);
        const uint8_t fixed[16] = {0x7f, 0x00, 0x17, 0xf2, 0x08, 0x10, 0x03, 0};
        memcpy(frame + 24, fixed, 16);
        const uint32_t n = (uint32_t)p->buf[0] | ((uint32_t)p->buf[1] << 8);
        NCHECK(n + 4 == p->len);
        memcpy(frame + 40, p->buf + 4, n);
        struct awdl_af a;
        NCHECK(awdl_af_parse(frame, 40 + n, &a) == 0);
        NCHECK((a.present & (1u << AWDL_TLV_ARPA)) && !strcmp(a.hostname, "switch"));
        NCHECK((a.present & (1u << AWDL_TLV_DATA_PATH_STATE)) && !memcmp(a.dps.awdl_address, me, 6));
        NCHECK(a.present & (1u << AWDL_TLV_VERSION));
    }
    NCHECK(awdl_core_role(core) == AWDL_ROLE_OFF);

    // the firmware made the AWDL bsscfg: WLC_E_IF add (a known event, not "unknown")
    struct awdl_fw_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = WLC_E_IF;
    const uint8_t ifadd[5] = {2, 1, 0, 1, 8};
    awdl_core_fw_event(core, &ev, ifadd, sizeof ifadd, 1050000);
    NCHECK(awdl_core_stats(core)->fw_unknown == 0 && awdl_core_stats(core)->fw_events == 1);
    // the firmware syncs to the phone: WLC_E_AWDL_ROLE slave, master = phone
    ev.type = WLC_E_AWDL_ROLE;
    uint8_t role[7] = {BCM_AWDL_ROLE_SLAVE};
    memcpy(role + 1, phone_addr, 6);
    awdl_core_fw_event(core, &ev, role, sizeof role, 1100000);
    NCHECK(awdl_core_role(core) == AWDL_ROLE_FOLLOWER && !memcmp(awdl_core_election(core)->root, phone_addr, 6));

    // the phone's MIF arrives as WLC_E_AWDL_RX_ACT_FRAME: rx header + frame
    uint8_t mif[1600];
    const size_t mif_len = awdl_core_build_af(phone, AWDL_SUBTYPE_MIF, 5100000, mif, sizeof mif);
    NCHECK(mif_len > 100);
    uint8_t evdata[1700];
    const uint8_t rxhdr[16] = {1, 0, 149, 0xd0, 0xc4, 0xff, 0xff, 0xff};  // version 1, chanspec, rssi -60
    memcpy(evdata, rxhdr, 16);
    memcpy(evdata + 16, mif, mif_len);
    ev.type = WLC_E_AWDL_RX_ACT_FRAME;
    const int before = ncalls;
    awdl_core_fw_event(core, &ev, evdata, 16 + mif_len, 1200000);
    NCHECK(presents == 1 && !memcmp(last_presence, phone_addr, 6));
    NCHECK(ncalls == before + 1 && !strcmp(calls[before].name, "awdl_peer_op") &&
           bytes_eq(calls[before].buf, 9, "00 00 0a1b2c3d4e5f 00"));
    const struct awdl_peer *p = awdl_core_find(core, phone_addr);
    NCHECK(p && p->reachable && p->rssi == -60 && !strcmp(p->hostname, "iPhone") && p->has_seq);
    // the same frame without a header, and a malformed one: no second add, the bad one counted
    awdl_core_fw_event(core, &ev, mif, mif_len, 1300000);
    NCHECK(presents == 1 && ncalls == before + 1);
    awdl_core_fw_event(core, &ev, mif, 30, 1300000);
    NCHECK(awdl_core_stats(core)->af_bad == 1);
    // an event whose payload is not settled is logged and counted, not fatal
    ev.type = WLC_E_AWDL_PEER_CACHE_CONTROL;
    awdl_core_fw_event(core, &ev, rxhdr, 4, 1300000);
    NCHECK(awdl_core_stats(core)->fw_unknown == 1);

    // data: to the phone and to a group go to the firmware's AWDL interface; to a stranger, nowhere
    uint8_t eth[64] = {0};
    memcpy(eth, phone_addr, 6);
    memcpy(eth + 6, me, 6);
    eth[12] = 0x86;
    eth[13] = 0xdd;
    NCHECK(awdl_core_output(core, eth, sizeof eth, 1400000) == 0 && eths == 1);
    eth[0] = 0x33;
    NCHECK(awdl_core_output(core, eth, sizeof eth, 1400000) == 0 && eths == 2);
    eth[0] = 0x02;
    eth[5] = 0x99;
    NCHECK(awdl_core_output(core, eth, sizeof eth, 1400000) < 0 && eths == 2);
    // data from the phone keeps it alive past the frame timeout
    uint8_t in[64] = {0};
    memcpy(in, me, 6);
    memcpy(in + 6, phone_addr, 6);
    awdl_core_rx_eth(core, in, sizeof in, 4000000);
    awdl_core_timer(core, 4100000);
    NCHECK(absents == 0);
    // silence: lost after 3 s, removed from the firmware, PEER_PRESENCE absent
    const int before_del = ncalls;
    awdl_core_timer(core, 7200000);
    NCHECK(absents == 1 && ncalls == before_del + 1 && !strcmp(calls[before_del].name, "awdl_peer_op") &&
           calls[before_del].buf[1] == BCM_AWDL_PEER_OP_DEL);
    // off: the AWDL bsscfg goes down
    struct a80211_awdl_sync_enabled off = {1, 0, 0};
    len = sizeof off;
    NCHECK(a80211_awdl_request(&f, 1, A80211_IOC_AWDL_SYNC_ENABLED, &val, &off, &len, sizeof off, 8000000) == 0);
    NCHECK(!awdl_core_running(core) && !strcmp(calls[ncalls - 1].name, "awdl_if") && calls[ncalls - 1].buf[4] == 0);
    facade = NULL;
    awdl_core_stop(phone);
    free(core);
    free(phone);
}

void test_native_main(void)
{
    test_encoders();
    test_native_flow();
}
