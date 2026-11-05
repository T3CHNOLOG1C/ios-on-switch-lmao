// drivers/SwitchBluetooth's RX by APB DMA (SwitchBluetooth.cpp header "RX by APB DMA", SwitchBluetoothRxDma.h) as a
// byte-timed host model of UART-D, driven by the driver's own framing and transfer-length code (the header compiled as
// C, switch_bluetooth_rx_dma_harness.c), at the peak load: two Joy-Cons' 59-byte HID reports at 120 Hz each, AirPods'
// A2DP (300-byte ACL bursts and the Number Of Completed Packets events for what we send), and LE scanning for AirDrop
// (43-byte advertising reports), all at once, the controller sending packets back to back whenever they queue up.
//
// Against the PIO path as it is (pio_rx_model.h, RX trigger 16) it checks: every byte reaches the ring once and in
// order; the FIFO never needs RTS to hold the chip; far fewer interrupts and far less interrupt time on CPU0; and no HID
// report's last byte waits longer than under PIO (an isolated packet is taken by EORD, 4 characters after its last
// byte, as PIO's RX timeout; one followed at once by another by the end of count of a transfer that ends 5 bytes past
// it, where PIO's 16-byte trigger needs up to 16 more). Also: framing lost keeps transfers at the reach; a transfer's
// visible words never go back. switch_bluetooth_source_test guards that the driver uses the header and programs DMA
// mode as modelled.
//
// Costs (microseconds, assumptions, the same entry/exit as the PIO model): interrupt entry 3.0 and exit 1.0; a UART or
// APB DMA register read 0.25, write 0.1; a byte taken from memory 0.01, a cache line invalidated 0.03.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "bazel/testing/require.h"
#include "pio_rx_model.h"
#include "switch_bluetooth_rx_dma_harness.h"

