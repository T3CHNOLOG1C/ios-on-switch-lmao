// The SwitchWLAN data path on the host: drivers/SwitchWLAN/SwitchWLANMsgbuf.cpp itself (its own translation unit over
// tests/switch_wlan_msgbuf_stubs.h) driven against a model of the BCM4356's msgbuf firmware: the chip's RAM (shared
// area, ring descriptors and indices) as host memory, the PCIe2 core's mailbox registers, and a firmware that takes
// receive buffers and transmit posts off the H2D rings and writes completions onto the D2H rings by physical address,
// raising the mailbox interrupt each time. The driver's own Mmio reads of the chip's RAM (the PCIe round trips a
// frame costs), its register reads and writes, doorbells, interrupts and the bytes copied per frame are counted, and
// each scenario prints one JSON object for tests/test_switch_wlan_msgbuf.py.
//
//   switch-wlan-msgbuf attach | rx | tx | mixed | eapol | small | link
#include <cstdio>
#include <cstring>  // SwitchWLANLink.h's snprintf, memset, memcmp, strlen
#include <deque>
#include <string>
#include <vector>

#include "SwitchWLANLink.h"
#include "SwitchWLANPrivate.h"

namespace {

// ---- what the driver did -------------------------------------------------------------------------------------------
struct Counts {
    uint64_t tcmReads = 0, tcmWrites = 0;  // Mmio accesses to the chip's RAM (ring indices, shared area)
    uint64_t regReads = 0, regWrites = 0;  // backplane registers through BAR0 (mailbox)
    uint64_t doorbells = 0;                // H2D mailbox 0 writes
    uint64_t msis = 0;                     // interrupts taken (service() calls)
    uint64_t copiedBytes = 0;              // frame bytes the CPU copied: out of RX buffers, into TX buffers
};
Counts gCount;

// ---- the chip's RAM ------------------------------------------------------------------------------------------------
constexpr uint32_t kTcmSize = 1u << 20;
uint8_t gTcm[kTcmSize];
constexpr uint32_t kShared = 0x1000, kDtohMb = 0x2000, kRingInfo = 0x3000, kRingMem = 0x4000;
constexpr uint32_t kH2DW = 0x5000, kH2DR = 0x5100, kD2HW = 0x5200, kD2HR = 0x5300;
constexpr uint32_t kPcie2Base = 0x18003000;
uint16_t gMaxRxPost = 255;

uint16_t tcm16(uint32_t a)
{
    return static_cast<uint16_t>(gTcm[a] | gTcm[a + 1] << 8);
}
uint32_t tcm32(uint32_t a)
{
    return tcm16(a) | static_cast<uint32_t>(tcm16(a + 2)) << 16;
}
void setTcm16(uint32_t a, uint16_t v)
{
    gTcm[a] = static_cast<uint8_t>(v);
    gTcm[a + 1] = static_cast<uint8_t>(v >> 8);
}
void setTcm32(uint32_t a, uint32_t v)
{
    setTcm16(a, static_cast<uint16_t>(v));
    setTcm16(a + 2, static_cast<uint16_t>(v >> 16));
}
void resetTcm()
{
    memset(gTcm, 0, sizeof(gTcm));
    setTcm16(kShared + 34, gMaxRxPost);  // max_rxbufpost
    setTcm32(kShared + 36, 0);           // rx_dataoffset
    setTcm32(kShared + 44, kDtohMb);
    setTcm32(kShared + 48, kRingInfo);
    setTcm32(kRingInfo + 0, kRingMem);
    setTcm32(kRingInfo + 4, kH2DW);
    setTcm32(kRingInfo + 8, kH2DR);
    setTcm32(kRingInfo + 12, kD2HW);
    setTcm32(kRingInfo + 16, kD2HR);
    setTcm16(kRingInfo + 52, 40);  // max_flowrings (v5: with the two common submission rings)
}

uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}
uint32_t le32(const uint8_t *p)
{
    return le16(p) | static_cast<uint32_t>(le16(p + 2)) << 16;
}
void put16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t *p, uint32_t v)
{
    put16(p, static_cast<uint16_t>(v));
    put16(p + 2, static_cast<uint16_t>(v >> 16));
}

