// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Wi-Fi radio's network side: the Ethernet interface en0 the system routes through, the registry entries by
// which the network configuration and wifid find a wireless interface, and the Apple80211 requests (ioctls on that
// interface) wifid uses to scan, join and read state. Nothing here talks to the radio: it calls the station
// interface (SwitchWLANStation.h) and turns what it answers into the structures the system's wireless framework
// reads. DHCP and everything above the link are iOS's own (configd, IPConfiguration).
//
// Ported from UniXNU-sheng package/WifiNet.cpp (BSD-2-Clause-Patent; iPadOS 17.7.11 21H461, the same build as ours,
// where wifid scans and joins WPA2 networks through it): layouts, request numbers, events and the registry classes
// are sheng's. Dropped: its unattended join.txt test join, the traffic and input-stall diagnostics (they use sheng's
// boot tracing). Changed: the key goes to our supplicant as wifid gives it (a 32-byte PMK for cipher 6, else the
// passphrase), and logging is the first occurrence of each request type (what wifid asks shows on the console once).
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSerialize.h>
#include <sys/kern_event.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_ether.h>
#include <net/kpi_interface.h>
extern "C" {
#include <net/bpf.h>
}
#include <net/if_types.h>
#include <netinet/in.h>
#include <sys/mbuf.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <stdarg.h>
#include "SwitchWLANAwdlRadio.h"
#include "SwitchWLANStation.h"
#include "SwitchWLANTrace.h"

#define SIOCSA80211 0x802869c8UL  // _IOW('i', 200, struct apple80211req)
#define SIOCGA80211 0xc02869c9UL  // _IOWR('i', 201, struct apple80211req)

// (the header declares it without C linkage; the symbol is the C one)
extern "C" int swlan_kev_post(struct kev_msg *) __asm__("_kev_post_msg");

