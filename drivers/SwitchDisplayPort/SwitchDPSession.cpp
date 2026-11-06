// SPDX-License-Identifier: GPL-2.0-only
// A hot-plugged sink, from its DPCD capabilities to a trained link scanning out (SwitchDisplayPortService.h): every
// step is the completion of the AUX transaction or the training wait before it. Order: Linux drivers/gpu/drm/tegra/sor.c
// tegra_sor_dp_enable and dp.c drm_dp_link_probe/_choose/_train; the display controller is running before SOR1 starts
// (the CRTC is enabled before the encoder in DRM's commit).
#include <IOKit/IOLib.h>

#include "SwitchDisplayPortService.h"

namespace {
constexpr uint8_t kDpcdD0 = 1;
constexpr unsigned kMaxEdidBlocks = 4;
}  // namespace

void SwitchDisplayPort::dispatch()
{
    switch (next) {
    case Next::Caps:
        for (unsigned i = 0; i < sizeof(caps); ++i) {
            caps[i] = request.data[i];
        }
        auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_SET_POWER, &kDpcdD0, 1, Next::Powered);
        break;
    case Next::Powered:
        edidBlock = 0;
        edidBlocks = 1;
        edidStep = 0;
        edidSteps = switch_dp_edid_requests(0, edidPlan);
        edidRead = 0;
        edidNext();
        break;
    case Next::Edid: {
        const struct switch_dp_aux_request &step = edidPlan[edidStep];
        if (step.op == SWITCH_DP_AUX_I2C_MOT_READ) {
            for (unsigned i = 0; i < 16; ++i) {
                edid[edidBlock * 128 + edidRead * 16 + i] = request.data[i];
            }
            ++edidRead;
        }
        ++edidStep;
        edidNext();
        break;
    }
    case Next::LinkSet: {
        const uint8_t coding[2] = {0x00, 0x01};  // no downspread, 8b/10b
        auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_DOWNSPREAD_CTRL, coding, 2, Next::Coding);
        break;
    }
    case Next::Coding:
        trainOut = switch_dp_train_start(&train, link.lanes,
                                         link.rate == SWITCH_DP_RATE_HBR2 && (caps[2] & SWITCH_DPCD_TPS3) != 0,
                                         caps[SWITCH_DPCD_TRAINING_AUX_RD_INTERVAL]);
        applyTraining();
        break;
    case Next::TrainingSet:
        stepTimer->setTimeoutUS(trainOut.wait_us);
        break;
    case Next::TrainingStatus:
        trainOut = switch_dp_train_status(&train, request.data);
        applyTraining();
        break;
    case Next::Trained:
        trained();
        break;
    case Next::LinkStatus:
        if (!switch_dp_link_holds(request.data + 2, link.lanes)) {
            IOLog("SwitchDisplayPort: IRQ_HPD: the link lost lock (lanes %#x %#x align %#x); training again\n",
                  request.data[2], request.data[3], request.data[4]);
            teardownLink();
            chooseAndTrain();
        }
        break;
    }
}

void SwitchDisplayPort::edidNext()
{
    if (edidStep < edidSteps) {
        auxStart(edidPlan[edidStep], Next::Edid);
        return;
    }
    if (edidBlock == 0) {
        edidBlocks = switch_dp_edid_header_valid(edid) ? 1u + edid[126] : 1u;
        edidBlocks = edidBlocks > kMaxEdidBlocks ? kMaxEdidBlocks : edidBlocks;
    }
    if (++edidBlock < edidBlocks) {
        edidStep = 0;
        edidRead = 0;
        edidSteps = switch_dp_edid_requests(edidBlock, edidPlan);
        edidNext();
        return;
    }
    edidDone();
}

void SwitchDisplayPort::edidDone()
{
    switch_dp_edid_modes(edid, edidBlocks, &modes);
    maxRate = switch_dp_rate_cap(caps[1]);
    const uint8_t sinkLanes = caps[2] & 0x1fu;
    maxLanes = sinkLanes < altLanes ? sinkLanes : (uint8_t)altLanes;
    IOLog("SwitchDisplayPort: sink DPCD %u.%u, %u lanes at up to %u.%02u Gb/s, EDID %u block%s, %u modes\n",
          caps[0] >> 4, caps[0] & 0xfu, sinkLanes, switch_dp_rate_khz(maxRate) / 100000,
          (switch_dp_rate_khz(maxRate) / 1000) % 100, edidBlocks, edidBlocks == 1 ? "" : "s", modes.count);
    chooseAndTrain();
}