namespace {

using Bytes = std::vector<uint8_t>;
using switch_bt_pio::kByte;  // us per byte at 3 Mbaud 8N1
using switch_bt_pio::kInf;
using switch_bt_pio::kTimeout;  // 4 characters: the RX timeout (PIO) and EORD (DMA mode)

constexpr double kEntry = 3.0, kExit = 1.0;
constexpr double kRead = 0.25, kWrite = 0.1, kMem = 0.01, kLine = 0.03;
constexpr double kDmaLatency = 0.5;  // a byte from the FIFO to memory once the channel may take it
constexpr size_t kFifo = 32;
constexpr double kSeconds = 1.0;

// --- Traffic -------------------------------------------------------------------------------------------------------
Bytes acl(uint16_t handle, const Bytes &payload)
{
    Bytes out{2, (uint8_t)(handle & 0xFF), (uint8_t)(handle >> 8 | 0x20), (uint8_t)(payload.size() & 0xFF),
              (uint8_t)(payload.size() >> 8)};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Bytes event(uint8_t code, const Bytes &params)
{
    Bytes out{4, code, (uint8_t)params.size()};
    out.insert(out.end(), params.begin(), params.end());
    return out;
}

Bytes zeros(size_t n)
{
    return Bytes(n, 0);
}

Bytes cat(Bytes a, const Bytes &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

enum Kind { kHid, kAclBurst, kNocp, kAdv };

struct Packet {
    double due;
    Kind kind;
    Bytes data;
};

// The peak mix, sorted by nominal start (us). Python's random.Random(1) stream is not reproduced; the mix is: the same
// rates, jitters and distributions from a fixed std::mt19937_64 seed.
std::vector<Packet> peak(double seconds = kSeconds, uint64_t seed = 1)
{
    std::mt19937_64 rng(seed);
    auto uniform = [&](double a, double b) { return std::uniform_real_distribution<double>(a, b)(rng); };
    std::vector<Packet> packets;
    for (const auto &[handle, phase] : {std::pair<uint16_t, double>{0x0040, 0.0}, {0x0041, 4100.0}}) {
        // two Joy-Cons, 120 Hz, a 0x30 report each
        for (double t = phase; t < seconds * 1e6; t += 1e6 / 120)
            packets.push_back({t + uniform(-400, 400), kHid, acl(handle, cat({0xA1, 0x30}, zeros(52)))});
    }
    for (double t = 0.0; t < seconds * 1e6; t += 1e6 / 150) {
        // AirPods: 300-byte ACL bursts and completions for what we send, ~150 a second each
        packets.push_back({t + uniform(0, 500), kAclBurst, acl(0x0042, zeros(295))});
        packets.push_back({t + uniform(0, 6000), kNocp, event(0x13, {1, 0x42, 0, 1, 0})});
    }
    std::exponential_distribution<double> gap(300 / 1e6);  // LE scanning in a crowded room: ~300 reports a second
    for (double t = 0.0; t < seconds * 1e6;) {
        t += gap(rng);
        packets.push_back({t, kAdv, event(0x3E, cat({2, 1}, zeros(38)))});
    }
    std::stable_sort(packets.begin(), packets.end(), [](const Packet &a, const Packet &b) { return a.due < b.due; });
    return packets;
}

struct Arrival {
    double at;
    size_t packet;
    bool last;
    uint8_t value;
};

struct Span {
    double start;
    unsigned length;
    Kind kind;
};

// The controller sends one packet at a time: each starts when it is due or when the last has gone, back to back.
void serialize(const std::vector<Packet> &packets, std::vector<Arrival> &arrivals, std::vector<Span> &spans)
{
    double free = 0.0;
    for (size_t index = 0; index < packets.size(); ++index) {
        const Packet &p = packets[index];
        const double start = std::max(p.due, free);
        for (size_t b = 0; b < p.data.size(); ++b)
            arrivals.push_back({start + (double)(b + 1) * kByte, index, b == p.data.size() - 1, p.data[b]});
        free = start + (double)p.data.size() * kByte;
        spans.push_back({start, (unsigned)p.data.size(), p.kind});
    }
}

// --- DMA mode ------------------------------------------------------------------------------------------------------
struct DmaRun {
    std::vector<std::optional<double>> takenAt;  // per byte, when the CPU took it
    unsigned eordIrqs = 0, eocIrqs = 0;
    double cpu = 0.0;
    size_t fifoPeak = 0;
    std::vector<size_t> order;  // the bytes in the order taken
};

// The driver's DMA mode on one CPU: a transfer runs from word 0 with the length the header gives; the CPU takes what
// moved at EORD (4 characters of idle after data) and at the end of count, which also starts the next transfer.
DmaRun simulateDma(const std::vector<Arrival> &arrivals)
{
    bt_frame_reset();
    const size_t n = arrivals.size();
    DmaRun run;
    run.takenAt.assign(n, std::nullopt);
    std::deque<size_t> fifo;
    std::vector<size_t> trans;
    size_t length = bt_rx_dma_length();
    size_t taken = 0;
    std::optional<double> eoc;   // time the transfer's last word landed (end of count pending)
    std::optional<double> eord;  // time EORD latched (line idle 4 characters after data)
    double lastRx = -1e18;
    bool dataSince = false;
    double cpuFree = 0.0;
    double nextFree = 0.0;  // when the channel can move the next byte
    size_t i = 0;

    auto move = [&](double now) {
        double t = std::max(now, nextFree);
        while (!fifo.empty() && trans.size() < length) {
            t += kDmaLatency;
            trans.push_back(fifo.front());
            fifo.pop_front();
            if (trans.size() == length) eoc = t;
        }
        nextFree = t;
    };
    auto take = [&](double now) {
        const size_t count = trans.size() - taken;
        for (size_t at = taken; at < trans.size(); ++at) {
            const size_t k = trans[at];
            run.takenAt[k] = now;
            run.order.push_back(k);
            bt_frame_byte(arrivals[k].value);
        }
        taken = trans.size();
        return (double)count * kMem + (double)((count * 4 + 63) / 64) * kLine;
    };

    for (;;) {
        const double tArr = i < n ? arrivals[i].at : kInf;
        double tIrq = kInf;
        if (eoc) tIrq = std::min(tIrq, *eoc);
        if (eord) tIrq = std::min(tIrq, *eord);
        if (dataSince && !eord) tIrq = std::min(tIrq, lastRx + kTimeout);
        const double start = std::max(tIrq, cpuFree);
        if (tArr == kInf && tIrq == kInf) break;
        if (tArr <= start) {
            if (dataSince && !eord && tArr >= lastRx + kTimeout) eord = lastRx + kTimeout;
            fifo.push_back(i);
            run.fifoPeak = std::max(run.fifoPeak, fifo.size());
            lastRx = tArr;
            dataSince = true;
            move(tArr);
            ++i;
            continue;
        }
        if (dataSince && !eord && lastRx + kTimeout <= start) eord = lastRx + kTimeout;
        const double now = start + kEntry;
        double cost;
        if (eoc && (!eord || *eoc <= *eord)) {  // the channel's end of count
            ++run.eocIrqs;
            cost = 2 * kRead + kWrite + take(now);
            trans.clear();
            taken = 0;
            eoc.reset();
            length = bt_rx_dma_length();
            cost += kRead + 2 * kWrite + 7 * kWrite;  // stop (CSR, STATUS) and the seven writes that start a transfer
            move(now + cost);
        } else {  // EORD: mask/unmask (3 IER writes), IIR twice, the channel's count and status, LSR
            ++run.eordIrqs;
            cost = 2 * kRead + 3 * kWrite + 2 * kRead + kRead;
            cost += take(now);
            eord.reset();
            dataSince = lastRx > (start - kTimeout) && lastRx + kTimeout > start;
        }
        run.cpu += kEntry + cost + kExit;
        cpuFree = now + cost + kExit;
    }
    return run;
}

// The last-byte waits of every packet of `kind`.
std::vector<double> latencies(const std::vector<Arrival> &arrivals, const std::vector<Span> &spans,
                              const std::vector<std::optional<double>> &takenAt, Kind kind)
{
    std::vector<double> out;
    for (size_t k = 0; k < arrivals.size(); ++k) {
        if (!arrivals[k].last || spans[arrivals[k].packet].kind != kind) continue;
        REQUIRE(takenAt[k].has_value());
        out.push_back(*takenAt[k] - arrivals[k].at);
    }
    return out;
}

struct PioRun {
    unsigned interrupts;
    double cpu;
    std::vector<std::optional<double>> waits;  // per packet
};

// PIO as it is (pio_rx_model.h at the 16-byte trigger) on the same serialized stream.
PioRun pio(const std::vector<Span> &spans)
{
    std::vector<std::pair<double, unsigned>> packets;
    unsigned total = 0;
    for (const Span &s : spans) {
        packets.emplace_back(s.start, s.length);
        total += s.length;
    }
    const switch_bt_pio::Result r = switch_bt_pio::run(packets, 16);
    PioRun out{r.interrupts, 0.0, {}};
    out.cpu = r.interrupts * (kEntry + kExit + 2 * kRead) + total * 0.35;  // IIR twice per interrupt; LSR+RBR per byte
    for (const switch_bt_pio::PacketResult &p : r.perPacket) out.waits.push_back(p.wait);
    return out;
}

double percentile(std::vector<double> values, double q)
{
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, (size_t)(q * (double)values.size()))];
}

class PeakLoad : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        arrivals = new std::vector<Arrival>;
        spans = new std::vector<Span>;
        serialize(peak(), *arrivals, *spans);
        dma = new DmaRun(simulateDma(*arrivals));
        pioRun = new PioRun(pio(*spans));
        const std::vector<double> hidDma = latencies(*arrivals, *spans, dma->takenAt, kHid);
        const std::vector<double> hidPio = pioHid();
        unsigned total = 0;
        for (const Span &s : *spans) total += s.length;
        const double busy = total * kByte / (kSeconds * 1e6);
        std::printf(
            "Bluetooth RX at peak (%zu packets/s, line %.0f%% busy): PIO %u interrupts/s, %.2f%% of CPU0; DMA %u "
            "interrupts/s (%u EORD, %u end of count), %.2f%% of CPU0; FIFO peak %zu; HID report last-byte wait us "
            "PIO p50/p99/max %.1f/%.1f/%.1f, DMA %.1f/%.1f/%.1f\n",
            spans->size(), busy * 100, pioRun->interrupts, pioRun->cpu / 1e4, dma->eordIrqs + dma->eocIrqs,
            dma->eordIrqs, dma->eocIrqs, dma->cpu / 1e4, dma->fifoPeak, percentile(hidPio, 0.5),
            percentile(hidPio, 0.99), *std::max_element(hidPio.begin(), hidPio.end()), percentile(hidDma, 0.5),
            percentile(hidDma, 0.99), *std::max_element(hidDma.begin(), hidDma.end()));
    }
    static void TearDownTestSuite()
    {
        delete arrivals;
        delete spans;
        delete dma;
        delete pioRun;
    }
    // The PIO last-byte waits of the HID reports (every one is taken: NoWaitBeyondTheRxTimeout's model property).
    static std::vector<double> pioHid()
    {
        std::vector<double> out;
        for (size_t i = 0; i < spans->size(); ++i) {
            if ((*spans)[i].kind != kHid) continue;
            REQUIRE(pioRun->waits[i].has_value());
            out.push_back(*pioRun->waits[i]);
        }
        return out;
    }

    static std::vector<Arrival> *arrivals;
    static std::vector<Span> *spans;
    static DmaRun *dma;
    static PioRun *pioRun;
};
std::vector<Arrival> *PeakLoad::arrivals;
std::vector<Span> *PeakLoad::spans;
DmaRun *PeakLoad::dma;
PioRun *PeakLoad::pioRun;