namespace {

struct apple80211req {
    char req_if_name[16];
    int32_t req_type;
    int32_t req_val;
    uint32_t req_len;
    user_addr_t req_data __attribute__((aligned(8)));
};

// request types
enum {
    kSsid = 1,
    kAuthType = 2,
    kCipherKey = 3,
    kChannel = 4,
    kPowerSave = 5,
    kTxPower = 7,
    kRate = 8,
    kBssid = 9,
    kScanReq = 10,
    kScanResult = 11,
    kCardCaps = 12,
    kState = 13,
    kPhyMode = 14,
    kOpMode = 15,
    kRssi = 16,
    kNoise = 17,
    kPower = 19,
    kAssociate = 20,
    kAssociateResult = 21,
    kDisassociate = 22,
    kSupportedChannels = 27,
    kLocale = 28,
    kDeauth = 29,
    kDriverVersion = 43,
    kHardwareVersion = 44,
    kCountryCode = 51,
    kRadioInfo = 54,
    kScanReqMultiple = 86,
    kScanCacheClear = 90,
    kVirtualIfCreate = 94,  // wifip2pd's awdl0 (SwitchWLANAwdl.cpp), only once en0 advertised AWDL
    kVirtualIfDelete = 95,
    kBtCoexFlags = 102,  // wifid's Bluetooth state (SwitchWLANCoex.h), the value in req_val
    kCurrentNetwork = 103,
    kBtPower = 104,
    kRssiBounds = 106,
    kChannelsInfo = 207,
    kHwSupportedChannels = 254,
};

// events
enum {
    kEvPowerChanged = 1,
    kEvSsidChanged = 2,
    kEvBssidChanged = 3,
    kEvLinkChanged = 4,
    kEvAssocDone = 9,
    kEvScanDone = 10
};

// APPLE80211_M_SUPPLICANT_EVENT (wifid starts monitoring 76 right after 80, ROAM_STATUS). IO80211.framework
// (21H461, the event 76 case of __eventRead) takes exactly these 40 bytes and makes the dictionary wifid reads:
// status and reason, transition-disable flags, the 4-way handshake's phase durations in us, joining a new network.
enum {
    kEvSupplicant = 76
};

struct supplicant_event {
    uint32_t status, reason, unused0, tsn_disabled_flags;
    uint32_t eapol_start_m1_us, m1_m2_us, m2_m3_us, m3_m4_us, total_us;
    uint8_t joining_new_network, unused1[3];
};

static_assert(sizeof(supplicant_event) == 40, "supplicant event layout");

// results of an association, as the framework numbers them
enum {
    kAssocOk = 1,
    kAssocFailed = 2,
    kAssocTimeout = 9,
    kAssocBadKey = 21
};

struct channel {
    uint32_t version, channel, flags;
};

struct channels_list {
    uint32_t version, count;
    channel list[401];
    uint32_t unused;
};  // 4824 bytes

struct scan_result {  // 1164 bytes
    uint32_t version;
    channel ch;
    uint16_t noise_known;
    int16_t noise, snr, rssi;
    uint16_t beacon_interval, capability;
    uint8_t bssid[6];
    uint8_t rate_count, unused0;
    uint32_t rates[15];
    uint8_t ssid_length, ssid[32], unused1[3];
    uint32_t age_ms;
    uint8_t flags, unused2;
    uint16_t elements_length;
    uint8_t elements[1024];
};

struct assoc_request {  // 592 bytes
    uint32_t version;
    uint16_t mode, auth_lower, auth_upper, unused0;
    uint32_t ssid_length;
    uint8_t ssid[32];
    uint8_t bssid[6], unused1[2];
    uint32_t key_version, key_length, key_cipher;
    uint16_t key_flags, key_index;
    uint8_t key[64];
    uint8_t rest[0x250 - 0x88];
};

struct link_event {
    uint8_t down, unused0[3];
    int32_t rssi;
    uint16_t snr, noise;
    uint8_t unused1[4];
    uint8_t involuntary, unused2[3];
    uint32_t reason, subreason;
    uint8_t unused3[4];
};

static_assert(sizeof(scan_result) == 1164 && __builtin_offsetof(scan_result, ssid_length) == 0x60 &&
                  __builtin_offsetof(scan_result, age_ms) == 0x84 && __builtin_offsetof(scan_result, elements) == 0x8c,
              "scan result layout");
static_assert(sizeof(assoc_request) == 592 && __builtin_offsetof(assoc_request, key) == 0x48, "association layout");
static_assert(sizeof(channels_list) == 4824, "channel list layout");
static_assert(sizeof(link_event) == 32, "link event layout");
static_assert(sizeof(struct ifmediareq64) == 44 && SIOCGIFMEDIA64 == SIOCGIFMEDIA,
              "media request as userland sends it");

ifnet_t gIf;
uint8_t gMac[6];
bool gPower = true;
bool gKeyed;            // the last join used a key
bool gLinkAnnounced;    // wifid was told the link is up (an association) and not yet that it went down
uint32_t gBtFlags;      // wifid's APPLE80211_IOC_BT_COEX_FLAGS: SWLAN_BT_* (SwitchWLANCoex.h)
uint32_t gBtPower = 1;  // wifid's APPLE80211_IOC_BT_POWER
uint32_t gRxFrames, gRxErrors, gRxNoBuffer, gTxOversize, gTxRefused, gTxStarts, gTxBatches, gRxBatches;
// en0 runs the stack's output queue (start model: the stack queues, we take what the radio has buffers for); false
// when that interface could not be made and en0 fell back to the legacy if_output, which drops what does not fit.
bool gStartModel;
int gAssocResult = kAssocOk;
unsigned gScanCursor, gScanning;

// ---- events to the framework: class IEEE 802.11 (6), subclass 1, data starting with the interface name ----------
void post_event(uint32_t code, const void *payload, size_t length)
{
    uint8_t data[128];
    if (length > sizeof data - 16)
        length = sizeof data - 16;
    memset(data, 0, 16);
    snprintf(reinterpret_cast<char *>(data), 16, "en0");
    if (length)
        memcpy(data + 16, payload, length);
    struct kev_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.vendor_code = KEV_VENDOR_APPLE;
    msg.kev_class = 6;
    msg.kev_subclass = 1;
    msg.event_code = code;
    msg.dv[0].data_length = static_cast<uint32_t>(16 + length);
    msg.dv[0].data_ptr = data;
    swlan_kev_post(&msg);
}

// a data-link event about en0, as the stack posts them
void post_dl(uint32_t code)
{
    struct {
        struct net_event_data data;
    } payload;

    memset(&payload, 0, sizeof payload);
    payload.data.if_family = IFNET_FAMILY_ETHERNET;
    payload.data.if_unit = 0;
    snprintf(payload.data.if_name, sizeof payload.data.if_name, "en");
    struct kev_msg msg;
    memset(&msg, 0, sizeof msg);
    msg.vendor_code = KEV_VENDOR_APPLE;
    msg.kev_class = KEV_NETWORK_CLASS;
    msg.kev_subclass = KEV_DL_SUBCLASS;
    msg.event_code = code;
    msg.dv[0].data_length = sizeof payload;
    msg.dv[0].data_ptr = &payload;
    swlan_kev_post(&msg);
}

// link state for the network configuration: the interface's flags and the stack's own link events
void post_link(bool up)
{
    if (!gIf)
        return;
    ifnet_set_flags(gIf, up ? IFF_RUNNING : 0, IFF_RUNNING);
    post_dl(up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF);
}

// wifid makes a Wi-Fi device only for an interface the network configuration lists under State:/Network/Interface
// (wifid __WiFiManagerDeviceAddedCallback: "%@ does not exist in dynamic store", then it waits for that key to
// change). configd's KernelEventMonitor is the only writer: its prime() adds every interface getifaddrs() lists, and
// afterwards an attach event adds one. On an iPad the radio's interface attaches long after configd is up; en0
// attaches before userland starts, so its own attach event went to nobody. The attach is announced again, once when
// configd's interface namer opens the network stack and once at the first Apple80211 request from userland, each time
// with the link state (configd ignores a name it already lists). An event monitor that takes them reads en0's media
// for the link state: that request is the console's "first request from configd" line (note_caller).
enum Announce : unsigned {
    kAnnounceNamer = 1,
    kAnnounceRequest = 2
};

unsigned gAnnounced;

void announce_attached(Announce when)
{
    if (!gIf || (__atomic_fetch_or(&gAnnounced, static_cast<unsigned>(when), __ATOMIC_RELAXED) & when))
        return;
    swlan_status st;
    switch_wlan_status_get(&st);
    post_dl(KEV_DL_IF_ATTACHED);
    post_dl(st.state == SWLAN_STA_UP ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF);
}

// Who asks en0 anything, once per process (eight at most): whether configd ever looks at en0 shows on the console.
IOLock *gCallersLock;
char gCallers[8][MAXCOMLEN + 1];

void note_caller(unsigned long cmd)
{
    char who[MAXCOMLEN + 1] = {};
    proc_selfname(who, sizeof who);
    bool fresh = false;
    IOLockLock(gCallersLock);
    unsigned i = 0;
    while (i < 8 && gCallers[i][0] && strncmp(gCallers[i], who, sizeof who))
        ++i;
    if (i < 8 && !gCallers[i][0]) {
        strlcpy(gCallers[i], who, sizeof gCallers[i]);
        fresh = true;
    }
    IOLockUnlock(gCallersLock);
    if (fresh)
        IOLog("SwitchWLAN: en0: first request from %s (%#lx)\n", who, cmd);
}

// configd's interface namer reports the interfaces as complete (Plugin:InterfaceNamer "*QUIET*") only once the
// registry is quiet, and SystemConfiguration's interface list (SCNetworkInterfaceCopyAll: wifid's join, Settings,
// configd's own plugins) waits for that. A registry still busy when wifid starts asking is named, with its busy
// leaves, once.
void report_busy()
{
    IOService *root = IOService::getServiceRoot();
    const UInt32 busy = root ? root->getBusyState() : 0;
    if (!busy)
        return;
    char line[240];
    size_t at = static_cast<size_t>(
        snprintf(line, sizeof line, "SwitchWLAN: IOKit still busy (%u) as wifid starts:", static_cast<unsigned>(busy)));
    unsigned shown = 0;
    if (IORegistryIterator *it = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively)) {
        while (OSObject *o = it->getNextObject()) {
            IOService *s = OSDynamicCast(IOService, o);
            if (!s || !s->getBusyState())
                continue;
            bool busyChild = false;
            if (OSIterator *children = s->getChildIterator(gIOServicePlane)) {
                while (OSObject *c = children->getNextObject()) {
                    IOService *cs = OSDynamicCast(IOService, c);
                    if (cs && cs->getBusyState()) {
                        busyChild = true;
                        break;
                    }
                }
                children->release();
            }
            if (busyChild)
                continue;
            if (at < sizeof line)
                at += static_cast<size_t>(
                    snprintf(line + at, sizeof line - at, " %s(%s)", s->getName(), s->getMetaClass()->getClassName()));
            if (++shown == 6)
                break;
        }
        it->release();
    }
    IOLog("%s\n", line);
}

// ---- what the radio tells us ---------------------------------------------------------------------------------------
void on_scan_done()
{
    __atomic_store_n(&gScanning, 0u, __ATOMIC_RELEASE);
    gScanCursor = 0;
    post_event(kEvScanDone, nullptr, 0);
}

// A scan answered from the cache finishes after the time a short scan takes: answered at once, the daemon asks again
// straight away.
thread_call_t gCachedScan;

void cached_scan_done(thread_call_param_t, thread_call_param_t)
{
    on_scan_done();
}

