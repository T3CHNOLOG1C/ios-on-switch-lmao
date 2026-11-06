// SPDX-License-Identifier: GPL-2.0-only
// SwitchPDAltModeSteps.h against a model BM92T36 with a partner behind it: Nintendo's dock, USB-C DisplayPort
// adapters, a plain USB-PD charger.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../SwitchPDAltModeSteps.h"

namespace {

constexpr uint16_t kInsert = SWITCH_PD_ALT_STATUS1_INSERT;
constexpr uint16_t kUfp = SWITCH_PD_ALT_DATA_ROLE_UFP << SWITCH_PD_ALT_STATUS1_DR_SHIFT;
constexpr uint16_t kDfp = SWITCH_PD_ALT_DATA_ROLE_DFP << SWITCH_PD_ALT_STATUS1_DR_SHIFT;

struct Partner {
    bool dock = true;
    bool identity = true;  // answers Discover Identity
    bool dp_svid = true;
    uint8_t caps[4] = {0x45, 0x00, 0x0C, 0x00};  // receptacle, UFP_D pins C and D
    int hub_busy = 0;                            // HUB_CONTROL answers busy this many times
    bool refuse_swap = false;
};

// The controller and partner: each command becomes the alerts the chip raises, the last reply sits in INCOMING_VDM.
struct Model {
    Partner partner;
    uint16_t status1 = kInsert | kUfp;
    uint16_t dp_status = 0;
    std::vector<uint16_t> alerts;
    std::vector<uint8_t> incoming;
    std::vector<uint8_t> pending_reply;
    std::vector<uint16_t> commands;
    std::vector<std::vector<uint8_t>> messages;
    std::vector<uint8_t> configured;

    void reply(std::vector<uint8_t> bytes)
    {
        pending_reply = std::move(bytes);
        alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
        alerts.push_back(SWITCH_PD_ALT_ALERT_VDM_RECEIVED);
    }

    void vdm(const std::vector<uint8_t> &m)
    {
        const uint8_t command = m[1];
        if (m[3] == 0x7E && m[4] == 0x05 && m[2] == 0x00) {
            const uint8_t dock_command = m[7];
            const uint8_t busy = dock_command == 0x20 && partner.hub_busy-- > 0 ? 1 : 0;
            reply({12, 0x40, 0x00, 0x7E, 0x05, busy, 2, static_cast<uint8_t>(dock_command + 1), 0, 0, 0, 0, 0});
            return;
        }
        switch (command) {
        case 0x01:
            if (!partner.identity) {
                reply({4, 0x81, 0x80, 0x00, 0xFF});
            } else if (partner.dock) {
                reply({16, 0x41, 0x80, 0x00, 0xFF, 0x7E, 0x05, 0x00, 5 << 3, 0, 0, 0, 0, 0x00, 0x01, 0x03, 0x20});
            } else {
                reply({16, 0x41, 0x80, 0x00, 0xFF, 0xD1, 0x2B, 0x00, 5 << 3, 0, 0, 0, 0, 0x00, 0x01, 0x11, 0x11});
            }
            break;
        case 0x02:
            if (partner.dp_svid) {
                reply({8, 0x42, 0x80, 0x00, 0xFF, 0x00, 0x00, 0x01, 0xFF});
            } else {
                reply({8, 0x42, 0x80, 0x00, 0xFF, 0x00, 0x00, 0x7E, 0x05});
            }
            break;
        case 0x03:
            reply({8, 0x43, 0x80, 0x01, 0xFF, 0x05, 0x00, 0x0C, 0x00});
            break;
        case 0x04:
            reply({4, 0x44, 0x81, 0x7E, 0x05});
            break;
        default:
            FAIL() << "unexpected VDM " << int(command);
        }
    }

