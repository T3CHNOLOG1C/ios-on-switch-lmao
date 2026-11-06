// SPDX-License-Identifier: GPL-2.0-only
// SwitchDisplayPort: TV output over the USB-C port; see SwitchDisplayPortService.h. Started by the first DisplayPort
// Configure the BM92T36 engine reports, on a thread call so SwitchPD's work loop never waits on it. Every event is an
// interrupt (DPAUX1: AUX done, HPD plug, unplug, IRQ_HPD; display controller B: frame end) or a one-shot timer; an AUX
// transaction whose AUX_DONE never reaches the CPU panics, with the GIC's view of the line.
#include "SwitchDisplayPort.h"

#include <IOKit/IOLib.h>
#include <kern/thread_call.h>

#include "../ArmGIC/SwitchInterrupts.h"
#include "SwitchDisplayPortService.h"

OSDefineMetaClassAndStructors(SwitchDisplayPort, IOService);

namespace {
constexpr uint32_t kAuxLostMs = 50;  // DPAUX1 times out a silent sink itself within a millisecond
SwitchDisplayPort *gService;
thread_call_t gApply;
uint32_t gLanes, gFlipped;

void applyAltMode(thread_call_param_t, thread_call_param_t)
{
    if (gService == nullptr) {
        auto *service = OSTypeAlloc(SwitchDisplayPort);
        if (service == nullptr || !service->init() || !service->attach(IOService::getResourceService()) ||
            !service->begin()) {
            IOLog("SwitchDisplayPort: could not start\n");
            OSSafeReleaseNULL(service);
            return;
        }
        gService = service;
    }
    const unsigned lanes = __atomic_load_n(&gLanes, __ATOMIC_ACQUIRE);
    const bool flipped = __atomic_load_n(&gFlipped, __ATOMIC_ACQUIRE) != 0;
    gService->getWorkLoop()->runAction(
        [](OSObject *owner, void *lanes, void *flipped, void *, void *) -> IOReturn {
            static_cast<SwitchDisplayPort *>(owner)->setAltLanes((unsigned)(uintptr_t)lanes, flipped != nullptr);
            return kIOReturnSuccess;
        },
        gService, reinterpret_cast<void *>((uintptr_t)lanes), flipped ? gService : nullptr);
}
}  // namespace