// ---- the firmware ----------------------------------------------------------------------------------------------------
struct Frame {
    std::vector<uint8_t> bytes;
};
Frame makeFrame(uint32_t seq, size_t length)
{
    Frame f;
    f.bytes.resize(length);
    for (size_t i = 0; i < length; ++i) f.bytes[i] = static_cast<uint8_t>((seq * 131 + i * 7) & 0xFF);
    f.bytes[12] = 0x08;  // IPv4
    f.bytes[13] = 0x00;
    put32(f.bytes.data() + 14, seq);
    return f;
}

class Firmware {
public:
    struct Ring {
        uint32_t pa = 0;
        uint16_t items = 0, size = 0, pos = 0;
        bool live = false;
    };
    uint32_t mbInt = 0, mbMask = 0;
    Ring h2d[2 + 8];                                      // control submit, rx post, flow rings by id 2..
    Ring d2h[3];                                          // control complete, tx complete, rx complete
    std::deque<std::pair<uint32_t, uint32_t>> rxBuffers;  // posted: id, pa
    std::deque<std::pair<uint32_t, uint32_t>> ioctlBuffers, eventBuffers;
    struct Held {
        uint32_t id;
        uint16_t ring;
    };
    std::deque<Held> txHeld;    // transmit posts fetched, waiting for the air
    std::vector<Frame> txSeen;  // frames as the chip DMA'd them
    unsigned holdLimit = 64;    // frames the firmware fetches into its own memory before the air takes them
    uint64_t rxStarved = 0;     // frames dropped for want of a posted receive buffer
    unsigned minPosted = ~0u;   // fewest receive buffers it held when a frame came
    uint16_t lastTxStatus = 0;