    void command(const switch_pd_alt_out &out)
    {
        commands.push_back(out.command);
        std::vector<uint8_t> message(out.message, out.message + out.message_len);
        if (out.message_len) {
            messages.push_back(message);
        }
        switch (out.command) {
        case SWITCH_PD_ALT_CMD_SEND_VDM:
            vdm(message);
            break;
        case SWITCH_PD_ALT_CMD_ACCEPT_VDM:
            incoming = pending_reply;
            alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            break;
        case SWITCH_PD_ALT_CMD_DR_SWAP:
            if (partner.refuse_swap) {
                status1 = static_cast<uint16_t>(status1 | (6u << 4));
            } else {
                status1 = static_cast<uint16_t>((status1 & ~(3u << 8)) | kDfp);
            }
            alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            break;
        case SWITCH_PD_ALT_CMD_DP_DISCOVER:
            incoming = {4, partner.caps[0], partner.caps[1], partner.caps[2], partner.caps[3]};
            alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            break;
        case SWITCH_PD_ALT_CMD_DP_CONFIGURE:
            configured.assign(out.message + 1, out.message + 5);
            dp_status = SWITCH_PD_ALT_DP_STATUS_DP_EN;
            alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            break;
        case SWITCH_PD_ALT_CMD_DP_EXIT:
            dp_status = 0;
            alerts.push_back(SWITCH_PD_ALT_ALERT_CMD_DONE);
            break;
        default:
            FAIL() << "unexpected command " << out.command;
        }
    }
};

struct Session {
    Model model;
    switch_pd_alt alt{};
    std::vector<std::string> lines;
    uint32_t timer_ms = 0;
    bool at_rest = true;
    bool dock_power = true;

    explicit Session(bool want_dp = true)
    {
        switch_pd_alt_init(&alt, want_dp);
    }

    void perform(switch_pd_alt_out out)
    {
        if (out.act == SWITCH_PD_ALT_READ) {
            out = switch_pd_alt_reply(&alt, model.incoming.data(), model.incoming.size());
        }
        if (out.line) {
            lines.emplace_back(out.line);
        }
        if (out.act == SWITCH_PD_ALT_COMMAND) {
            model.command(out);
        }
        timer_ms = out.timer_ms;
    }

    void event(uint16_t alert)
    {
        const switch_pd_alt_in in{alert, model.status1, model.dp_status, true, at_rest, dock_power};
        perform(switch_pd_alt_event(&alt, &in));
    }

    // Alerts until the chip is quiet, then the step timers (the hub's delays) until the sequence rests.
    void settle()
    {
        event(0);
        for (int guard = 0; guard < 200; ++guard) {
            if (!model.alerts.empty()) {
                const uint16_t alert = model.alerts.front();
                model.alerts.erase(model.alerts.begin());
                event(alert);
            } else if (alt.step == SWITCH_PD_ALT_HUB_POWER || alt.step == SWITCH_PD_ALT_HUB_RETRY) {
                perform(switch_pd_alt_timer(&alt, at_rest));
            } else {
                return;
            }
        }
        FAIL() << "no rest";
    }
};

TEST(AltMode, DockGetsPinDTwoLanesAndItsHub)
{
    Session run;
    run.model.partner.hub_busy = 2;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_TRUE(run.alt.dock);
    EXPECT_EQ(run.alt.pin, SWITCH_PD_ALT_PIN_D);
    EXPECT_EQ(run.alt.dp_lanes, 2);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    EXPECT_TRUE(run.alt.hub_enabled);
    EXPECT_EQ(run.model.configured, (std::vector<uint8_t>{0x06, 0x00, 0x08, 0x00}));
    EXPECT_EQ(run.model.commands.front(), SWITCH_PD_ALT_CMD_DR_SWAP);
    EXPECT_TRUE(run.lines.empty());
}

TEST(AltMode, DockSequenceIsBm92txxs)
{
    Session run;
    run.settle();
    const std::vector<std::vector<uint8_t>> expected = {
        {4, 0x01, 0x80, 0x00, 0xFF},
        {4, 0x04, 0x81, 0x7E, 0x05},
        {4, 0x06, 0x00, 0x08, 0x00},
        {8, 0x00, 0x00, 0x7E, 0x05, 0, 1, 0x16, 0x00},
        {12, 0x00, 0x00, 0x7E, 0x05, 1, 1, 0x01, 0x00, 64, 0, 0, 128},
        {8, 0x00, 0x00, 0x7E, 0x05, 1, 1, 0x20, 0x00},
    };
    EXPECT_EQ(run.model.messages, expected);
}

TEST(AltMode, DockOfferingOnlyCTakesAllFourLanes)
{
    Session run;
    run.model.partner.caps[2] = SWITCH_PD_ALT_PIN_C;
    run.settle();
    EXPECT_EQ(run.alt.pin, SWITCH_PD_ALT_PIN_C);
    EXPECT_EQ(run.alt.dp_lanes, 4);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_TAKEN);
    EXPECT_EQ(run.model.configured, (std::vector<uint8_t>{0x06, 0x00, 0x04, 0x00}));
    EXPECT_TRUE(run.alt.hub_enabled);
}