// A link down carries the last signal too: wifid keeps it as the network's RSSI at the loss and lets the network
// auto-join again once a scan sees it better than that (__WiFiDeviceManagerHasNetworkRssiImproved; 0 is "invalid
// blacklistedRssi").
void post_link_event(bool up, int reason)
{
    link_event ev;
    memset(&ev, 0, sizeof ev);
    swlan_status st;
    switch_wlan_status_get(&st);
    ev.rssi = st.signal_dbm;
    ev.noise = static_cast<uint16_t>(-95);
    ev.snr = static_cast<uint16_t>(st.signal_dbm + 95);
    if (!up) {
        ev.down = 1;
        ev.involuntary = 1;
        ev.reason = static_cast<uint32_t>(reason);
    }
    gLinkAnnounced = up;
    post_event(kEvLinkChanged, &ev, sizeof ev);
}

// The order wifid (21H461) needs, as an iPad's own driver gives it: the link up at the association, the join's
// completion after the key handshake. wifid finds the joined network for a link up (APPLE80211_IOC_CURRENT_NETWORK;
// none: "Link up event with no current network detected. Dropping."), ranks en0 for routes (SetInterfaceRank
// Default) and, for a secured network, holds the link up from its clients ("Hold off dispatching client link up
// event until 4 way completes"). The association's completion (ASSOC_DONE, then __WiFiManagerJoinDoneCallback)
// hands the held link up to them; one that completes first finds nothing held, and Settings never shows the network
// joined (r3j: completion, then the link up, held for good).
void on_associated()
{
    post_event(kEvSsidChanged, nullptr, 0);
    post_event(kEvBssidChanged, nullptr, 0);
    post_link_event(true, 0);
}

void on_connect_done(int result)
{
    gAssocResult = result == SWLAN_RES_OK        ? kAssocOk
                   : result == SWLAN_RES_BAD_KEY ? kAssocBadKey
                   : result == SWLAN_RES_TIMEOUT ? kAssocTimeout
                                                 : kAssocFailed;
    if (result == SWLAN_RES_OK) {
        post_link(true);  // en0 carries data from here: the keys are in
        if (gKeyed) {
            // wifid's supplicant event: status 0, reason 0, the handshake done (its phase durations are not measured)
            supplicant_event sup;
            memset(&sup, 0, sizeof sup);
            post_event(kEvSupplicant, &sup, sizeof sup);
        }
    }
    post_event(kEvAssocDone, nullptr, 0);
    // A join that failed after the association (the key handshake): the link up wifid was given goes down again.
    if (result != SWLAN_RES_OK && gLinkAnnounced)
        post_link_event(false, 0);
}

// en0's queued frames are for the network that went: dropped (the stack's queue now, frames the start routine holds
// at its next run), so none go out on the next one.
bool gTxPurge;

void tx_purge()
{
    if (!gIf || !gStartModel)
        return;
    ifnet_purge(gIf);
    __atomic_store_n(&gTxPurge, true, __ATOMIC_RELEASE);
}

void on_link_lost(int reason)
{
    post_link(false);
    post_link_event(false, reason);
    tx_purge();
}

// Work loop only: frames received in this interrupt, not yet handed to the stack.
mbuf_t gRxHead, gRxTail;
uint32_t gRxCount, gRxBytes;
// Fed by on_rx and tx_fill, both under SwitchWLAN's gate (the work loop's); read under it too. Debug kernels only
// (the stats' probe line).
#if DEVELOPMENT || DEBUG
constexpr bool kTrace = true;
#else
constexpr bool kTrace = false;
#endif
swlan_trace gTrace;

uint64_t trace_now()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

void on_rx(const uint8_t *eth, size_t length)
{
    if (!gIf || length < 14)
        return;
    if (kTrace) {
        const swlan_frame_facts facts = swlan_frame_classify(eth, length);
        if (facts.kind != SWLAN_FRAME_OTHER)
            swlan_trace_note_received(&gTrace, facts, !(eth[0] & 1), trace_now());
    }

    // Two bytes ahead of the Ethernet header put the IP header on a 4-byte boundary. The packet may come back as a
    // chain: each buffer gets only what fits in it (sheng i469: setting the whole length on the first one let the stack
    // read past that buffer's end).
    enum : size_t {
        Align = 2
    };

    mbuf_t m = nullptr;
    if (mbuf_allocpacket(MBUF_DONTWAIT, length + Align, nullptr, &m) || !m) {
        gRxNoBuffer++;
        return;
    }
    size_t done = 0;
    bool first = true;
    for (mbuf_t n = m; n; n = mbuf_next(n)) {
        size_t room = mbuf_maxlen(n) - mbuf_leadingspace(n);
        if (first) {
            if (room < Align + 14)
                break;
            mbuf_setdata(n, static_cast<uint8_t *>(mbuf_datastart(n)) + mbuf_leadingspace(n) + Align, 0);
            room -= Align;
        }
        const size_t take = length - done < room ? length - done : room;
        memcpy(mbuf_data(n), eth + done, take);
        mbuf_setlen(n, take);
        done += take;
        first = false;
    }
    if (done != length) {
        gRxNoBuffer++;
        mbuf_freem(m);
        return;
    }
    mbuf_pkthdr_setlen(m, length);
    mbuf_pkthdr_setrcvif(m, gIf);
    mbuf_pkthdr_setheader(m, mbuf_data(m));
    // The packet filter sees what arrives only if the driver hands it over (as IONetworkInterface does): the
    // configuration's ARP client reads the router's ARP reply through it, and without it the router never counts as
    // reached (RouterARPVerified 0), the service is never published and en0 gets no route (r4a: bpf0 "fcount 0").
    bpf_tap_in(gIf, DLT_EN10MB, m, nullptr, 0);
    mbuf_adj(m, 14);  // the stack wants the Ethernet header apart from the payload
    // Gathered: the interrupt's frames go to the stack together (on_rx_flush), one input-thread wakeup for them all.
    if (gRxTail)
        mbuf_setnextpkt(gRxTail, m);
    else
        gRxHead = m;
    gRxTail = m;
    ++gRxCount;
    gRxBytes += static_cast<uint32_t>(length);
}

// Work loop, after each interrupt's frames: the gathered chain to the stack in one ifnet_input (it frees the chain
// when it refuses it).
void on_rx_flush()
{
    if (!gRxHead)
        return;
    struct ifnet_stat_increment_param inc;
    memset(&inc, 0, sizeof inc);
    inc.packets_in = gRxCount;
    inc.bytes_in = gRxBytes;
    mbuf_t chain = gRxHead;
    const uint32_t count = gRxCount;
    gRxHead = gRxTail = nullptr;
    gRxCount = gRxBytes = 0;
    if (ifnet_input(gIf, chain, &inc))
        gRxErrors += count;
    else
        gRxFrames += count;
    ++gRxBatches;
}

// Work loop: the radio has room again (or the station is up): en0's start routine runs again.
void on_tx_ready()
{
    if (gIf && gStartModel)
        ifnet_start(gIf);
}
const struct swlan_events gEvents = {on_scan_done, on_associated, on_connect_done, on_link_lost,
                                     on_rx,        on_rx_flush,   on_tx_ready};