    uint8_t *va(uint32_t pa) const
    {
        if (pa < kFakeAreaPA || pa - kFakeAreaPA >= gAreaLength) throw std::runtime_error("DMA outside the area");
        return gAreaBytes + (pa - kFakeAreaPA);
    }
    void attach()
    {
        *this = Firmware{};
        static const uint16_t items[5] = {64, 512, 64, 1024, 512}, sizes[5] = {40, 32, 24, 16, 32};
        for (unsigned k = 0; k < 5; ++k) {
            Ring &r = k < 2 ? h2d[k] : d2h[k - 2];
            const uint32_t mem = kRingMem + 16 * k;
            r.pa = tcm32(mem + 8);
            r.items = tcm16(mem + 4);
            r.size = tcm16(mem + 6);
            r.live = true;
            if (r.items != items[k] || r.size != sizes[k]) throw std::runtime_error("ring geometry");
        }
    }
    // The H2D rings up to the host's write indices (the firmware's own reads of its RAM are not PCIe for the host).
    void serviceH2D()
    {
        for (unsigned id = 0; id < sizeof(h2d) / sizeof(h2d[0]); ++id) {
            Ring &r = h2d[id];
            if (!r.live) continue;
            const uint16_t w = tcm16(kH2DW + 4 * id);
            while (r.pos != w) {
                const uint8_t *item = va(r.pa + static_cast<uint32_t>(r.pos) * r.size);
                if (id >= 2 && item[0] == 0x0F && txHeld.size() >= holdLimit) break;  // leaves it in the ring
                take(id, item);
                r.pos = static_cast<uint16_t>((r.pos + 1) % r.items);
                setTcm16(kH2DR + 4 * id, r.pos);
            }
        }
    }
    // One completion onto a D2H ring and its doorbell interrupt; false when the host has not made room.
    bool push(unsigned k, const uint8_t *item)
    {
        Ring &r = d2h[k];
        if ((r.pos + 1) % r.items == tcm16(kD2HR + 4 * k)) return false;
        memcpy(va(r.pa + static_cast<uint32_t>(r.pos) * r.size), item, r.size);
        r.pos = static_cast<uint16_t>((r.pos + 1) % r.items);
        setTcm16(kD2HW + 4 * k, r.pos);
        mbInt |= 0x10000;
        return true;
    }
    // A received frame into the oldest posted buffer.
    bool receive(const Frame &f)
    {
        minPosted = rxBuffers.size() < minPosted ? static_cast<unsigned>(rxBuffers.size()) : minPosted;
        if (rxBuffers.empty()) {
            ++rxStarved;
            return false;
        }
        const auto [id, pa] = rxBuffers.front();
        rxBuffers.pop_front();
        memcpy(va(pa), f.bytes.data(), f.bytes.size());
        uint8_t item[32] = {};
        item[0] = 0x12;
        put32(item + 4, id);
        put16(item + 14, static_cast<uint16_t>(f.bytes.size()));
        if (!push(2, item)) throw std::runtime_error("rx complete ring full");
        return true;
    }
    // The air took `n` of the frames held: their completions, oldest first.
    void transmitted(unsigned n, uint16_t status = 0)
    {
        for (; n && !txHeld.empty(); --n) {
            uint8_t item[16] = {};
            item[0] = 0x10;
            put32(item + 4, txHeld.front().id);
            put16(item + 8, status);
            put16(item + 10, txHeld.front().ring);
            if (!push(1, item)) throw std::runtime_error("tx complete ring full");
            txHeld.pop_front();
        }
    }
    void reg(uint32_t offset, uint32_t value)
    {
        switch (offset) {
        case 0x48: mbInt &= ~value; break;  // write 1 to clear
        case 0x4C: mbMask = value; break;
        case 0x140: ++gCount.doorbells; break;
        default: break;
        }
    }
    uint32_t reg(uint32_t offset) const { return offset == 0x48 ? mbInt : offset == 0x4C ? mbMask : 0; }
    bool interrupting() const { return (mbInt & mbMask) != 0; }

private:
    void control(const uint8_t *item)
    {
        uint8_t out[24] = {};
        switch (item[0]) {
        case 0x0B: ioctlBuffers.emplace_back(le32(item + 4), le32(item + 16)); return;
        case 0x0D: eventBuffers.emplace_back(le32(item + 4), le32(item + 16)); return;
        case 0x09: {  // an ioctl: its answer (four bytes) into the oldest response buffer
            if (ioctlBuffers.empty()) throw std::runtime_error("ioctl without a response buffer");
            const auto [id, pa] = ioctlBuffers.front();
            ioctlBuffers.pop_front();
            put32(va(pa), 0x12345678);
            out[0] = 0x0C;
            put32(out + 4, id);
            put16(out + 12, 4);
            put16(out + 14, le16(item + 12));
            break;
        }
        case 0x03: {  // flow ring create
            const uint16_t flowId = le16(item + 22);
            Ring &f = h2d[flowId];
            f.items = le16(item + 28);
            f.size = le16(item + 30);
            f.pa = le32(item + 32);
            f.pos = 0;
            f.live = true;
            out[0] = 0x04;
            put16(out + 10, flowId);
            break;
        }
        default: return;
        }
        if (!push(0, out)) throw std::runtime_error("control complete ring full");
    }
    void take(unsigned id, const uint8_t *item)
    {
        if (id == 0) return control(item);
        if (id == 1) {
            if (item[0] != 0x11) throw std::runtime_error("rx post ring: not a buffer post");
            rxBuffers.emplace_back(le32(item + 4), le32(item + 24));
            return;
        }
        if (item[0] != 0x0F) throw std::runtime_error("flow ring: not a TX post");
        Frame f;
        f.bytes.assign(item + 8, item + 22);
        const uint8_t *payload = va(le32(item + 32));
        f.bytes.insert(f.bytes.end(), payload, payload + le16(item + 42));
        txSeen.push_back(std::move(f));
        txHeld.push_back({le32(item + 4), static_cast<uint16_t>(id)});
    }
};
Firmware gFw;

// ---- the driver around Msgbuf: what SwitchWLAN.cpp and SwitchWLANNet.cpp do with it ----------------------------------
std::vector<Frame> gReceived;
void onFrame(void *, const uint8_t *frame, size_t length, uint8_t)
{
    gCount.copiedBytes += length;  // SwitchWLANNet on_rx: the one copy, into the mbuf
    gReceived.push_back(Frame{std::vector<uint8_t>(frame, frame + length)});
}
void onEvent(void *, const swlan::Event &) {}