TEST(AltMode, AdapterPlugGetsPinDInTheStandardField)
{
    Session run;
    run.model.partner.dock = false;
    run.model.partner.caps[0] = 0x05;  // plug: UFP_D pins in 15:8
    run.model.partner.caps[1] = 0x0C;
    run.model.partner.caps[2] = 0x00;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_FALSE(run.alt.dock);
    EXPECT_EQ(run.alt.dp_lanes, 2);
    EXPECT_EQ(run.model.configured, (std::vector<uint8_t>{0x06, 0x08, 0x00, 0x00}));
    EXPECT_FALSE(run.alt.hub_enabled);
}

TEST(AltMode, AdapterWithoutDisplayPortFreesTheLanes)
{
    Session run;
    run.model.partner.dock = false;
    run.model.partner.dp_svid = false;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.dp_lanes, 0);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
}

TEST(AltMode, ChargerWithoutIdentityFreesTheLanes)
{
    Session run;
    run.model.partner.identity = false;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
}

TEST(AltMode, OnlyTheDocksPowerSignatureIsAskedToSwap)
{
    Session run;
    run.dock_power = false;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_TRUE(run.model.commands.empty());
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    EXPECT_TRUE(run.lines.empty());
}

TEST(AltMode, LanesArePendingUntilConfigured)
{
    Session run;
    run.event(0);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_PENDING);
    Session no_dp(false);
    no_dp.event(0);
    EXPECT_EQ(no_dp.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
}

TEST(AltMode, WithoutDisplayPortTheDockStillGetsItsHub)
{
    Session run(false);
    run.settle();
    EXPECT_TRUE(run.alt.hub_enabled);
    EXPECT_EQ(run.alt.dp_lanes, 0);
    for (const uint16_t command : run.model.commands) {
        EXPECT_NE(command, SWITCH_PD_ALT_CMD_DP_DISCOVER);
        EXPECT_NE(command, SWITCH_PD_ALT_CMD_DP_CONFIGURE);
    }
}

TEST(AltMode, NothingStartsWhileSwitchPDUsesTheCommandRegister)
{
    Session run;
    run.at_rest = false;
    run.event(SWITCH_PD_ALT_ALERT_CONTRACT);
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_IDLE);
    EXPECT_TRUE(run.model.commands.empty());
    run.at_rest = true;
    run.model.status1 |= SWITCH_PD_ALT_STATUS1_CMD_BUSY;
    run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_IDLE);
}

TEST(AltMode, ContractAlertPausesAndResendsTheStep)
{
    Session run;
    run.event(0);  // DR swap
    run.model.alerts.clear();
    run.at_rest = false;
    run.event(SWITCH_PD_ALT_ALERT_CONTRACT);
    EXPECT_TRUE(run.alt.paused);
    EXPECT_EQ(run.model.commands.size(), 1u);
    run.at_rest = true;
    run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);
    EXPECT_FALSE(run.alt.paused);
    EXPECT_EQ(run.model.commands.size(), 2u);
    EXPECT_EQ(run.model.commands.back(), SWITCH_PD_ALT_CMD_DR_SWAP);
    run.settle();
    EXPECT_EQ(run.alt.dp_lanes, 2);
}

TEST(AltMode, DetachEndsTheSessionAndFreesTheLanes)
{
    Session run;
    run.settle();
    run.model.status1 = 0;
    run.event(SWITCH_PD_ALT_ALERT_PLUGPULL);
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_IDLE);
    EXPECT_EQ(run.alt.dp_lanes, 0);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    EXPECT_FALSE(run.alt.hub_enabled);
}

