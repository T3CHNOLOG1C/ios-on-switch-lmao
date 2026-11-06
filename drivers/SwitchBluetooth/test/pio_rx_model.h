// drivers/SwitchBluetooth's RX path on UART-D by PIO, as a host model (no driver code runs here): interrupts per HCI
// packet and how long a packet's last byte waits in the FIFO, for an RX trigger (the driver's "more than 16", FCR
// 0xC0, Linux serial-tegra's PIO setting, or the old "more than 8", FCR 0x80). Shared by switch_bluetooth_rx_test and
// switch_bluetooth_rx_dma_test (the PIO baseline at the peak load).
//
// UART (Tegra 16550, 32-byte FIFO, 3 Mbaud 8N1): a byte lands every 10 bit times (3.33 us). The data interrupt is
// raised once the FIFO holds more than the trigger; the RX timeout once it holds anything and no byte has arrived for 4
// character times. The filter (SwitchBluetooth::filter -> drainRx) starts `entry` after the line rises, reads LSR+RBR
// per byte (`perByte`), keeps going while LSR shows data (so it takes bytes that land meanwhile), re-reads IIR and
// returns (`exit`). RTS flow control is not modelled: it only pauses the chip at a full FIFO, which these rates never
// reach.
#pragma once
#include <algorithm>
#include <cstddef>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace switch_bt_pio {

constexpr double kByte = 10 / 3e6 * 1e6;  // us per byte at 3 Mbaud, 8N1
constexpr double kTimeout = 4 * kByte;    // the RX timeout: 4 characters
constexpr double kInf = std::numeric_limits<double>::infinity();

// A packet's interrupts and the wait of its last byte (none if it was never taken).
struct PacketResult {
    unsigned interrupts = 0;
    std::optional<double> wait;
};

struct Result {
    unsigned interrupts = 0;
    std::vector<PacketResult> perPacket;
};

// packets: (start us, length). The model, step for step.
inline Result run(const std::vector<std::pair<double, unsigned>> &packets, unsigned trigger, double entry = 3.0,
                  double perByte = 0.35, double exitCost = 1.0)
{
    struct Arrival {
        double at;
        size_t packet;
        bool last;
    };

    std::vector<Arrival> arrivals;
    for (size_t i = 0; i < packets.size(); ++i)
        for (unsigned b = 0; b < packets[i].second; ++b)
            arrivals.push_back({packets[i].first + (b + 1) * kByte, i, b == packets[i].second - 1});
    std::sort(arrivals.begin(), arrivals.end(), [](const Arrival &a, const Arrival &b) {
        return std::tie(a.at, a.packet, a.last) < std::tie(b.at, b.packet, b.last);
    });
    std::deque<Arrival> fifo;
    double t = 0.0;
    size_t k = 0;
    Result result;
    result.perPacket.resize(packets.size());
    double busyUntil = 0.0;
    double lastArrival = -1e9;
    while (k < arrivals.size() || !fifo.empty()) {
        // Next byte arrival, and when the interrupt line would rise with the FIFO as it is.
        const double nextArrival = k < arrivals.size() ? arrivals[k].at : kInf;
        double rise = kInf;
        if (fifo.size() > trigger)
            rise = std::max(t, busyUntil);
        else if (!fifo.empty())
            rise = std::max(lastArrival + kTimeout, busyUntil);
        if (nextArrival <= rise) {
            t = nextArrival;
            fifo.push_back(arrivals[k]);
            lastArrival = t;
            ++k;
            continue;
        }
        // The interrupt: the filter drains until the FIFO is empty, taking bytes that land while it runs.
        t = rise + entry;
        ++result.interrupts;
        std::set<size_t> touched;
        for (;;) {
            while (k < arrivals.size() && arrivals[k].at <= t) {
                fifo.push_back(arrivals[k]);
                lastArrival = arrivals[k].at;
                ++k;
            }
            if (fifo.empty())
                break;
            const Arrival byte = fifo.front();
            fifo.pop_front();
            t += perByte;
            touched.insert(byte.packet);
            if (byte.last)
                result.perPacket[byte.packet].wait = t - byte.at;
        }
        for (size_t packet : touched)
            ++result.perPacket[packet].interrupts;
        t += exitCost;
        busyUntil = t;
    }
    return result;
}

}  // namespace switch_bt_pio