swlan::Chip gChip;
swlan::Msgbuf gMb;

// The interrupt: taken as SwitchWLAN::msiFired takes it, as long as the chip raises it.
unsigned gResumes = 0;
std::deque<Frame> gQueue;  // en0's output queue (the stack's)
void start();
void interrupts()
{
    while (gFw.interrupting()) {
        bool halted = false;
        ++gCount.msis;
        gMb.service(&halted);
        if (gMb.txResume()) {
            ++gResumes;
            start();
        }
    }
}
// en0's start routine (SwitchWLANNet if_start over SwitchWLAN::transmitFrames): what the radio takes, in batches of
// at most 64, one copy per frame from the queue into its transmit buffer, one doorbell per batch.
unsigned gBatches = 0;
void start()
{
    for (;;) {
        unsigned room = gMb.txRoom();
        if (!room || gQueue.empty()) return;
        unsigned batch = room < 64 ? room : 64;
        ++gBatches;
        for (; batch && !gQueue.empty() && gMb.txRoom(); --batch) {
            const Frame &f = gQueue.front();
            uint8_t *buffer = gMb.txBuffer();
            memcpy(buffer, f.bytes.data(), f.bytes.size());
            gCount.copiedBytes += f.bytes.size();
            if (!gMb.txPost(f.bytes.size())) throw std::runtime_error("txPost refused with room");
            gQueue.pop_front();
        }
        gMb.txFlush();
    }
}
// The firmware's turn: everything the host rang for.
void firmware()
{
    gFw.serviceH2D();
    interrupts();
}

const uint8_t kAp[6] = {0x02, 0, 0, 0, 0, 1}, kUs[6] = {0x98, 0xb6, 0xe9, 0x5c, 0x10, 0xc8};

bool bringUp()
{
    resetTcm();
    gFw = Firmware{};
    gCount = {};
    gReceived.clear();
    char why[200] = "";
    gChip.attach(nullptr, 0, 0, why, sizeof(why));
    if (!gMb.attach(&gChip, onEvent, onFrame, nullptr, why, sizeof(why))) {
        printf("{\"error\": \"attach: %s\"}\n", why);
        return false;
    }
    gFw.attach();
    gMb.enableInterrupts();
    firmware();
    return true;
}
bool openFlow()
{
    gMb.flowCreateSubmit(kAp, kUs);
    firmware();
    return gMb.flow() == swlan::Msgbuf::FLOW_OPEN;
}
unsigned gPeak = 0;  // the most frames in flight seen (takeCounters starts its peak again each time)
uint32_t inFlight()
{
    const swlan::DataCounters d = gMb.takeCounters();
    gPeak = d.txPeak > gPeak ? d.txPeak : gPeak;
    return d.txInFlight;
}
void resetCounts()
{
    gCount = {};
    gBatches = gResumes = gPeak = 0;
}

void printCounts(const char *scenario, uint64_t frames, const char *extra)
{
    const swlan::DataCounters d = gMb.takeCounters();
    const unsigned peak = d.txPeak > gPeak ? d.txPeak : gPeak;
    printf("{\"scenario\": \"%s\", \"frames\": %llu, \"tcm_reads\": %llu, \"tcm_writes\": %llu, \"reg_reads\": %llu, "
           "\"reg_writes\": %llu, \"doorbells\": %llu, \"msis\": %llu, \"copied_bytes\": %llu, \"index_reads\": %llu, "
           "\"rx_posted\": %u, \"rx_buffers\": %u, \"tx_buffers\": %u, \"tx_in_flight\": %u, \"tx_peak\": %u, "
           "\"tx_full\": %llu, \"tx_refused\": %llu, \"rx_starved\": %llu, \"min_posted\": %u, \"batches\": %u, "
           "\"resumes\": %u%s}\n",
           scenario, static_cast<unsigned long long>(frames), static_cast<unsigned long long>(gCount.tcmReads),
           static_cast<unsigned long long>(gCount.tcmWrites), static_cast<unsigned long long>(gCount.regReads),
           static_cast<unsigned long long>(gCount.regWrites), static_cast<unsigned long long>(gCount.doorbells),
           static_cast<unsigned long long>(gCount.msis), static_cast<unsigned long long>(gCount.copiedBytes),
           static_cast<unsigned long long>(d.indexReads), d.rxPosted, gMb.rxBufferCount(), gMb.txBufferCount(),
           d.txInFlight, peak, static_cast<unsigned long long>(d.txFull), static_cast<unsigned long long>(d.txRefused),
           static_cast<unsigned long long>(gFw.rxStarved), gFw.minPosted == ~0u ? 0 : gFw.minPosted, gBatches, gResumes,
           extra);
}