TEST(AltMode, UnansweredDiscoveryFailsWithLanesFree)
{
    Session run;
    run.event(0);
    run.event(SWITCH_PD_ALT_ALERT_CMD_DONE);  // swap done: Discover Identity out, no answer
    run.model.alerts.clear();
    run.perform(switch_pd_alt_timer(&run.alt, true));
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_FAILED);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
    ASSERT_EQ(run.lines.size(), 1u);
}

TEST(AltMode, UnansweredConfigureStillEnablesTheDockHub)
{
    Session run;
    run.event(0);
    for (int i = 0; i < 50 && run.alt.step != SWITCH_PD_ALT_DP_CONFIGURE; ++i) {
        const uint16_t alert = run.model.alerts.front();
        run.model.alerts.erase(run.model.alerts.begin());
        run.event(alert);
    }
    ASSERT_EQ(run.alt.step, SWITCH_PD_ALT_DP_CONFIGURE);
    run.model.alerts.clear();
    run.perform(switch_pd_alt_timer(&run.alt, true));
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DOCK_QUERY);
    run.settle();
    EXPECT_TRUE(run.alt.hub_enabled);
    EXPECT_EQ(run.alt.dp_lanes, 0);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
}

TEST(AltMode, StaleDisplayPortIsExitedWithoutAReset)
{
    Session run;
    run.model.dp_status = SWITCH_PD_ALT_DP_STATUS_DP_EN;
    run.model.status1 = kInsert | kDfp;
    run.settle();
    EXPECT_EQ(run.model.commands.front(), SWITCH_PD_ALT_CMD_DP_EXIT);
    EXPECT_EQ(run.alt.dp_lanes, 2);
}

TEST(AltMode, RefusedSwapFails)
{
    Session run;
    run.model.partner.refuse_swap = true;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_FAILED);
    EXPECT_EQ(run.alt.lanes, SWITCH_PD_ALT_LANES_FREE);
}

TEST(AltMode, BusyHubGivesUpAfterTenRetries)
{
    Session run;
    run.model.partner.hub_busy = 100;
    run.settle();
    EXPECT_EQ(run.alt.step, SWITCH_PD_ALT_DONE);
    EXPECT_FALSE(run.alt.hub_enabled);
    EXPECT_EQ(run.alt.dp_lanes, 2);
}

TEST(PinChoice, DIsPreferredWheneverOffered)
{
    const uint8_t both_receptacle[] = {4, 0x45, 0x00, 0x1C, 0x00};
    EXPECT_EQ(switch_pd_alt_choose_pins(both_receptacle, 5, false).pin, SWITCH_PD_ALT_PIN_D);
    const uint8_t both_plug[] = {4, 0x05, 0x3C, 0x00, 0x00};
    EXPECT_EQ(switch_pd_alt_choose_pins(both_plug, 5, false).pin, SWITCH_PD_ALT_PIN_D);
    const uint8_t only_c[] = {4, 0x05, 0x04, 0x00, 0x00};
    const switch_pd_alt_config c = switch_pd_alt_choose_pins(only_c, 5, false);
    EXPECT_EQ(c.pin, SWITCH_PD_ALT_PIN_C);
    EXPECT_EQ(c.lanes, 4);
    const uint8_t only_e[] = {4, 0x05, 0x10, 0x00, 0x00};
    EXPECT_EQ(switch_pd_alt_choose_pins(only_e, 5, false).pin, 0);
}

TEST(PinChoice, DockIsReadAsAReceptacle)
{
    const uint8_t dock[] = {4, 0x05, 0x00, 0x0C, 0x00};
    const switch_pd_alt_config config = switch_pd_alt_choose_pins(dock, 5, true);
    EXPECT_EQ(config.pin, SWITCH_PD_ALT_PIN_D);
    EXPECT_EQ(config.vdo[1], 0);
    EXPECT_EQ(config.vdo[2], SWITCH_PD_ALT_PIN_D);
}

TEST(PinChoice, AWrongCountIsRefused)
{
    const uint8_t caps[] = {8, 0x45, 0x00, 0x0C, 0x00};
    EXPECT_EQ(switch_pd_alt_choose_pins(caps, 5, false).pin, 0);
}

}  // namespace