void SwitchDisplayPort::chooseAndTrain()
{
    if (maxRate == 0 || maxLanes == 0) {
        fail("the link", "no rate or lane left to train");
        return;
    }
    const int chosen = switch_dp_pick_mode(&modes, switch_dp_payload_kbps(maxRate, maxLanes));
    if (chosen < 0 || !switch_dp_link_for(modes.modes[chosen].pixel_khz, maxRate, maxLanes, &link)) {
        fail("the mode", "nothing the sink offers fits 1920x1080 at 60 Hz on this link");
        return;
    }
    modeIndex = (unsigned)chosen;
    mode = modes.modes[chosen];
    stage = Stage::Training;
    char why[128] = "";
    if (!sor.pixelClock(mode.pixel_khz, why, sizeof(why))) {
        fail("the pixel clock", why);
        return;
    }
    if (!dcbUp) {
        if (!dcb.powerUp(why, sizeof(why))) {
            fail("display controller B", why);
            return;
        }
        dcbUp = true;
    }
    dcb.setMode(mode);
    if (!sorUp) {
        if (!sor.powerUp(why, sizeof(why))) {
            fail("SOR1", why);
            return;
        }
        sorUp = true;
    }
    const bool enhanced = (caps[2] & SWITCH_DPCD_ENHANCED_FRAME) != 0;
    if (!sor.startLink(link, enhanced, why, sizeof(why))) {
        fail("SOR1", why);
        return;
    }
    const uint8_t linkSet[2] = {link.rate, (uint8_t)(link.lanes | (enhanced ? SWITCH_DPCD_ENHANCED_FRAME : 0u))};
    auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_LINK_BW_SET, linkSet, 2, Next::LinkSet);
}

void SwitchDisplayPort::applyTraining()
{
    switch (trainOut.act) {
    case SWITCH_DP_TRAIN_DRIVE:
        sor.drive(train, trainOut);
        auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_TRAINING_PATTERN_SET, trainOut.dpcd, trainOut.dpcd_len,
                  Next::TrainingSet);
        break;
    case SWITCH_DP_TRAIN_DONE:
        sor.drive(train, trainOut);
        auxNative(SWITCH_DP_AUX_NATIVE_WRITE, SWITCH_DPCD_TRAINING_PATTERN_SET, trainOut.dpcd, 1, Next::Trained);
        break;
    case SWITCH_DP_TRAIN_FAIL: {
        IOLog("SwitchDisplayPort: training at %u Mb/s x%u: %s\n", switch_dp_rate_khz(link.rate) / 100, link.lanes,
              train.why != nullptr ? train.why : "failed");
        maxRate = switch_dp_rate_below(link.rate);
        teardownLink();
        chooseAndTrain();
        break;
    }
    }
}

void SwitchDisplayPort::trained()
{
    const bool enhanced = (caps[2] & SWITCH_DPCD_ENHANCED_FRAME) != 0;
    struct switch_dp_tu tu = {};
    char why[128] = "";
    if (!switch_dp_tu_config(mode.pixel_khz, mode.hactive, switch_dp_mode_htotal(&mode), link.rate, link.lanes,
                             enhanced, &tu)) {
        fail("SOR1", "no transfer-unit configuration for the mode");
        return;
    }
    if (!sor.attach(mode, tu, why, sizeof(why))) {
        fail("SOR1", why);
        return;
    }
    dcb.enableSor(true);
    if (!sor.wake(why, sizeof(why))) {
        fail("SOR1", why);
        return;
    }
    stage = Stage::Running;
    IOLog("SwitchDisplayPort: link up at %u Mb/s x%u (TU %u, watermark %u)\n", switch_dp_rate_khz(link.rate) / 100,
          link.lanes, tu.tu_size, tu.watermark);
    screen.attach(modes, modeIndex, &dcb);
}