// ---- scenarios -------------------------------------------------------------------------------------------------------
int attachScenario()
{
    if (!bringUp()) return 1;
    char layout[200], extra[300];
    gMb.describe(layout, sizeof(layout));
    snprintf(extra, sizeof(extra), ", \"posted_at_attach\": %zu, \"lines\": %zu, \"describe\": \"%s\"",
             gFw.rxBuffers.size(), gLog.size(), layout);
    printCounts("attach", 0, extra);
    return 0;
}

// A download: bursts of `burst` frames (an A-MPDU's worth) per interrupt, 1500-byte frames, every one delivered and
// intact, the receive buffers back with the chip after each interrupt.
int rxScenario(unsigned frames, unsigned burst)
{
    if (!bringUp()) return 1;
    resetCounts();
    gReceived.clear();
    unsigned sent = 0;
    while (sent < frames) {
        for (unsigned i = 0; i < burst && sent < frames; ++i, ++sent) gFw.receive(makeFrame(sent, 1514));
        firmware();  // the interrupt, then the reposts it rang for
        firmware();
    }
    bool intact = gReceived.size() == frames;
    for (unsigned i = 0; intact && i < frames; ++i) intact = gReceived[i].bytes == makeFrame(i, 1514).bytes;
    char extra[64];
    snprintf(extra, sizeof(extra), ", \"intact\": %s, \"burst\": %u", intact ? "true" : "false", burst);
    printCounts("rx", frames, extra);
    return 0;
}

// An upload: `frames` queued at once (TCP's window), the firmware fetching at most `hold` and the air completing
// `perAir` per turn. Nothing may be dropped: what does not fit waits in the queue until a completion resumes it.
int txScenario(unsigned frames, unsigned hold, unsigned perAir)
{
    if (!bringUp() || !openFlow()) return 1;
    resetCounts();
    gFw.holdLimit = hold;
    for (unsigned i = 0; i < frames; ++i) gQueue.push_back(makeFrame(i, 1514));
    start();
    for (unsigned turns = 0; (!gQueue.empty() || !gFw.txHeld.empty() || inFlight()) && turns < 100000; ++turns) {
        firmware();
        gFw.transmitted(perAir);
        firmware();
    }
    bool intact = gFw.txSeen.size() == frames && gQueue.empty();
    for (unsigned i = 0; intact && i < frames; ++i) intact = gFw.txSeen[i].bytes == makeFrame(i, 1514).bytes;
    char extra[96];
    snprintf(extra, sizeof(extra), ", \"intact\": %s, \"queued_left\": %zu", intact ? "true" : "false", gQueue.size());
    printCounts("tx", frames, extra);
    return 0;
}

// A download with its ACKs: one 66-byte ACK queued per two frames received, both directions through the same
// interrupts.
int mixedScenario(unsigned frames, unsigned burst)
{
    if (!bringUp() || !openFlow()) return 1;
    resetCounts();
    gReceived.clear();
    gFw.holdLimit = 32;
    unsigned sent = 0, acks = 0;
    while (sent < frames) {
        for (unsigned i = 0; i < burst && sent < frames; ++i, ++sent) gFw.receive(makeFrame(sent, 1514));
        firmware();
        while (acks < sent / 2) gQueue.push_back(makeFrame(100000 + acks++, 66));
        start();
        firmware();
        gFw.transmitted(burst / 2);
        firmware();
    }
    for (unsigned turns = 0; (!gQueue.empty() || !gFw.txHeld.empty()) && turns < 1000; ++turns) {
        firmware();
        gFw.transmitted(64);
        firmware();
    }
    char extra[96];
    snprintf(extra, sizeof(extra), ", \"acks\": %u, \"acks_seen\": %zu, \"received\": %zu", acks, gFw.txSeen.size(),
             gReceived.size());
    printCounts("mixed", static_cast<uint64_t>(frames) + acks, extra);
    return 0;
}

