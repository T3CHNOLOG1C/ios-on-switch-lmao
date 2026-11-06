// SPDX-License-Identifier: GPL-2.0-only
// SwitchDPAux.h: DP_AUXCTL encodings (Linux tegra_dpaux_transfer's), reply decoding, retries and the EDID read plan.
#include <gtest/gtest.h>

#include <cstdint>

#include "../SwitchDPAux.h"

namespace {

TEST(Aux, ControlWordsMatchLinux)
{
    switch_dp_aux_request r = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_READ, 0x000, 16);
    EXPECT_EQ(switch_dp_auxctl(&r), 0x900Fu);
    r = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, 0x102, 3);
    EXPECT_EQ(switch_dp_auxctl(&r), 0x8002u);
    r = switch_dp_aux_make(SWITCH_DP_AUX_I2C_MOT_WRITE, 0x50, 1);
    EXPECT_EQ(switch_dp_auxctl(&r), 0x4000u);
    r = switch_dp_aux_make(SWITCH_DP_AUX_I2C_MOT_READ, 0x50, 16);
    EXPECT_EQ(switch_dp_auxctl(&r), 0x500Fu);
    r = switch_dp_aux_make(SWITCH_DP_AUX_I2C_READ, 0x50, 0);
    EXPECT_EQ(switch_dp_auxctl(&r), 0x1100u);
    r = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, 0x102, 0);
    EXPECT_EQ(switch_dp_auxctl(&r), 0u);
    r = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_READ, 0x000, 17);
    EXPECT_EQ(switch_dp_auxctl(&r), 0u);
}

TEST(Aux, FifoWordsAreLittleEndian)
{
    switch_dp_aux_request r = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, 0x102, 5);
    for (uint8_t i = 0; i < 5; ++i) {
        r.data[i] = static_cast<uint8_t>(0x10 + i);
    }
    uint32_t words[4];
    switch_dp_aux_fifo_words(&r, words);
    EXPECT_EQ(words[0], 0x13121110u);
    EXPECT_EQ(words[1], 0x14u);
    EXPECT_EQ(words[2], 0u);
}

TEST(Aux, Replies)
{
    const switch_dp_aux_request read = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_READ, 0, 6);
    const switch_dp_aux_request write = switch_dp_aux_make(SWITCH_DP_AUX_NATIVE_WRITE, 0x102, 3);
    EXPECT_EQ(switch_dp_aux_decode(6, &read), SWITCH_DP_AUX_ACK);
    EXPECT_EQ(switch_dp_aux_decode(4, &read), SWITCH_DP_AUX_SHORT);
    EXPECT_EQ(switch_dp_aux_decode(0, &write), SWITCH_DP_AUX_ACK);
    EXPECT_EQ(switch_dp_aux_decode(1u << 8, &read), SWITCH_DP_AUX_TIMEOUT);
    EXPECT_EQ(switch_dp_aux_decode(1u << 9, &read), SWITCH_DP_AUX_ERROR);
    EXPECT_EQ(switch_dp_aux_decode(1u << 16, &write), SWITCH_DP_AUX_NACK);
    EXPECT_EQ(switch_dp_aux_decode(2u << 16, &write), SWITCH_DP_AUX_DEFER);
    EXPECT_EQ(switch_dp_aux_decode(4u << 16, &write), SWITCH_DP_AUX_I2C_NACK);
    EXPECT_EQ(switch_dp_aux_decode(8u << 16, &write), SWITCH_DP_AUX_I2C_DEFER);
    EXPECT_EQ(switch_dp_aux_decode(SWITCH_DPAUX_AUXSTAT_HPD | 6, &read), SWITCH_DP_AUX_ACK);
}

TEST(Aux, OnlyTransientRepliesAreRetried)
{
    EXPECT_EQ(switch_dp_aux_retry_us(SWITCH_DP_AUX_DEFER, 0), SWITCH_DP_AUX_RETRY_US);
    EXPECT_EQ(switch_dp_aux_retry_us(SWITCH_DP_AUX_TIMEOUT, 6), SWITCH_DP_AUX_RETRY_US);
    EXPECT_EQ(switch_dp_aux_retry_us(SWITCH_DP_AUX_I2C_DEFER, SWITCH_DP_AUX_RETRIES), 0u);
    EXPECT_EQ(switch_dp_aux_retry_us(SWITCH_DP_AUX_NACK, 0), 0u);
    EXPECT_EQ(switch_dp_aux_retry_us(SWITCH_DP_AUX_ERROR, 0), 0u);
}

TEST(Aux, EdidBlockPlans)
{
    switch_dp_aux_request r[SWITCH_DP_EDID_REQUESTS];
    ASSERT_EQ(switch_dp_edid_requests(0, r), 10u);
    EXPECT_EQ(r[0].op, SWITCH_DP_AUX_I2C_MOT_WRITE);
    EXPECT_EQ(r[0].address, SWITCH_DP_EDID_ADDRESS);
    EXPECT_EQ(r[0].data[0], 0);
    for (int k = 1; k <= 8; ++k) {
        EXPECT_EQ(r[k].op, SWITCH_DP_AUX_I2C_MOT_READ);
        EXPECT_EQ(r[k].size, 16);
    }
    EXPECT_EQ(r[9].op, SWITCH_DP_AUX_I2C_READ);
    EXPECT_EQ(r[9].size, 0);
    ASSERT_EQ(switch_dp_edid_requests(1, r), 10u);
    EXPECT_EQ(r[0].data[0], 128);
    ASSERT_EQ(switch_dp_edid_requests(3, r), 11u);
    EXPECT_EQ(r[0].address, SWITCH_DP_EDID_SEGMENT);
    EXPECT_EQ(r[0].data[0], 1);
    EXPECT_EQ(r[1].data[0], 128);
}

}  // namespace
