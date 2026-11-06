// SPDX-License-Identifier: GPL-2.0-only
// The SwitchDisplayPort service: one work loop owns DPAUX1, SOR1, display controller B and the external screen.
// SwitchDisplayPort.cpp has the service, its interrupts and the AUX executor; SwitchDPSession.cpp walks a hot-plugged
// sink from its capabilities to a trained, scanning link.
#ifndef SWITCH_DISPLAY_PORT_SERVICE_H
#define SWITCH_DISPLAY_PORT_SERVICE_H

#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

#include "SwitchDPAux.h"
#include "SwitchDPLink.h"
#include "SwitchDPModes.h"
#include "SwitchDPTraining.h"
#include "SwitchExternalScreen.h"
#include "TegraDisplayB.h"
#include "TegraDpaux1.h"
#include "TegraSor1.h"

class SwitchDisplayPort : public IOService {
    OSDeclareDefaultStructors(SwitchDisplayPort);

public:
    bool begin();
    IOWorkLoop *getWorkLoop() const override
    {
        return loop;
    }
    void setAltLanes(unsigned lanes, bool flipped);

private:
    enum class Stage : uint8_t { Off, Waiting, Probing, Training, Running };
    enum class Next : uint8_t { Caps, Powered, Edid, LinkSet, Coding, TrainingSet, TrainingStatus, Trained, LinkStatus };

    IOWorkLoop *loop = nullptr;
    IOInterruptEventSource *auxSource = nullptr, *dcSource = nullptr;
    IOTimerEventSource *auxTimer = nullptr, *stepTimer = nullptr;
    TegraDpaux1 aux;
    TegraSor1 sor;
    TegraDisplayB dcb;
    SwitchExternalScreen screen;

    Stage stage = Stage::Off;
    unsigned altLanes = 0;
    bool flipped = false;
    bool dcbUp = false, sorUp = false;

    struct switch_dp_aux_request request = {};
    Next next = Next::Caps;
    unsigned attempt = 0;
    bool auxPending = false, auxRetrying = false;

    uint8_t caps[16] = {};
    uint8_t edid[512] = {};
    unsigned edidBlock = 0, edidBlocks = 0, edidStep = 0, edidSteps = 0, edidRead = 0;
    struct switch_dp_aux_request edidPlan[SWITCH_DP_EDID_REQUESTS] = {};
    struct switch_dp_mode_list modes = {};
    struct switch_dp_mode mode = {};
    unsigned modeIndex = 0;
    struct switch_dp_link link = {};
    uint8_t maxRate = 0, maxLanes = 0;
    struct switch_dp_train train = {};
    struct switch_dp_train_out trainOut = {};

    static void auxInterrupt(OSObject *owner, IOInterruptEventSource *, int);
    static void dcInterrupt(OSObject *owner, IOInterruptEventSource *, int);
    static void auxTimerFired(OSObject *owner, IOTimerEventSource *);
    static void stepTimerFired(OSObject *owner, IOTimerEventSource *);

    void auxStart(const struct switch_dp_aux_request &transaction, Next then);
    void auxIssue();
    void auxNative(enum switch_dp_aux_op op, uint32_t address, const uint8_t *bytes, uint8_t size, Next then);
    void auxDone();
    void fail(const char *what, const char *why);
    void teardownLink();
    void powerOff();

    void plugged();
    void unplugged();
    void irqHpd();
    void dispatch();
    void edidNext();
    void edidDone();
    void chooseAndTrain();
    void applyTraining();
    void trained();
};

#endif