// The supplicant's handshake frame: sent at once, its completion's status read back by id.
int eapolScenario()
{
    if (!bringUp() || !openFlow()) return 1;
    const Frame f = makeFrame(7, 121);
    const uint32_t id = gMb.send(f.bytes.data(), f.bytes.size());
    uint16_t status = 0xFFFF;
    const bool before = gMb.sent(id, &status);
    firmware();
    gFw.transmitted(1, 0);
    firmware();
    const bool after = gMb.sent(id, &status);
    char extra[160];
    snprintf(extra, sizeof(extra),
             ", \"id\": %u, \"sent_before\": %s, \"sent_after\": %s, \"status\": %u, "
             "\"seen\": %s",
             id, before ? "true" : "false", after ? "true" : "false", status,
             gFw.txSeen.size() == 1 && gFw.txSeen[0].bytes == f.bytes ? "true" : "false");
    printCounts("eapol", 1, extra);
    return 0;
}

// No 2 MiB contiguous: the old layout, one line.
int smallScenario()
{
    gFailAllocationsFrom = swlan::kAreaLarge.length;
    gLog.clear();
    if (!bringUp()) return 1;
    std::string log = gLog.empty() ? "" : gLog.front();
    for (char &c : log)
        if (c == '"') c = '\'';
    char extra[400];
    snprintf(extra, sizeof(extra), ", \"posted_at_attach\": %zu, \"lines\": %zu, \"log\": \"%s\"", gFw.rxBuffers.size(),
             gLog.size(), log.c_str());
    printCounts("small", 0, extra);
    return 0;
}

// SwitchWLANLink.h: chanspecs, an access point's elements, the best rate, a dump's counters.
int linkScenario()
{
    swlan_chanspec a, b, c, d;
    const bool okA = swlan_chanspec_decode(0x1006, SWLAN_IOTYPE_D11AC, &a);  // 2.4 GHz 6, 20 MHz
    const bool okB = swlan_chanspec_decode(0xE09B, SWLAN_IOTYPE_D11AC, &b);  // 5 GHz 80 MHz centre 155, ctl 149
    const bool okC = swlan_chanspec_decode(0xD926, SWLAN_IOTYPE_D11AC, &c);  // 5 GHz 40 MHz centre 38, upper: ctl 40
    const bool okD =
        swlan_chanspec_decode(0x2E06, SWLAN_IOTYPE_D11N, &d);  // d11n 2.4 GHz 40 MHz centre 6, upper: ctl 8
    // A VHT80 2x2 access point on 149: HT caps (SGI20/40, 2 streams), HT op (secondary above), VHT caps (SGI80,
    // 2 streams), VHT op (80 MHz, centre 155), HE caps.
    const uint8_t ie[] = {0,    4,    'h', 'o',  'm', 'e', 45, 26,  0x6F, 0x00, 0x17, 0xFF, 0xFF, 0,    0,    0, 0,
                          0,    0,    0,   0,    0,   0,   0,  0,   0,    0,    0,    0,    0,    0,    0,    0, 0,
                          61,   22,   149, 0x05, 0,   0,   0,  0,   0,    0,    0,    0,    0,    0,    0,    0, 0,
                          0,    0,    0,   0,    0,   0,   0,  191, 12,   0x20, 0,    0,    0,    0xFA, 0xFF, 0, 0,
                          0xFA, 0xFF, 0,   0,    192, 5,   1,  155, 0,    0,    0,    255,  3,    35,   0,    0};
    swlan_peer ap;
    swlan_peer_parse(ie, sizeof(ie), &ap);
    char text[160];
    swlan_peer_describe(&ap, text, sizeof(text));
    const char dump[] = "AMPDU Tx counters:\ntxampdu 120 txmpdu 1500 txmpduperampdu 13 noba 0\nAMPDU Rx counters:\n"
                        "rxampdu 400 rxmpdu 9000 rxmpduperampdu 23\n";
    uint32_t txampdu = 0, txmpdu = 0, rxampdu = 0, rxmpdu = 0, missing = 0;
    const bool dumpOk = swlan_dump_value(dump, sizeof(dump), "txampdu", &txampdu) &&
                        swlan_dump_value(dump, sizeof(dump), "txmpdu", &txmpdu) &&
                        swlan_dump_value(dump, sizeof(dump), "rxampdu", &rxampdu) &&
                        swlan_dump_value(dump, sizeof(dump), "rxmpdu", &rxmpdu);
    const bool noMissing = !swlan_dump_value(dump, sizeof(dump), "mpdu", &missing);
    char tenths[16];
    swlan_tenths(txmpdu, txampdu, tenths, sizeof(tenths));
    printf("{\"scenario\": \"link\", \"a\": [%d, %u, %u, %u, %d], \"b\": [%d, %u, %u, %u, %d], "
           "\"c\": [%d, %u, %u, %u, %d], \"d\": [%d, %u, %u, %u, %d], \"peer\": \"%s\", \"best80\": %u, "
           "\"best20\": %u, \"dump\": [%d, %u, %u, %u, %u, %d], \"tenths\": \"%s\"}\n",
           okA, a.control, a.center, a.bw_mhz, a.five_ghz, okB, b.control, b.center, b.bw_mhz, b.five_ghz, okC,
           c.control, c.center, c.bw_mhz, c.five_ghz, okD, d.control, d.center, d.bw_mhz, d.five_ghz, text,
           swlan_link_best_kbps(&ap, 80), swlan_link_best_kbps(&ap, 20), dumpOk, txampdu, txmpdu, rxampdu, rxmpdu,
           noMissing, tenths);
    return 0;
}

}  // namespace