// ---- the interface --------------------------------------------------------------------------------------------------
// Frames on their way out: written one at a time into the radio's DMA buffers (switch_wlan_tx_frames, under its
// gate), each copied once, straight from the mbuf chain. Sent frames go to the packet filter and are freed, with the
// dropped ones, by the caller after the batch, outside the gate (tx_done).
struct TxCursor {
    mbuf_t next;                // not yet written
    mbuf_t sentHead, sentTail;  // written into the radio's buffers
    mbuf_t droppedHead;         // too short, too long or unreadable
    uint32_t packets, bytes, dropped;
};

size_t tx_fill(void *context, uint8_t *buffer, size_t room)
{
    auto *c = static_cast<TxCursor *>(context);
    while (mbuf_t m = c->next) {
        c->next = mbuf_nextpkt(m);
        mbuf_setnextpkt(m, nullptr);
        const size_t length = mbuf_pkthdr_len(m);
        if (length < 14 || length > room || mbuf_copydata(m, 0, length, buffer)) {
            mbuf_setnextpkt(m, c->droppedHead);
            c->droppedHead = m;
            ++c->dropped;
            continue;
        }
        if (kTrace) {
            const swlan_frame_facts facts = swlan_frame_classify(buffer, length);
            if (facts.kind != SWLAN_FRAME_OTHER)
                swlan_trace_note_sent(&gTrace, facts, trace_now());
        }
        if (c->sentTail)
            mbuf_setnextpkt(c->sentTail, m);
        else
            c->sentHead = m;
        c->sentTail = m;
        ++c->packets;
        c->bytes += static_cast<uint32_t>(length);
        return length;
    }
    return 0;
}

void tx_done(ifnet_t ifp, TxCursor &c)
{
    for (mbuf_t m = c.sentHead; m; m = mbuf_nextpkt(m))
        bpf_tap_out(gIf, DLT_EN10MB, m, nullptr, 0);
    if (c.sentHead)
        mbuf_freem_list(c.sentHead);
    if (c.droppedHead)
        mbuf_freem_list(c.droppedHead);
    ifnet_stat_increment_out(ifp, c.packets, c.bytes, c.dropped);
    if (c.dropped)
        __atomic_fetch_add(&gTxOversize, c.dropped, __ATOMIC_RELAXED);
}

// en0's start routine (the stack's starter thread for en0, one run at a time): frames from the stack's output queue
// into the radio, as many as it has buffers for. When it has none, switch_wlan_tx_room says 0 and the radio calls
// tx_ready once completions have freed a batch' worth; the frames wait in the stack's queue meanwhile (its AQM and
// flow control decide what waits and what is dropped, not a full ring). Frames dequeued but not taken (the room
// went between the two calls: a handshake frame or AWDL took buffers) are held here and go first next time.
constexpr unsigned kTxBatch = 64;     // frames per dequeue: one gate hold and one doorbell
constexpr uint32_t kSendQueue = 512;  // the stack's queue for en0, frames
mbuf_t gTxHeld;

void if_start(ifnet_t ifp)
{
    __atomic_fetch_add(&gTxStarts, 1u, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&gTxPurge, false, __ATOMIC_ACQ_REL) && gTxHeld) {
        mbuf_freem_list(gTxHeld);
        gTxHeld = nullptr;
    }
    for (;;) {
        const unsigned room = switch_wlan_tx_room();
        if (!room)
            return;  // tx_ready starts this again
        if (!gTxHeld) {
            mbuf_t head = nullptr;
            uint32_t count = 0;
            if (ifnet_dequeue_multi(ifp, room < kTxBatch ? room : kTxBatch, &head, nullptr, &count, nullptr) || !head)
                return;  // the queue is empty
            gTxHeld = head;
        }
        TxCursor c = {gTxHeld, nullptr, nullptr, nullptr, 0, 0, 0};
        switch_wlan_tx_frames(tx_fill, &c, UINT32_MAX);
        gTxHeld = c.next;
        tx_done(ifp, c);
        __atomic_fetch_add(&gTxBatches, 1u, __ATOMIC_RELAXED);
        if (gTxHeld && !c.packets && !c.dropped)
            return;  // nothing taken: the radio's tx_ready follows
    }
}

// The legacy output (only when en0 could not get the stack's queue): one frame, dropped when the radio has no room.
errno_t if_output(ifnet_t ifp, mbuf_t m)
{
    mbuf_setnextpkt(m, nullptr);
    TxCursor c = {m, nullptr, nullptr, nullptr, 0, 0, 0};
    switch_wlan_tx_frames(tx_fill, &c, 1);
    if (c.next) {  // not taken
        mbuf_freem(c.next);
        ifnet_stat_increment_out(ifp, 0, 0, 1);
        __atomic_fetch_add(&gTxRefused, 1u, __ATOMIC_RELAXED);
    }
    tx_done(ifp, c);
    return 0;
}

void if_event_stub(ifnet_t, const struct kev_msg *)
{}

// ---- Apple80211 -------------------------------------------------------------------------------------------------------
// APPLE80211_C_FLAG_*: 20 MHz 0x2, 40 MHz 0x4 (0x200: its extension channel above), 80 MHz 0x400, 160 MHz 0x800;
// 2.4 GHz 0x8, 5 GHz 0x10; active scanning 0x80; DFS 0x100.
void fill_channel(channel &c, uint32_t mhz, uint16_t bw = 20, uint8_t center = 0)
{
    c.version = 1;
    if (mhz >= 4900) {
        c.channel = (mhz - 5000) / 5;
        c.flags = 0x10;
    } else {
        c.channel = mhz == 2484 ? 14 : (mhz - 2407) / 5;
        c.flags = 0x8;
    }
    if (bw >= 160)
        c.flags |= 0x800;
    else if (bw >= 80)
        c.flags |= 0x400;
    else if (bw >= 40)
        c.flags |= 0x4 | (center > c.channel ? 0x200 : 0);
    else
        c.flags |= 0x2;
}

void fill_result(scan_result &r, const swlan_bss &b)
{
    memset(&r, 0, sizeof r);
    r.version = 1;
    fill_channel(r.ch, b.freq_mhz, b.bw_mhz, b.center);
    r.noise_known = 1;
    r.noise = -95;
    r.rssi = b.signal_dbm;
    r.snr = static_cast<int16_t>(b.signal_dbm + 95);
    r.beacon_interval = b.beacon_int;
    r.capability = b.caps;
    memcpy(r.bssid, b.bssid, 6);
    r.ssid_length = b.ssid_len;
    memcpy(r.ssid, b.ssid, 32);
    r.age_ms = b.age_ms;
    r.elements_length = b.ie_len > sizeof r.elements ? sizeof r.elements : b.ie_len;
    memcpy(r.elements, b.ie, r.elements_length);
}

int copy_out(const apple80211req &rq, const void *src, size_t size)
{
    if (rq.req_len == 0 || !rq.req_data)
        return EINVAL;
    return copyout(src, rq.req_data, rq.req_len < size ? rq.req_len : size);
}