extern "C" void switch_displayport_alt_mode(unsigned lanes, bool flipped)
{
    __atomic_store_n(&gLanes, lanes, __ATOMIC_RELEASE);
    __atomic_store_n(&gFlipped, flipped ? 1u : 0u, __ATOMIC_RELEASE);
    if (gApply == nullptr) {
        thread_call_t fresh = thread_call_allocate(applyAltMode, nullptr), none = nullptr;
        if (!__atomic_compare_exchange_n(&gApply, &none, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            thread_call_free(fresh);
        }
    }
    thread_call_enter(gApply);
}

bool SwitchDisplayPort::begin()
{
    loop = IOWorkLoop::workLoop();
    IOService *auxNub = switch_interrupt_nub(TEGRA_DPAUX1_INTID, false);
    IOService *dcNub = switch_interrupt_nub(TEGRA_DISPLAY_B_INTID, false);
    auxSource = loop && auxNub ? IOInterruptEventSource::interruptEventSource(this, &auxInterrupt, auxNub, 0) : nullptr;
    dcSource = loop && dcNub ? IOInterruptEventSource::interruptEventSource(this, &dcInterrupt, dcNub, 0) : nullptr;
    auxTimer = loop ? IOTimerEventSource::timerEventSource(this, &auxTimerFired) : nullptr;
    stepTimer = loop ? IOTimerEventSource::timerEventSource(this, &stepTimerFired) : nullptr;
    if (auxSource == nullptr || dcSource == nullptr || auxTimer == nullptr || stepTimer == nullptr ||
        loop->addEventSource(auxSource) != kIOReturnSuccess || loop->addEventSource(dcSource) != kIOReturnSuccess ||
        loop->addEventSource(auxTimer) != kIOReturnSuccess || loop->addEventSource(stepTimer) != kIOReturnSuccess) {
        panic("SwitchDisplayPort: cannot attach the DPAUX1 (INTID %u) or display controller B (INTID %u) interrupt",
              TEGRA_DPAUX1_INTID, TEGRA_DISPLAY_B_INTID);
    }
    setName("SwitchDisplayPort");
    registerService();
    return true;
}

void SwitchDisplayPort::setAltLanes(unsigned lanes, bool plugFlipped)
{
    if (lanes == altLanes) {
        return;
    }
    if (lanes == 0) {
        IOLog("SwitchDisplayPort: DisplayPort alternate mode ended\n");
        powerOff();
        altLanes = 0;
        return;
    }
    altLanes = lanes;
    flipped = plugFlipped;
    char why[128] = "";
    if (stage == Stage::Off) {
        if (!aux.powerUp(why, sizeof(why))) {
            fail("DPAUX1", why);
            return;
        }
        auxSource->enable();
        dcSource->enable();
        stage = Stage::Waiting;
    }
    IOLog("SwitchDisplayPort: DisplayPort on %u lanes (pin assignment %c, plug %s), HPD %s\n", lanes,
          lanes == 2 ? 'D' : 'C', flipped ? "flipped" : "straight", aux.hpdHigh() ? "high" : "low");
    if (aux.hpdHigh()) {
        plugged();
    }
}

void SwitchDisplayPort::auxInterrupt(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    const uint32_t cause = self->aux.takeInterrupt();
    if ((cause & SWITCH_DPAUX_INTR_AUX_DONE) != 0 && self->auxPending) {
        self->auxTimer->cancelTimeout();
        self->auxPending = false;
        self->auxDone();
    }
    if ((cause & SWITCH_DPAUX_INTR_UNPLUG_EVENT) != 0) {
        self->unplugged();
    }
    if ((cause & SWITCH_DPAUX_INTR_PLUG_EVENT) != 0) {
        self->plugged();
    }
    if ((cause & SWITCH_DPAUX_INTR_IRQ_EVENT) != 0) {
        self->irqHpd();
    }
}

void SwitchDisplayPort::dcInterrupt(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    const uint32_t cause = self->dcb.takeInterrupt();
    if ((cause & (1u << 1)) != 0) {
        self->screen.frameEnd();
    }
}

void SwitchDisplayPort::auxStart(const struct switch_dp_aux_request &transaction, Next then)
{
    request = transaction;
    next = then;
    attempt = 0;
    auxIssue();
}

void SwitchDisplayPort::auxNative(enum switch_dp_aux_op op, uint32_t address, const uint8_t *bytes, uint8_t size,
                                  Next then)
{
    struct switch_dp_aux_request native = switch_dp_aux_make(op, address, size);
    for (uint8_t i = 0; bytes != nullptr && i < size; ++i) {
        native.data[i] = bytes[i];
    }
    auxStart(native, then);
}

void SwitchDisplayPort::auxIssue()
{
    auxRetrying = false;
    auxPending = true;
    aux.start(request);
    switch_check_after_ms(auxTimer, kAuxLostMs);
}

void SwitchDisplayPort::auxDone()
{
    const enum switch_dp_aux_result result = aux.finish(request);
    if (result == SWITCH_DP_AUX_ACK) {
        dispatch();
        return;
    }
    const uint32_t retryUs = switch_dp_aux_retry_us(result, attempt++);
    if (retryUs != 0) {
        auxRetrying = true;
        auxTimer->setTimeoutUS(retryUs);
        return;
    }
    static const char *const names[] = {"ack", "NACK", "defer", "I2C NACK", "I2C defer", "timeout", "error", "short"};
    char why[96];
    snprintf(why, sizeof(why), "AUX %s at %#x after %u tries (AUXSTAT %#x)", names[result], request.address, attempt,
             aux.auxstat());
    fail("the sink", why);
}

void SwitchDisplayPort::auxTimerFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    if (self->auxRetrying) {
        self->auxIssue();
        return;
    }
    if (!self->auxPending) {
        return;
    }
    char gic[192];
    switch_interrupt_describe(TEGRA_DPAUX1_INTID, gic, sizeof(gic));
    panic("SwitchDisplayPort: DPAUX1 interrupt lost: no AUX_DONE %u ms after a transaction (AUXCTL %#x, AUXSTAT %#x); %s",
          kAuxLostMs, switch_dp_auxctl(&self->request), self->aux.auxstat(), gic);
}

void SwitchDisplayPort::stepTimerFired(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchDisplayPort *>(owner);
    if (self->stage == Stage::Training) {
        self->auxNative(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DPCD_LANE0_1_STATUS, nullptr, 6, Next::TrainingStatus);
    }
}

void SwitchDisplayPort::fail(const char *what, const char *why)
{
    IOLog("SwitchDisplayPort: %s: %s; DisplayPort off until the next plug\n", what, why);
    teardownLink();
    if (stage != Stage::Off) {
        stage = Stage::Waiting;
    }
}

void SwitchDisplayPort::teardownLink()
{
    auxTimer->cancelTimeout();
    stepTimer->cancelTimeout();
    auxPending = auxRetrying = false;
    screen.detach();
    if (dcbUp) {
        dcb.windowOff();
    }
    sor.detachHead();
    if (dcbUp) {
        dcb.enableSor(false);
    }
    if (sorUp) {
        sor.powerDown();
        sorUp = false;
    }
    if (dcbUp) {
        dcb.powerDown();
        dcbUp = false;
    }
}

void SwitchDisplayPort::powerOff()
{
    teardownLink();
    if (stage != Stage::Off) {
        auxSource->disable();
        dcSource->disable();
        aux.powerDown();
    }
    stage = Stage::Off;
}

void SwitchDisplayPort::plugged()
{
    if (stage != Stage::Waiting) {
        return;
    }
    stage = Stage::Probing;
    auxNative(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DPCD_REV, nullptr, sizeof(caps), Next::Caps);
}

void SwitchDisplayPort::unplugged()
{
    if (stage == Stage::Off || stage == Stage::Waiting) {
        return;
    }
    IOLog("SwitchDisplayPort: the sink went away (HPD low)\n");
    teardownLink();
    stage = Stage::Waiting;
}

void SwitchDisplayPort::irqHpd()
{
    if (stage == Stage::Running && !auxPending && !auxRetrying) {
        auxNative(SWITCH_DP_AUX_NATIVE_READ, SWITCH_DPCD_SINK_COUNT, nullptr, 6, Next::LinkStatus);
    }
}