TEST_F(PeakLoad, EveryByteOnceInOrder)
{
    ASSERT_EQ(dma->order.size(), arrivals->size());
    for (size_t k = 0; k < dma->order.size(); ++k) ASSERT_EQ(dma->order[k], k);
    for (const std::optional<double> &t : dma->takenAt) ASSERT_TRUE(t.has_value());
}

TEST_F(PeakLoad, FifoNeverNeedsRts)
{
    EXPECT_LT(dma->fifoPeak, kFifo / 2);
}

TEST_F(PeakLoad, FewerInterruptsAndLessCpu)
{
    EXPECT_LT(dma->eordIrqs + dma->eocIrqs, 0.6 * pioRun->interrupts);
    EXPECT_LT(dma->cpu, 0.35 * pioRun->cpu);
}

TEST_F(PeakLoad, NoHidReportWaitsLonger)
{
    const std::vector<double> hidDma = latencies(*arrivals, *spans, dma->takenAt, kHid);
    const std::vector<double> hidPio = pioHid();
    EXPECT_LE(*std::max_element(hidDma.begin(), hidDma.end()), *std::max_element(hidPio.begin(), hidPio.end()));
    EXPECT_LE(percentile(hidDma, 0.99), percentile(hidPio, 0.99));
    // The bound by construction: EORD 4 characters on, or a transfer's end the reach on, then the interrupt, behind
    // at most one other handler.
    const double bound = std::max(kTimeout, bt_rx_dma_reach() * kByte) + 2 * (kEntry + kExit + 4.0) + kDmaLatency;
    std::vector<double> everyReport = hidDma;
    const std::vector<double> adv = latencies(*arrivals, *spans, dma->takenAt, kAdv);
    everyReport.insert(everyReport.end(), adv.begin(), adv.end());
    EXPECT_LT(*std::max_element(everyReport.begin(), everyReport.end()), bound);
}