// A set's single value: in req_val without a buffer, else the buffer's {version, value}.
uint32_t set_value(const apple80211req &rq)
{
    uint32_t w[2] = {};
    if (rq.req_len >= sizeof(w) && rq.req_data && !copyin(rq.req_data, w, sizeof(w)))
        return w[1];
    return static_cast<uint32_t>(rq.req_val);
}

// A get that gives no buffer takes a single value in req_val; one with a buffer gets {version, value}.
int answer_value(apple80211req &rq, uint32_t value)
{
    if (rq.req_len == 0 || !rq.req_data) {
        rq.req_val = static_cast<int32_t>(value);
        return 0;
    }
    const uint32_t w[2] = {1, value};
    return copyout(w, rq.req_data, rq.req_len < sizeof w ? rq.req_len : sizeof w);
}

// The raw bytes, their length in req_len.
int answer_bytes(apple80211req &rq, const void *src, uint32_t size)
{
    if (!rq.req_data)
        return EINVAL;
    const uint32_t n = rq.req_len < size ? rq.req_len : size;
    rq.req_len = size;
    return copyout(src, rq.req_data, n);
}

int copy_in(const apple80211req &rq, void *dst, size_t size)
{
    if (!rq.req_data)
        return EINVAL;
    memset(dst, 0, size);
    return copyin(rq.req_data, dst, rq.req_len < size ? rq.req_len : size);
}

// The regulatory country as the access points announce it (802.11d: the Country element, id 7, of the strongest
// network that has one), else "XX".
void country(char cc[3])
{
    cc[0] = 'X';
    cc[1] = 'X';
    cc[2] = 0;
    int best = -200;
    swlan_bss *b = static_cast<swlan_bss *>(IOMallocData(sizeof(swlan_bss)));
    if (!b)
        return;
    for (int i = 0, n = switch_wlan_bss_count(); i < n; ++i) {
        if (switch_wlan_bss_get(i, b) || b->signal_dbm <= best)
            continue;
        for (unsigned o = 0; o + 2 <= b->ie_len && o + 2 + b->ie[o + 1] <= b->ie_len; o += 2 + b->ie[o + 1])
            if (b->ie[o] == 7 && b->ie[o + 1] >= 2 && b->ie[o + 2] >= 'A' && b->ie[o + 2] <= 'Z' &&
                b->ie[o + 3] >= 'A' && b->ie[o + 3] <= 'Z') {
                cc[0] = static_cast<char>(b->ie[o + 2]);
                cc[1] = static_cast<char>(b->ie[o + 3]);
                best = b->signal_dbm;
                break;
            }
    }
    IOFreeData(b, sizeof(swlan_bss));
}

// APPLE80211_S_*: init, scan, auth, assoc, run. As in net80211, run starts at the association (the link up wifid is
// given then); the key handshake happens in it.
int map_state(unsigned s)
{
    switch (s) {
    case SWLAN_STA_SCANNING:
        return 1;
    case SWLAN_STA_AUTH:
        return 2;
    case SWLAN_STA_ASSOC:
        return 3;
    case SWLAN_STA_KEYS:
    case SWLAN_STA_UP:
        return 4;
    default:
        return 0;
    }
}

