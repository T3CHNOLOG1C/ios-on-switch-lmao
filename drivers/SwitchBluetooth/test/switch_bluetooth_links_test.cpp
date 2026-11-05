// SwitchBluetooth's link table and link trace (drivers/SwitchBluetooth/SwitchBluetoothLinks.h) on the host, through
// switch_bluetooth_links_harness.c, with the HCI the r7r test produced (11-05 11:07:42-:46: the left Joy-Con
// connecting and authenticating, the right one connecting while it is up) and with two Joy-Cons and AirPods at once:
// the per-handle credit accounting (packets sent, completed, outstanding, peak) across three links, the stray counts,
// the trace lines, and that a Link Key Request Reply never prints its key.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "switch_bluetooth_links_harness.h"

namespace {

using Bytes = std::vector<uint8_t>;
using Counts = std::array<uint32_t, 4>;  // rx, tx, done, peak
using Strays = std::array<uint32_t, 2>;  // strayTx, strayDone
using testing::HasSubstr;
using testing::Not;

const char *const kLeft = "48:f1:eb:b6:cf:96";
const char *const kRight = "48:f1:eb:b6:9e:d1";
const char *const kAirPods = "34:0e:22:32:9d:ed";
constexpr uint8_t kAcl = 1, kLe = 2;

// A display-order address as HCI carries it (least significant byte first).
Bytes hciAddr(const std::string &text)
{
    Bytes out;
    for (size_t i = 0; i < text.size(); i += 3)
        out.insert(out.begin(), (uint8_t)std::stoul(text.substr(i, 2), nullptr, 16));
    return out;
}

Bytes cat(Bytes a, const Bytes &b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::array<uint8_t, 2> aclHeader(uint16_t handle, unsigned flags = 0x2)
{
    const unsigned value = handle | flags << 12;
    return {(uint8_t)(value & 0xff), (uint8_t)(value >> 8)};
}

std::string event(uint8_t code, const Bytes &params)
{
    char out[200] = {};
    bt_describe_event(out, sizeof(out), code, params.data(), (uint32_t)params.size());
    return out;
}

std::string command(uint16_t opcode, const Bytes &params)
{
    char out[160] = {};
    bt_describe_command(out, sizeof(out), opcode, params.data(), (uint32_t)params.size());
    return out;
}

std::pair<std::optional<Counts>, Strays> counts(uint16_t handle)
{
    uint32_t out[6] = {};
    const bool found = bt_link_counts(handle, out);
    std::optional<Counts> link;
    if (found) link = Counts{out[0], out[1], out[2], out[3]};
    return {link, Strays{out[4], out[5]}};
}

std::optional<Counts> close(uint16_t handle)
{
    uint32_t out[4] = {};
    if (!bt_link_close(handle, out)) return std::nullopt;
    return Counts{out[0], out[1], out[2], out[3]};
}

int completed(const std::vector<std::pair<uint16_t, uint16_t>> &pairs)
{
    Bytes params{(uint8_t)pairs.size()};
    for (const auto &[handle, n] : pairs)
        params.insert(params.end(), {(uint8_t)handle, (uint8_t)(handle >> 8), (uint8_t)n, (uint8_t)(n >> 8)});
    return bt_links_completed(params.data(), (uint32_t)params.size());
}

void tx(uint16_t handle, unsigned flags = 0x2)
{
    const auto header = aclHeader(handle, flags);
    bt_link_tx(header.data());
}

void rx(uint16_t handle, unsigned flags = 0x2)
{
    const auto header = aclHeader(handle, flags);
    bt_link_rx(header.data());
}

bool open(uint16_t handle, uint8_t kind, const std::string &addr, uint8_t role)
{
    return bt_link_open(handle, kind, hciAddr(addr).data(), role);
}

void note(const Bytes &addr, uint32_t cod)
{
    bt_request_note(addr.data(), cod);
}

// Accept Connection Request parameters (address, role) through the rule: (changed, the role byte after, the class).
std::tuple<int, int, uint32_t> accept(const Bytes &addr, uint8_t role)
{
    Bytes params = cat(addr, {role});
    uint32_t cod = 0;
    const int changed = bt_accept_as_central(params.data(), 7, &cod);
    return {changed, params[6], cod};
}
std::tuple<int, int, uint32_t> accept(const std::string &addr, uint8_t role)
{
    return accept(hciAddr(addr), role);
}

class Links : public testing::Test {
protected:
    void SetUp() override
    {
        bt_links_reset();
        bt_requests_reset();
    }
};

// --- The trace -----------------------------------------------------------------------------------------------------
TEST_F(Links, LeftJoyConConnectionAsR7rSawIt)
{
    const std::string left = kLeft;
    const Bytes request = cat(hciAddr(left), {0x08, 0x25, 0x00, 0x01});  // class 0x002508 (gamepad), ACL
    EXPECT_EQ(event(0x04, request), "Connection Request " + left + " class 002508 type 1");
    EXPECT_EQ(command(0x0409, cat(hciAddr(left), {0x00})), "Accept Connection Request " + left + " role 0");
    EXPECT_EQ(event(0x12, cat(cat({0x00}, hciAddr(left)), {0x00})), "Role Change " + left + " status 0x00 role 0");
    const Bytes complete = cat(cat({0x00, 0x0b, 0x00}, hciAddr(left)), {0x01, 0x00});
    EXPECT_EQ(event(0x03, complete), "Connection Complete 000b " + left + " status 0x00 type 1 encryption 0");
    EXPECT_EQ(event(0x17, hciAddr(left)), "Link Key Request " + left);
    EXPECT_EQ(event(0x08, {0x00, 0x0b, 0x00, 0x01}), "Encryption Change 000b status 0x00 on 1");
    EXPECT_EQ(event(0x14, {0x00, 0x0b, 0x00, 0x02, 0x18, 0x00}), "Mode Change 000b status 0x00 mode 2 interval 24");
    // The right Joy-Con, 11:07:43.364: its own end of the link (Remote User Terminated Connection).
    EXPECT_EQ(event(0x05, {0x00, 0x0c, 0x00, 0x13}), "Disconnection Complete 000c status 0x00 reason 0x13");
}

TEST_F(Links, LinkKeyReplyNeverPrintsTheKey)
{
    const std::string right = kRight;
    Bytes key;
    for (unsigned b = 0xa0; b < 0xb0; ++b) key.push_back((uint8_t)b);
    std::string line = command(0x040b, cat(hciAddr(right), key));
    EXPECT_EQ(line, "Link Key Request Reply " + right);
    EXPECT_THAT(line, Not(HasSubstr("a0a1")));
    line = event(0x18, cat(cat(hciAddr(right), key), {0x04}));
    EXPECT_EQ(line, "Link Key Notification " + right + " key type 4");
    EXPECT_THAT(line, Not(HasSubstr("a0a1")));
}

TEST_F(Links, CommandLines)
{
    EXPECT_EQ(command(0x0803, {0x0b, 0x00, 0x18, 0x00, 0x0c, 0x00, 0x02, 0x00, 0x01, 0x00}),
              "Sniff Mode 000b interval 12-24 attempt 2 timeout 1");
    EXPECT_EQ(command(0x0c1a, {0x02}), "Write Scan Enable 0x02 (inquiry scan 0, page scan 1)");
    EXPECT_EQ(command(0x080d, {0x0b, 0x00, 0x05, 0x00}), "Write Link Policy Settings 000b policy 0x0005");
    EXPECT_EQ(command(0x080f, {0x05, 0x00}), "Write Default Link Policy Settings policy 0x0005");
    EXPECT_EQ(command(0x0c05, {0x02, 0x00, 0x02}), "Set Event Filter [020002]");
    EXPECT_EQ(command(0x0406, {0x0c, 0x00, 0x16}), "Disconnect 000c reason 0x16");
    EXPECT_EQ(command(0x0409, {0x01}), "Accept Connection Request (0409) of 1 bytes, too short");
    EXPECT_TRUE(bt_command_traced(0x0409));
    EXPECT_TRUE(bt_command_traced(0x040b));
    EXPECT_FALSE(bt_command_traced(0x0c35));  // Host Number Of Completed Packets: every packet
    EXPECT_FALSE(bt_command_traced(0x200c));  // LE: the once-a-boot trace has those
}

TEST_F(Links, LeConnectionComplete)
{
    const Bytes params = cat(cat({0x01, 0x00, 0x40, 0x00, 0x00, 0x01}, hciAddr(kAirPods)), {0x18, 0, 0, 0, 0x48, 0, 0});
    EXPECT_EQ(event(0x3e, params), std::string("LE Connection Complete 0040 ") + kAirPods +
                                       " (type 1) status 0x00 role 0 interval 24 latency 0 timeout 72");
    EXPECT_THAT(event(0x3e, Bytes(params.begin(), params.begin() + 10)), HasSubstr("too short"));
}

TEST_F(Links, ShortEventsSaySo)
{
    EXPECT_EQ(event(0x03, {0x00, 0x0b}), "Connection Complete (0x03) of 2 bytes, too short");
    EXPECT_EQ(event(0x10, {0x07}), "Hardware Error 0x07");
    EXPECT_EQ(event(0x1a, {0x01}), "Data Buffer Overflow 0x01");
}

TEST_F(Links, WhichEventsAreTakenWhole)
{
    for (uint8_t code : {0x03, 0x04, 0x05, 0x06, 0x08, 0x10, 0x12, 0x13, 0x14, 0x17, 0x18, 0x1a, 0x1b, 0x38})
        EXPECT_TRUE(bt_event_wanted(code, 0)) << std::hex << (unsigned)code;
    for (uint8_t code : {0x0e, 0x0f, 0xff, 0x0b, 0x2f})  // completions (matched whole already), vendor, features, EIR
        EXPECT_FALSE(bt_event_wanted(code, 0)) << std::hex << (unsigned)code;
    EXPECT_TRUE(bt_event_wanted(0x3e, 0x01));
    EXPECT_TRUE(bt_event_wanted(0x3e, 0x0a));
    EXPECT_FALSE(bt_event_wanted(0x3e, 0x02));  // advertising reports: hundreds a second, never held back
}

// --- Incoming gamepads accepted as central -------------------------------------------------------------------------
TEST_F(Links, Classes)
{
    EXPECT_TRUE(bt_cod_is_gamepad(0x000508));   // Joy-Con (L), (R), r7v Connection Requests
    EXPECT_TRUE(bt_cod_is_gamepad(0x002508));   // gamepad with Limited Discoverable / other service bits
    EXPECT_TRUE(bt_cod_is_gamepad(0x000504));   // joystick
    EXPECT_FALSE(bt_cod_is_gamepad(0x240418));  // AirPods: audio, headphones
    EXPECT_FALSE(bt_cod_is_gamepad(0x002540));  // keyboard
    EXPECT_FALSE(bt_cod_is_gamepad(0x002580));  // mouse
    EXPECT_FALSE(bt_cod_is_gamepad(0x5a020c));  // phone
}

TEST_F(Links, JoyConAcceptedAsCentral)
{
    note(hciAddr(kRight), 0x000508);
    EXPECT_EQ(accept(kRight, 1), std::make_tuple(1, 0, 0x000508u));
    // The request is consumed: a second Accept (bluetoothd retrying) is left alone.
    EXPECT_EQ(accept(kRight, 1), std::make_tuple(0, 1, 0u));
}

TEST_F(Links, OthersLeftAlone)
{
    note(hciAddr(kAirPods), 0x240418);
    EXPECT_EQ(accept(kAirPods, 1), std::make_tuple(0, 1, 0u));
    note(hciAddr(kLeft), 0x000508);
    EXPECT_EQ(accept(kLeft, 0), std::make_tuple(0, 0, 0u));   // bluetoothd asked to become central itself
    EXPECT_EQ(accept(kRight, 1), std::make_tuple(0, 1, 0u));  // no Connection Request seen for it
    Bytes shortParams = cat(hciAddr(kRight), {1});
    EXPECT_EQ(bt_accept_as_central(shortParams.data(), 6, nullptr), 0);
}

TEST_F(Links, RequestsKeptPerAddress)
{
    note(hciAddr(kLeft), 0x000508);   // slot 0
    note(hciAddr(kRight), 0x000508);  // slot 1
    note(hciAddr(kLeft), 0x240418);   // the same address again: slot 0 updated, not a second entry
    EXPECT_EQ(accept(kLeft, 1), std::make_tuple(0, 1, 0u));  // its newest class (audio) decides, and it is consumed
    note(hciAddr(kLeft), 0x000508);                          // slot 2
    for (uint8_t i = 0; i < 2; ++i) note({i, 1, 2, 3, 4, 5}, 0x000508);  // slots 3 and 0 (the oldest): four at most
    EXPECT_EQ(accept(kRight, 1), std::make_tuple(1, 0, 0x000508u));
    EXPECT_EQ(accept(kLeft, 1), std::make_tuple(1, 0, 0x000508u));
    EXPECT_EQ(accept("05:04:03:02:01:01", 1), std::make_tuple(1, 0, 0x000508u));
    EXPECT_EQ(accept("05:04:03:02:01:00", 1), std::make_tuple(1, 0, 0x000508u));
}

// --- The table -----------------------------------------------------------------------------------------------------
TEST_F(Links, TwoJoyConsAndAirPodsShareTheCredits)
{
    EXPECT_TRUE(open(0x000b, kAcl, kLeft, 0));
    EXPECT_TRUE(open(0x000c, kAcl, kRight, 0));
    EXPECT_TRUE(open(0x000d, kAcl, kAirPods, 0));
    // 30 HID output reports to each Joy-Con, 200 A2DP packets to the AirPods; input reports from the Joy-Cons.
    for (int i = 0; i < 30; ++i) {
        tx(0x000b);
        tx(0x000c);
        rx(0x000b);
        rx(0x000c, 0x2);
    }
    for (int i = 0; i < 200; ++i) tx(0x000d, 0x2);
    tx(0x000d, 0x1);  // a continuation fragment takes a buffer as well
    // The controller completes them in one event per round, several handles each.
    EXPECT_EQ(completed({{0x000b, 30}, {0x000c, 29}, {0x000d, 195}}), 254);
    EXPECT_EQ(counts(0x000b).first, (Counts{30, 30, 30, 30}));
    EXPECT_EQ(counts(0x000c).first, (Counts{30, 30, 29, 30}));
    EXPECT_EQ(counts(0x000d).first, (Counts{0, 201, 195, 201}));
    // The right Joy-Con goes: its last counts say one packet never came back.
    EXPECT_EQ(close(0x000c), (Counts{30, 30, 29, 30}));
    EXPECT_EQ(close(0x000c), std::nullopt);
    // A completion for it after its Disconnection Complete is a stray, not a negative count elsewhere.
    EXPECT_EQ(completed({{0x000c, 1}}), 1);
    EXPECT_EQ(counts(0x000b).second, (Strays{0, 1}));
}

TEST_F(Links, PeakIsTheMostEverOutstanding)
{
    open(0x0010, kAcl, kAirPods, 0);
    for (int i = 0; i < 8; ++i) tx(0x0010);
    completed({{0x0010, 6}});
    for (int i = 0; i < 3; ++i) tx(0x0010);
    EXPECT_EQ(counts(0x0010).first, (Counts{0, 11, 6, 8}));
}

TEST_F(Links, HandlesReusedAndTableFull)
{
    for (uint16_t handle = 0; handle < 8; ++handle) EXPECT_TRUE(open(handle, kAcl, kLeft, 0xff));
    EXPECT_FALSE(open(0x0100, kLe, kRight, 1));
    tx(0x0100);
    EXPECT_EQ(counts(0x0003).second, (Strays{1, 0}));
    tx(0x0003);
    EXPECT_TRUE(open(0x0003, kAcl, kRight, 0));  // the same handle again: counts restart
    EXPECT_EQ(counts(0x0003).first, (Counts{0, 0, 0, 0}));
}

TEST_F(Links, MalformedCompletedPackets)
{
    open(0x000b, kAcl, kLeft, 0);
    const uint8_t twoClaimedOneGiven[] = {0x02, 0x0b, 0x00, 0x01, 0x00};
    EXPECT_EQ(bt_links_completed(twoClaimedOneGiven, 5), -1);
    const uint8_t empty[1] = {};
    EXPECT_EQ(bt_links_completed(empty, 0), -1);
    EXPECT_EQ(counts(0x000b).first, (Counts{0, 0, 0, 0}));
    // Handle bits above 11 (flags) are not part of the handle.
    const uint8_t flagged[] = {0x01, 0x0b, 0x30, 0x02, 0x00};
    EXPECT_EQ(bt_links_completed(flagged, 5), 2);
    EXPECT_EQ(counts(0x000b).first, (Counts{0, 0, 2, 0}));
}

}  // namespace