// --- Transfers -----------------------------------------------------------------------------------------------------
class Transfers : public testing::Test {
protected:
    void SetUp() override { bt_frame_reset(); }
    static std::vector<int> feed(const Bytes &data)
    {
        std::vector<int> out;
        for (uint8_t b : data) out.push_back(bt_frame_byte(b));
        return out;
    }
};

TEST_F(Transfers, LengthsFollowTheFraming)
{
    const uint32_t reach = bt_rx_dma_reach();
    EXPECT_EQ(bt_rx_dma_length(), reach);  // between packets
    const Bytes report = acl(0x40, zeros(54));
    feed(
        Bytes(report.begin(), report.begin() + 16));  // a transfer of 16 ended inside the report: its end and the reach
    EXPECT_EQ(bt_rx_dma_length(), report.size() - 16 + reach);
    EXPECT_EQ(feed(Bytes(report.begin() + 16, report.end())).back(), 1);
    EXPECT_EQ(bt_rx_dma_length(), reach);
    const Bytes ev = event(0x0E, zeros(4));
    feed(Bytes(ev.begin(), ev.begin() + 2));  // in an event's header: its remainder (the length byte), then the reach
    EXPECT_EQ(bt_rx_dma_length(), 1 + reach);
    const Bytes big = acl(0x40, zeros(4000));
    bt_frame_reset();
    feed(Bytes(big.begin(), big.begin() + 5));
    EXPECT_EQ(bt_rx_dma_length(), bt_rx_dma_words());  // within the buffer
}

TEST_F(Transfers, LostFramingKeepsTheReach)
{
    feed({0x07});  // no H4 packet type
    EXPECT_EQ(bt_rx_dma_length(), bt_rx_dma_reach());
    bt_frame_reset();
    const Bytes report = acl(0x40, zeros(100));
    feed(Bytes(report.begin(), report.begin() + 10));
    bt_frame_lose();
    EXPECT_EQ(bt_rx_dma_length(), bt_rx_dma_reach());
}

TEST_F(Transfers, PacketEnds)
{
    const Bytes stream =
        cat(cat(cat(event(0x0E, {1, 3, 0x0C, 0}), acl(0x40, zeros(3))), event(0x13, {})), {3, 1, 0, 2, 9, 9});
    std::vector<size_t> ends;
    const std::vector<int> done = feed(stream);
    for (size_t i = 0; i < done.size(); ++i)
        if (done[i]) ends.push_back(i);
    EXPECT_EQ(ends, (std::vector<size_t>{6, 14, 17, 23}));
}

TEST_F(Transfers, VisibleWords)
{
    EXPECT_EQ(bt_rx_dma_visible(40, 12 * 4, 0, 0), 12u);
    EXPECT_EQ(bt_rx_dma_visible(40, 0, 1, 12), 40u);      // ended: the whole transfer
    EXPECT_EQ(bt_rx_dma_visible(40, 0, 0, 12), 12u);      // the count read as reloaded: never back
    EXPECT_EQ(bt_rx_dma_visible(40, 4 * 60, 0, 0), 40u);  // never past the transfer
}

}  // namespace