int apple80211(bool set, apple80211req &rq)
{
    swlan_status st;
    switch_wlan_status_get(&st);
    const int type = rq.req_type;

    // (not on the stack: the largest answer is 4824 bytes, plus a scan record to fill)
    union Out {
        uint8_t raw[5120];
        channels_list chans;
        scan_result res;
    };

    struct Scratch {
        Out out;
        swlan_bss bss;
    };

    Scratch *scratch = static_cast<Scratch *>(IOMallocData(sizeof(Scratch)));
    if (!scratch)
        return ENOMEM;
    memset(scratch, 0, sizeof(Scratch));

    struct Free {
        Scratch *p;

        ~Free()
        {
            IOFreeData(p, sizeof(Scratch));
        }
    } freeScratch{scratch};

    Out &out = scratch->out;
    swlan_bss &b = scratch->bss;
    uint32_t *w = reinterpret_cast<uint32_t *>(out.raw);
    if (set) {
        switch (type) {
        case kPower: {
            uint32_t in[6];
            if (copy_in(rq, in, sizeof in))
                return EFAULT;
            gPower = in[2] != 0;
            switch_wlan_power(gPower);
            if (!gPower)
                post_link(false);
            post_event(kEvPowerChanged, nullptr, 0);
            return 0;
        }
        case kScanReq:
        case kScanReqMultiple: {
            uint8_t buf[0x80];
            if (copy_in(rq, buf, sizeof buf))
                return EFAULT;
            uint32_t ssidLength;
            memcpy(&ssidLength, buf + 16, 4);
            const uint8_t *ssid = buf + 20;
            // Joining or joined: a scan takes the radio off channel for seconds and the access point's beacons and
            // handshake frames are missed, so the daemon gets the last results instead.
            if (st.state >= SWLAN_STA_AUTH && switch_wlan_bss_count() > 0) {
                if (!gCachedScan)
                    gCachedScan = thread_call_allocate(cached_scan_done, nullptr);
                gScanning = 1;
                gScanCursor = 0;
                uint64_t at;
                clock_interval_to_deadline(2000, kMillisecondScale, &at);
                thread_call_enter_delayed(gCachedScan, at);
                return 0;
            }
            gScanning = 1;
            gScanCursor = 0;
            return switch_wlan_scan(ssidLength && ssidLength <= 32 ? ssid : nullptr, ssidLength <= 32 ? ssidLength : 0)
                       ? EBUSY
                       : 0;
        }
        case kAssociate: {
            assoc_request a;
            if (copy_in(rq, &a, sizeof a))
                return EFAULT;
            if (a.ssid_length > 32 || a.key_length > 64)
                return EINVAL;
            const bool hasKey = a.key_length != 0;
            // cipher 6: the daemon already derived the pairwise master key (32 bytes), which the supplicant takes as is
            const bool pmk = hasKey && a.key_cipher == 6 && a.key_length == 32;
            gAssocResult = kAssocFailed;
            gKeyed = hasKey;
            IOLog("SwitchWLAN: wifid asks to join a %u-byte name, %s\n", a.ssid_length,
                  !hasKey ? "open"
                  : pmk   ? "with its PMK"
                          : "with a passphrase");
            const int r = switch_wlan_connect(a.ssid, a.ssid_length, a.bssid, hasKey ? a.key : nullptr,
                                              hasKey ? a.key_length : 0, pmk);
            memset(&a, 0, sizeof a);
            return r ? EINVAL : 0;
        }
        case kDisassociate:
        case kDeauth:
            switch_wlan_disconnect();
            post_link(false);
            tx_purge();
            gLinkAnnounced = false;  // wifid's own leaving: it knows
            return 0;
        case kVirtualIfCreate:
        case kVirtualIfDelete: {
            uint8_t vif[32];
            if (copy_in(rq, vif, sizeof vif))
                return EFAULT;
            const int r = type == kVirtualIfCreate ? switch_wlan_awdl_vif_create(vif, sizeof vif)
                                                   : switch_wlan_awdl_vif_delete(vif, sizeof vif);
            // the created interface's name goes back in the caller's buffer
            if (!r && type == kVirtualIfCreate &&
                copyout(vif, rq.req_data, rq.req_len < sizeof vif ? rq.req_len : sizeof vif))
                return EFAULT;
            return r;
        }
        case kBtCoexFlags:
        case kBtPower:
            // What Bluetooth does, for the radio's coexistence handling and its stats line (SwitchWLANCoex.h); wifid
            // logged "APPLE80211_IOC_BT_COEX_FLAGS return -3903" at every A2DP start and stop while this was refused.
            if (type == kBtCoexFlags)
                gBtFlags = set_value(rq);
            else
                gBtPower = set_value(rq) != 0;
            switch_wlan_bt_state(gBtFlags, gBtPower != 0);
            return 0;
        case kScanCacheClear:
        case kLocale:
        case kCountryCode:
        case kPowerSave:
        case kAuthType:
        case kCipherKey:
            return 0;
        default:
            return EOPNOTSUPP;
        }
    }
    switch (type) {
    case kPower:
        w[0] = 1;
        w[1] = 1;
        w[2] = gPower ? 1 : 0;
        return copy_out(rq, w, 24);
    case kState:
        return answer_value(rq, static_cast<uint32_t>(gPower ? map_state(st.state) : 0));
    case kOpMode:
        return answer_value(rq, 1);  // station
    case kAuthType:
        w[0] = 1;
        w[1] = 1;
        w[2] = gKeyed ? 0x8 : 0;
        return copy_out(rq, w, 12);  // {version, open system, WPA2 personal or none}
    case kPowerSave:
        return answer_value(rq, 0);
    case kCardCaps: {  // a bitmap, bit n in byte n / 8: AES, AES-CCM, power management, short slot and preamble, WPA2, WME, short guard interval at 20 MHz
        static const uint8_t has[] = {2, 3, 6, 9, 10, 14, 17, 19};
        for (uint8_t bit : has)
            out.raw[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
        // AWDL (bit 32, what wifip2pd gates awdl0 on) when the radio's probe found the firmware's AWDL (SwitchWLANAwdl.cpp)
        if (switch_wlan_awdl_capable() == 1)
            out.raw[4] |= 1;
        return answer_bytes(rq, out.raw, 12);
    }
    case kPhyMode:
        w[0] = 1;
        w[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40;
        w[2] = st.state >= SWLAN_STA_ASSOC ? 0x8 : 0;
        return copy_out(rq, w, 12);
    case kSsid:
        if (st.state < SWLAN_STA_ASSOC)
            return ENOENT;
        return answer_bytes(rq, st.ssid, st.ssid_len);
    case kBssid:
        if (st.state < SWLAN_STA_ASSOC)
            return ENOENT;
        return answer_bytes(rq, st.bssid, 6);
    case kChannel:
        if (st.state < SWLAN_STA_AUTH)
            return ENOENT;
        w[0] = 1;
        fill_channel(*reinterpret_cast<channel *>(out.raw + 4), st.freq_mhz, st.bw_mhz, st.center);
        return copy_out(rq, w, 16);
    case kRssi:
    case kNoise: {  // {version, radios, unit (0: dBm), per chain[4], aggregate, extension per chain[4], its aggregate}
        if (type == kRssi && st.state < SWLAN_STA_ASSOC)
            return ENOENT;
        const uint32_t v = static_cast<uint32_t>(type == kRssi ? static_cast<int32_t>(st.signal_dbm) : -95);
        w[0] = 1;
        w[1] = 1;
        w[2] = 0;
        for (int i = 3; i < 13; i++)
            w[i] = v;
        return copy_out(rq, w, 13 * 4);
    }
    case kRate:  // Mb/s: the firmware's transmit rate once known, 54 until then
        return answer_value(rq, st.state < SWLAN_STA_ASSOC ? 0 : st.tx_rate_kbps ? st.tx_rate_kbps / 1000 : 54);
    case kTxPower:
        return answer_value(rq, 15);
    case kDriverVersion:
    case kHardwareVersion: {
        strlcpy(reinterpret_cast<char *>(out.raw), type == kDriverVersion ? "SwitchWLAN 1" : "BCM4356A3", 256);
        return copy_out(rq, out.raw, 256);
    }
    case kCountryCode: {
        char cc[3];
        country(cc);
        return answer_bytes(rq, cc, 3);
    }  // two letters and a NUL
    case kAssociateResult:
        return answer_value(rq, static_cast<uint32_t>(gAssocResult));
    case kBtCoexFlags:
        return answer_value(rq, gBtFlags);
    case kBtPower:
        return answer_value(rq, gBtPower);
    case kSupportedChannels:
    case kHwSupportedChannels: {
        out.chans.version = 1;
        unsigned n = 0;
        // The firmware's regulatory domain (US/0): 2.4 GHz 1-11, 5 GHz 36-48 and 149-165 scanned actively, and the
        // DFS channels 52-64 and 100-144, scanned passively (its escan covers them; wifid listed a network on 52).
        static const uint16_t known[] = {2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 5180,
                                         5200, 5220, 5240, 5745, 5765, 5785, 5805, 5825, 5260, 5280, 5300, 5320,
                                         5500, 5520, 5540, 5560, 5580, 5600, 5620, 5640, 5660, 5680, 5700, 5720};
        for (uint16_t f : known) {
            fill_channel(*reinterpret_cast<channel *>(&out.chans.list[n]), f);
            const bool dfs = f >= 5250 && f <= 5730;
            out.chans.list[n].flags |= dfs ? 0x100 : 0x80;
            n++;
        }
        out.chans.count = n;
        return copy_out(rq, &out.chans, sizeof out.chans);
    }
    case kScanResult: {
        const int count = switch_wlan_bss_count();
        if (gScanCursor >= static_cast<unsigned>(count) || switch_wlan_bss_get(static_cast<int>(gScanCursor), &b)) {
            gScanCursor = 0;
            return EIO;  // after the last record
        }
        gScanCursor++;
        fill_result(out.res, b);
        return copy_out(rq, &out.res, sizeof out.res);
    }
    case kRssiBounds:
        // {version, minimum, maximum, threshold} dBm (IO80211.framework 21H461 makes RSSI_MIN, RSSI_MAX, RSSI_THRESHOLD
        // of words 1-3). wifid reads it once, when it makes the Wi-Fi device, and auto-joins only networks above the
        // threshold: unanswered it is 0 and every network is "Below minimum RSSI threshold" (r3j, r3v, r4a).
        w[0] = 1;
        w[1] = static_cast<uint32_t>(-100);
        w[2] = 0;
        w[3] = static_cast<uint32_t>(-80);  // wifid's own bad-RSSI line (WiFiUsagePoorLinkSession badRssiThreshold)
        return copy_out(rq, w, 16);
    case kCurrentNetwork:  // the access point the firmware joined, which need not be in the scan table
        if (switch_wlan_current_bss(&b))
            return ENOENT;
        fill_result(out.res, b);
        return copy_out(rq, &out.res, sizeof out.res);
    default:
        return EOPNOTSUPP;
    }
}

// The stack's link-layer groups (its AF_LINK memberships, as IONetworkInterface collects them) for the radio's filter.
void push_multicast(ifnet_t ifp)
{
    uint8_t groups[SWLAN_MCAST_MAX][6];
    unsigned count = 0;
    bool all = (ifnet_flags(ifp) & IFF_ALLMULTI) != 0;
    ifmultiaddr_t *list = nullptr;
    if (ifnet_get_multicast_list(ifp, &list) != 0) {
        switch_wlan_multicast(groups, 0, 1);
        return;
    }
    for (unsigned i = 0; list[i] && !all; ++i) {
        union {
            struct sockaddr sa;
            struct sockaddr_dl dl;
            uint8_t room[64];
        } address = {};

        if (ifmaddr_address(list[i], &address.sa, sizeof(address)) != 0 || address.sa.sa_family != AF_LINK ||
            address.dl.sdl_alen != 6)
            continue;
        if (count == SWLAN_MCAST_MAX) {
            all = true;
            break;
        }
        memcpy(groups[count++], LLADDR(&address.dl), 6);
    }
    ifnet_free_multicast_list(list);
    switch_wlan_multicast(groups, count, all);
}

errno_t if_ioctl(ifnet_t ifp, unsigned long cmd, void *data)
{
    note_caller(cmd);
    switch (cmd) {
    case SIOCGA80211:
    case SIOCSA80211: {
        if (!(__atomic_load_n(&gAnnounced, __ATOMIC_RELAXED) & kAnnounceRequest))
            report_busy();
        announce_attached(kAnnounceRequest);
        apple80211req *rq = static_cast<apple80211req *>(data);
        return apple80211(cmd == SIOCSA80211, *rq);
    }
    case SIOCGIFMEDIA64: {  // (the stack hands SIOCGIFXMEDIA over as this too)
        struct ifmediareq64 *m = static_cast<struct ifmediareq64 *>(data);
        swlan_status st;
        switch_wlan_status_get(&st);
        // IO80211.framework lists wireless interfaces by this media type; configd reads the link state from the status
        static const int media = IFM_IEEE80211 | IFM_AUTO;
        m->ifm_current = media;
        m->ifm_mask = 0;
        m->ifm_status = IFM_AVALID | (st.state == SWLAN_STA_UP ? IFM_ACTIVE : 0);
        m->ifm_active = media;
        if (m->ifmu_ulist && m->ifm_count > 0 && copyout(&media, m->ifmu_ulist, sizeof media))
            return EFAULT;
        m->ifm_count = 1;
        return 0;
    }
    case SIOCSIFFLAGS:  // IFF_ALLMULTI comes this way
    case SIOCADDMULTI:
    case SIOCDELMULTI:
        push_multicast(ifp);
        return 0;
    case SIOCSIFMTU:
    case SIOCSIFADDR:
    case SIOCSIFMEDIA:
        return 0;
    default: {
        static unsigned said;
        if (said++ < 16)
            IOLog("SwitchWLAN: interface request %lx not handled\n", cmd);
        return EOPNOTSUPP;
    }
    }
}

}  // namespace

// ---- registry entries -----------------------------------------------------------------------------------------------
// No Apple networking family is in this kernel (none of IONetworkingFamily, IO80211Family, IOSkywalkFamily is
// prelinked), so these names are free; the network configuration and wifid tell a wireless interface by them.
class IO80211Controller : public IOService {
    OSDeclareDefaultStructors(IO80211Controller);
};

OSDefineMetaClassAndStructors(IO80211Controller, IOService);

class IONetworkInterface : public IOService {
    OSDeclareDefaultStructors(IONetworkInterface);
};

OSDefineMetaClassAndStructors(IONetworkInterface, IOService);

class IO80211Interface : public IONetworkInterface {
    OSDeclareDefaultStructors(IO80211Interface);

public:
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE
    {
        return kIOReturnUnsupported;  // wifid's requests come as ioctls on en0
    }
};

OSDefineMetaClassAndStructors(IO80211Interface, IONetworkInterface);

// The network configuration's interface namer waits for a service of this class, opens it and only then publishes
// that IOKit is quiet; everything that lists interfaces (wifid among them) waits for that. Interfaces here are named
// when they are made, so the commands it sends (as properties of its connection) are taken and ignored. One sent at
// all means the namer took en0 for unnamed and waits for a name that does not come: the first two show.
class IONetworkStackUserClient : public IOUserClient {
    OSDeclareDefaultStructors(IONetworkStackUserClient);

public:
    IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE
    {
        static unsigned said;
        if (__atomic_fetch_add(&said, 1u, __ATOMIC_RELAXED) < 2) {
            char text[200] = "?";
            if (OSSerialize *s = OSSerialize::withCapacity(512)) {
                if (properties && properties->serialize(s))
                    strlcpy(text, s->text(), sizeof text);
                s->release();
            }
            IOLog("SwitchWLAN: the interface namer asks the network stack: %s\n", text);
        }
        return kIOReturnSuccess;
    }

    IOReturn clientClose() APPLE_KEXT_OVERRIDE
    {
        terminate();
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);

class IONetworkStack : public IOService {
    OSDeclareDefaultStructors(IONetworkStack);

public:
    IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *,
                           IOUserClient **handler) APPLE_KEXT_OVERRIDE
    {
        IONetworkStackUserClient *client = OSTypeAlloc(IONetworkStackUserClient);
        if (!client || !client->initWithTask(task, security, type)) {
            OSSafeReleaseNULL(client);
            return kIOReturnNoMemory;
        }
        if (!client->attach(this)) {
            client->release();
            return kIOReturnError;
        }
        if (!client->start(this)) {
            client->detach(this);
            client->release();
            return kIOReturnError;
        }
        *handler = client;
        announce_attached(kAnnounceNamer);  // configd is up: its event monitor was loaded before the namer
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(IONetworkStack, IOService);

namespace {

IOService *publish(IOService *service, IOService *provider, const char *name)
{
    if (!service || !service->attach(provider)) {
        OSSafeReleaseNULL(service);
        return nullptr;
    }
    service->setName(name);
    if (!service->start(provider)) {
        service->detach(provider);
        service->release();
        return nullptr;
    }
    service->registerService();
    return service;
}

void publish_registry(const char *ifname)
{
    IOService *platform = IOService::getPlatform();
    if (IONetworkStack *stack = OSTypeAlloc(IONetworkStack)) {
        if (!stack->init() || !publish(stack, platform, "IONetworkStack"))
            IOLog("SwitchWLAN: the network stack service did not start\n");
        else
            stack->release();
    }
    IO80211Controller *controller = OSTypeAlloc(IO80211Controller);
    IO80211Interface *wlan = OSTypeAlloc(IO80211Interface);
    if (!platform || !controller || !wlan || !controller->init() || !wlan->init()) {
        OSSafeReleaseNULL(controller);
        OSSafeReleaseNULL(wlan);
        IOLog("SwitchWLAN: registry objects not made\n");
        return;
    }
    if (OSData *mac = OSData::withBytes(gMac, 6)) {
        controller->setProperty("IOMACAddress", mac);
        mac->release();
    }
    controller->setProperty("IOBuiltin", true);
    controller = static_cast<IO80211Controller *>(publish(controller, platform, "SwitchWLANController"));
    if (!controller) {
        wlan->release();
        IOLog("SwitchWLAN: the controller did not start\n");
        return;
    }
    wlan->setProperty("IOInterfaceName", ifname);
    wlan->setProperty("BSD Name", ifname);
    wlan->setProperty("IOInterfaceNamePrefix", "en");
    if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(IFT_ETHER), 32)) {
        wlan->setProperty("IOInterfaceType", n);
        n->release();
    }
    if (OSNumber *n = OSNumber::withNumber(0ULL, 32)) {
        wlan->setProperty("IOInterfaceUnit", n);
        n->release();
    }
    wlan->setProperty("IOBuiltin", true);
    wlan->setProperty("IOPrimaryInterface", true);
    wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
    if (!publish(wlan, controller, "SwitchWLAN"))
        IOLog("SwitchWLAN: the interface service did not start\n");
    else
        wlan->release();
    controller->release();
}

}  // namespace

extern "C" void switch_wlan_net_counters(struct swlan_net_counters *out)
{
    out->rx_input = gRxFrames;
    out->rx_no_mbuf = gRxNoBuffer;
    out->rx_input_errors = gRxErrors;
    out->tx_oversize = __atomic_load_n(&gTxOversize, __ATOMIC_RELAXED);
    out->tx_refused = __atomic_load_n(&gTxRefused, __ATOMIC_RELAXED);
    out->tx_starts = __atomic_load_n(&gTxStarts, __ATOMIC_RELAXED);
    out->tx_batches = __atomic_load_n(&gTxBatches, __ATOMIC_RELAXED);
    out->rx_batches = gRxBatches;
    out->start_model = gStartModel ? 1 : 0;
}

extern "C" void switch_wlan_net_trace(struct swlan_trace_counts *out)
{
    *out = gTrace.c;
}

extern "C" void switch_wlan_net_rate(uint64_t bps)
{
    if (gIf && bps)
        ifnet_set_baudrate(gIf, bps);
}

// Called once the radio is up with its address (SwitchWLAN.cpp, worker thread).
namespace {

// en0 allocated with `p` and attached; nullptr (with the stage and the error) when either fails.
ifnet_t make_en0(struct ifnet_init_eparams &p, const char *&stage, errno_t &e)
{
    ifnet_t ifp = nullptr;
    stage = "allocation";
    e = ifnet_allocate_extended(&p, &ifp);
    if (e || !ifp)
        return nullptr;
    ifnet_set_mtu(ifp, 1500);
    ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff);
    ifnet_set_baudrate(ifp, 54000000ULL);  // until the firmware reports the link's rate (switch_wlan_net_rate)
    ifnet_set_hdrlen(ifp, 14);
    ifnet_set_addrlen(ifp, 6);
    if (p.flags & IFNET_INIT_LEGACY)
        ifnet_set_eflags(ifp, IFEF_ENQUEUE_MULTI, IFEF_ENQUEUE_MULTI);
    stage = "attach";
    e = ifnet_attach(ifp, nullptr);
    if (e) {
        ifnet_release(ifp);
        return nullptr;
    }
    return ifp;
}

}  // namespace