// ---- the Chip methods Msgbuf calls (SwitchWLANChip.cpp's, over the model) ----------------------------------------------
void swlan_test_mmio(const void *, char kind, uint32_t)
{
    ++(kind == 'r' ? gCount.tcmReads : gCount.tcmWrites);
}
namespace swlan {
bool Chip::attach(TegraPcie *, uint32_t, uint32_t, char *, size_t)
{
    tcm_.base = gTcm;
    shared_ = kShared;
    sharedFlags_ = 0x20005;
    coreCount_ = 1;
    cores_[0] = {0x83C, 7, kPcie2Base, kPcie2Base + 0x100000};
    return true;
}
uint32_t Chip::read(uint32_t address)
{
    ++gCount.regReads;
    return gFw.reg(address - kPcie2Base);
}
void Chip::write(uint32_t address, uint32_t value)
{
    ++gCount.regWrites;
    gFw.reg(address - kPcie2Base, value);
}
const Core *Chip::find(uint16_t coreId) const
{
    for (unsigned i = 0; i < coreCount_; ++i)
        if (cores_[i].id == coreId) return &cores_[i];
    return nullptr;
}
}  // namespace swlan

int main(int argc, char **argv)
{
    const std::string what = argc > 1 ? argv[1] : "";
    const unsigned a = argc > 2 ? static_cast<unsigned>(atoi(argv[2])) : 0;
    const unsigned b = argc > 3 ? static_cast<unsigned>(atoi(argv[3])) : 0;
    const unsigned c = argc > 4 ? static_cast<unsigned>(atoi(argv[4])) : 0;
    try {
        if (what == "attach") return attachScenario();
        if (what == "rx") return rxScenario(a ? a : 4000, b ? b : 32);
        if (what == "tx") return txScenario(a ? a : 4000, b ? b : 64, c ? c : 32);
        if (what == "mixed") return mixedScenario(a ? a : 4000, b ? b : 32);
        if (what == "eapol") return eapolScenario();
        if (what == "small") return smallScenario();
        if (what == "link") return linkScenario();
    } catch (const std::exception &e) {
        printf("{\"error\": \"%s\"}\n", e.what());
        return 2;
    }
    fprintf(stderr, "usage: %s attach|rx|tx|mixed|eapol|small|link\n", argv[0]);
    return 64;
}
