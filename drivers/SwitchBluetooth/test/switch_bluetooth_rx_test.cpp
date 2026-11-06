// drivers/SwitchBluetooth's PIO RX path on UART-D (pio_rx_model.h): the RX trigger the driver uses ("more than 16")
// takes fewer interrupts per HCI packet than the old one ("more than 8"), and no packet's last byte waits beyond the RX
// timeout plus the filter's entry and drain, at either trigger. switch_bluetooth_source_test checks the driver programs
// that trigger with hardware flow control on.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "pio_rx_model.h"

namespace {

using Packets = std::vector<std::pair<double, unsigned>>;
using switch_bt_pio::kTimeout;

// Two Joy-Cons' HID input reports 0x30 (H4 1 + ACL 4 + L2CAP 4 + 0xA1 + 49 = 59 bytes) at `hz` each.
Packets joycons(int hz)
{
    Packets packets;
    for (int i = 0; i < 4 * hz; ++i)
        packets.emplace_back(i * (1e6 / (2 * hz)), 59);
    return packets;
}

Packets every(double period, unsigned count, unsigned length)
{
    Packets packets;
    for (unsigned i = 0; i < count; ++i)
        packets.emplace_back(i * period, length);
    return packets;
}

struct PerPacket {
    double interruptsPerPacket;
    double worstWait;
    bool everyLastByteTaken;
};

PerPacket perPacket(const Packets &packets, unsigned trigger)
{
    const switch_bt_pio::Result r = switch_bt_pio::run(packets, trigger);
    PerPacket out{(double)r.interrupts / (double)packets.size(), 0.0, true};
    for (const switch_bt_pio::PacketResult &p : r.perPacket) {
        if (!p.wait) {
            out.everyLastByteTaken = false;
            continue;
        }
        out.worstWait = std::max(out.worstWait, *p.wait);
    }
    return out;
}

TEST(SwitchBluetoothRx, FewerInterruptsPerPacket)
{
    const std::vector<std::tuple<std::string, Packets, double>> cases = {
        {"Joy-Con reports at 2 x 60 Hz", joycons(60), 0.70},
        {"Joy-Con reports at 2 x 120 Hz", joycons(120), 0.70},
        {"LE advertising reports", every(45000, 40, 43), 0.70},
        {"300-byte ACL bursts", every(2000, 50, 300), 0.60},
    };
    for (const auto &[name, packets, ratio] : cases) {
        const double before = perPacket(packets, 8).interruptsPerPacket;
        const double now = perPacket(packets, 16).interruptsPerPacket;
        EXPECT_LE(now, before * ratio) << name << ": " << before << " -> " << now << " interrupts per packet";
    }
}

TEST(SwitchBluetoothRx, NoWaitBeyondTheRxTimeout)
{
    // A packet's last byte is taken by a data interrupt or by the RX timeout, 4 characters after it: never later than
    // that timeout plus the filter's entry and its drain, at either trigger.
    const double bound = kTimeout + 3.0 + 64 * 0.35;
    Packets alternating;
    for (unsigned i = 0; i < 200; ++i)
        alternating.emplace_back(i * 1000.0, 7 + (i & 1));
    for (const Packets &packets : {joycons(60), joycons(120), alternating, every(45000, 40, 43)}) {
        for (unsigned trigger : {8u, 16u}) {
            const PerPacket p = perPacket(packets, trigger);
            EXPECT_LT(p.worstWait, bound) << "trigger " << trigger;
            EXPECT_TRUE(p.everyLastByteTaken) << "every packet's last byte is taken (trigger " << trigger << ")";
        }
        // The new trigger's worst case is the old one's (both end on the RX timeout).
        EXPECT_LE(perPacket(packets, 16).worstWait, perPacket(packets, 8).worstWait + kTimeout);
    }
}

}  // namespace