extern "C" void switch_wlan_net_start(const uint8_t mac[6])
{
    if (gIf)
        return;
    gCallersLock = IOLockAlloc();
    if (!gCallersLock) {
        IOLog("SwitchWLAN: no lock for en0\n");
        return;
    }
    memcpy(gMac, mac, 6);
    struct ifnet_init_eparams p;
    memset(&p, 0, sizeof p);
    p.ver = IFNET_INIT_CURRENT_VERSION;
    p.len = sizeof p;
    // Without NX_NOAUTO the stack puts a compatibility nexus in front of a Wi-Fi "en" interface, and its input no
    // longer goes through the interface's own input thread (sheng). Output: the stack's queue and our start routine
    // (if_start: what the radio has buffers for, the rest waits there, the stack's flow control holds TCP back);
    // the legacy if_output only if that interface cannot be made (one line). r8a, legacy: 40% of en0's frames at
    // fast.com's 40 Mb/s were dropped for want of one of 32 buffers (ACKs going down, and uploads' data).
    p.flags = IFNET_INIT_NX_NOAUTO;
    p.start = if_start;
    p.output_sched_model = IFNET_SCHED_MODEL_NORMAL;
    p.sndq_maxlen = kSendQueue;
    p.name = "en";
    p.unit = 0;
    p.family = IFNET_FAMILY_ETHERNET;
    p.subfamily = IFNET_SUBFAMILY_WIFI;
    p.type = IFT_ETHER;
    p.demux = ether_demux;
    p.add_proto = ether_add_proto;
    p.del_proto = ether_del_proto;
    p.check_multi = ether_check_multi;
    p.framer = ether_frameout;
    p.ioctl = if_ioctl;
    p.event = if_event_stub;
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    p.broadcast_addr = bcast;
    p.broadcast_len = 6;
    // The start model first; if the stack will not allocate or attach that interface, one line and the legacy one.
    const char *stage = "allocation";
    errno_t e = 0;
    ifnet_t ifp = make_en0(p, stage, e);
    gStartModel = ifp != nullptr;
    if (!gStartModel) {
        IOLog("SwitchWLAN: en0 with the stack's output queue not made (%s: %d): legacy output, frames past the "
              "radio's buffers dropped\n",
              stage, e);
        p.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
        p.start = nullptr;
        p.output_sched_model = 0;
        p.sndq_maxlen = 0;
        p.output = if_output;
        ifp = make_en0(p, stage, e);
    }
    if (!ifp) {
        IOLog("SwitchWLAN: ifnet %s failed: %d\n", stage, e);
        return;
    }
    ifnet_set_lladdr(ifp, mac, 6);
    // A driver of its own interface attaches the packet filter itself: the DHCP client sends through it while the
    // interface has no address.
    e = bpf_attach(ifp, DLT_EN10MB, 14, nullptr, nullptr);
    if (e)
        IOLog("SwitchWLAN: packet filter attach failed: %d\n", e);
    gIf = ifp;
    ifnet_set_flags(ifp, IFF_UP, IFF_UP);
    switch_wlan_set_events(&gEvents);
    publish_registry("en0");
}
