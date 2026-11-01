// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXUDC: the Tegra X1 XUSB device controller as a backend of Apple's
// original IOUSBDeviceController (com.apple.iokit.IOUSBDeviceFamily, 21H461).
//
// The family does everything above the controller: descriptors, the EP0
// request state machine (IOUSBDeviceControlRequest), configurations and
// functions. With ConfigurationType "standardBringup" the original
// IOUSBDeviceConfigurator publishes one AppleUSBMux interface, so the original
// AppleUSBDeviceMux runs and a Mac's usbmuxd sees an iPad (iproxy, SSH).
// This file implements the 18 pure-virtual hardware methods the in-cache
// reference backend (AppleUSBXDCI, a DWC3 driver) implements, on top of
// XudcHardware. USB2 high speed; bulk and interrupt endpoints; no isochronous.
//
// Contract (symbolized macOS IOUSBDeviceFamily, checked against the iOS code):
// - setup packets go to controlRequest()->inputSetupPacket() + processSetupPacket();
// - io() requests reach ioGated() inside the family's command gate on our
//   workloop; completion = status + actual into the request, then
//   completeEndpointIO(); a nonzero ioGated() return means "not queued";
// - EP0: the family sends IN data and the device's IN status as io(0x80),
//   tells us OUT data stages (state 5/6) and the host's OUT status (state 8)
//   through handleControlRequestStateChange(); both end in inputDataPacket().
// All calls run on one IOWorkLoop: the family's command gate, the XUDC
// interrupt (GIC SPI 44 = INTID 76: Linux tegra210.dtsi:1303, OLED DTB
// xudc@700d0000) and a housekeeping timer share it, so nothing here needs a
// lock. The interrupt drains the event ring (Linux tegra_xudc_irq); nothing
// else does. The timer repeats the cable kick until the family first goes on
// the bus (the family's own readiness, which raises no interrupt), then only
// watches ST.IP: set at two ticks in a row with no interrupt taken in between,
// the interrupt is not reaching the CPU and we panic. No polled mode.
#include "OriginalIOUSBDeviceFamily.h"
#include "XudcHardware.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <mach/kmod.h>
#include <string.h>
#include "../ArmGIC/IOSwitchInterrupts.h"

using namespace xudc;

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void switchxudc_static_init(void) __asm__("__GLOBAL__sub_I_SwitchXUDC.cpp");
int switchxudc_start(void);
void switchxudc_set_host_owns_port(bool host);
}

namespace {
// _IOUSBDeviceIORequestTag fields (IOUSBDeviceController::io and completeEndpointIO).
constexpr size_t kReqEndpoint = 0, kReqBuffer = 8, kReqStatus = 40, kReqIsoc = 48, kReqLength = 64, kReqActual = 72;
template <typename T> T get(const _IOUSBDeviceIORequestTag *r, size_t off)
{
    T value;
    memcpy(&value, reinterpret_cast<const uint8_t *>(r) + off, sizeof(value));
    return value;
}
template <typename T> void put(_IOUSBDeviceIORequestTag *r, size_t off, T value)
{
    memcpy(reinterpret_cast<uint8_t *>(r) + off, &value, sizeof(value));
}

// IOUSBDeviceControlRequest states that need the controller (see the header).
enum : int { kStateOutData = 5, kStateOutDataLong = 6, kStateInStatus = 7, kStateOutStatus = 8 };
// USB transfer types as passed to activateEndpoint.
enum : int { kTypeBulk = 2, kTypeInterrupt = 3 };
constexpr int kSpeedFull = 1, kSpeedHigh = 2, kSpeedNone = -1;

constexpr uint32_t kQueueDepth = 16;
constexpr uint32_t kEp0Bytes = 0x1000;      // descriptors and control data
constexpr uint32_t kBounceBytes = 0x10000;  // AppleUSBDeviceMux uses 32 KiB transfers
constexpr uint32_t kStallCode = 6;
constexpr uint32_t kXudcIntid = 32 + 44;
constexpr uint32_t kWatchdogMS = 2000;

// USB2 pad 0 belongs to the host controller (SwitchXHCI.cpp, switch-usb-host=1) while this is set: the
// family's goOnBus(true) is remembered and connects only when the pad comes back.
bool gHostOwnsPort;

uint32_t dciFor(int address)
{
    uint32_t number = address & 0xf;
    return number ? number * 2 + ((address & 0x80) ? 1 : 0) : 0;
}

uint32_t log2Floor(uint32_t v)
{
    uint32_t n = 0;
    while (v > 1) {
        v >>= 1;
        ++n;
    }
    return n;
}

uint64_t nowMs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}
}

class SwitchXUDC : public IOUSBDeviceController {
    OSDeclareDefaultStructors(SwitchXUDC)

        struct Endpoint {
        bool active, busy;
        uint32_t busyTrb, busyLength;
        DmaBuffer bounce;
        _IOUSBDeviceIORequestTag *queue[kQueueDepth];
        uint32_t head, count;
    };
    enum class Stage : uint8_t { Idle, DataIn, StatusIn, DataOut, StatusOut };

    Hardware hw;
    IOWorkLoop *loop;
    IOTimerEventSource *timer;
    IOFilterInterruptEventSource *interrupt;
    Endpoint endpoints[kMaxDci];
    Stage stage;
    _IOUSBDeviceIORequestTag *ep0Request;
    uint32_t ep0Trb, ep0Length;
    uint8_t setup[8];
    uint16_t sequence;
    int speed;
    bool onBus, everOnBus, suspended, configuratorReported, pendingSeen, wantBus;
    uint64_t startedAt, lastCableKick;
    volatile uint32_t interrupts;  // ST.IP acknowledged by the filter
    uint32_t interruptsAtPending;

    void housekeeping(IOTimerEventSource *);
    void serviceEvents();
    void attachInterrupt();
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void interruptOccurred(OSObject *owner, IOInterruptEventSource *sender, int count);
    void portChange();
    void busReset();
    void setupEvent(const Event &e);
    void transferEvent(const Event &e);
    void ep0Complete(const Event &e);
    IOReturn ep0Io(_IOUSBDeviceIORequestTag *request, bool in);
    void ep0Kick();
    void ep0Cancel(IOReturn status);
    void kick(uint32_t dci);
    void cancel(uint32_t dci, IOReturn status);
    void finish(_IOUSBDeviceIORequestTag *request, IOReturn status, uint32_t actual);
    bool copy(_IOUSBDeviceIORequestTag *request, uint8_t *bytes, uint32_t length, bool toDevice);
    void checkConfigurator();
    IOReturn portOwnerChanged();
    void setBus(bool on);

public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOWorkLoop *getWorkLoop() const override { return loop; }

    void softDisconnect(uint32_t reason) override;
    void goOnBus(bool on) override;
    bool connectedWithAccessoryCable() override { return false; }
    IOReturn handleStallEndpoint(int address, bool stall) override;
    IOReturn handleAbortEndpoint(int address) override;
    IOReturn handleSetAddressPreStatusPhase(int address) override;
    IOReturn handleSetAddressPostStatusPhase(int) override { return kIOReturnSuccess; }
    int getConnectionSpeed() override { return speed; }
    IOReturn provideEndpointIDsForConfiguration(OSArray *interfaces, uint8_t *ids) override;
    IOReturn prepareConfiguration(OSArray *) override { return kIOReturnSuccess; }
    IOReturn activateEndpoint(int address, int type, int maxPacketSize, int interval, int property) override;
    IOReturn deactivateEndpoint(int address) override;
    IOBufferMemoryDescriptor *createIOBuffer(IOOptionBits options, uint64_t capacity) override;
    bool isEndpointStalled(int address) override { return hw.halted(dciFor(address)); }
    IOReturn resetEndpointDataToggle(int address) override;
    void forceReEnumeration() override;
    IOReturn doRemoteWakeGated() override { return kIOReturnUnsupported; }
    void handleControlRequestStateChange(int oldState, int newState) override;
    IOReturn ioGated(_IOUSBDeviceIORequestTag *request) override;
    IOReturn getBufferOptions(uint32_t &options) override
    {
        options = 0;
        return kIOReturnSuccess;
    }

    static SwitchXUDC *running;
    static IOReturn portOwnerAction(OSObject *owner, void *, void *, void *, void *);
};
OSDefineMetaClassAndStructors(SwitchXUDC, IOUSBDeviceController) SwitchXUDC *SwitchXUDC::running;

// ---------------------------------------------------------------------------
// Lifecycle.
bool SwitchXUDC::start(IOService *provider)
{
    stage = Stage::Idle;
    speed = kSpeedNone;
    if (!hw.map()) {
        IOLog("SwitchXUDC: MMIO map failed\n");
        return false;
    }
    loop = IOWorkLoop::workLoop();
    if (!loop || !endpoints[0].bounce.allocate(kEp0Bytes)) {
        IOLog("SwitchXUDC: allocation failed\n");
        return false;
    }
    if (!hw.powerOn()) {
        hw.report("bring-up failed");
        return false;
    }
    // The family builds its command gate, pool and timers on getWorkLoop().
    if (!IOUSBDeviceController::start(provider)) {
        IOLog("SwitchXUDC: IOUSBDeviceController::start failed\n");
        return false;
    }
    timer = IOTimerEventSource::timerEventSource(
        this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &SwitchXUDC::housekeeping));
    if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) {
        IOLog("SwitchXUDC: timer setup failed\n");
        return false;
    }
    attachInterrupt();        // sleeps until the GIC is up
    readyForHost();           // control request, USB device description, registerService()
    handleUSBCableConnect();  // VBUS is overridden on: the cable counts as connected
    startedAt = lastCableKick = nowMs();
    timer->setTimeoutMS(1);
    __atomic_store_n(&running, this, __ATOMIC_RELEASE);
    IOLog("SwitchXUDC: started\n");
    return true;
}

void SwitchXUDC::stop(IOService *provider)
{
    if (timer) timer->cancelTimeout();
    if (interrupt) interrupt->disable();
    goOnBus(false);
    if (timer && loop) loop->removeEventSource(timer);
    if (interrupt && loop) loop->removeEventSource(interrupt);
    IOUSBDeviceController::stop(provider);
}

void SwitchXUDC::free()
{
    OSSafeReleaseNULL(timer);
    OSSafeReleaseNULL(interrupt);
    for (Endpoint &ep : endpoints) ep.bounce.release();
    OSSafeReleaseNULL(loop);
    IOUSBDeviceController::free();
}

// ---------------------------------------------------------------------------
// Events (workloop context: the family's gate is held).
void SwitchXUDC::serviceEvents()
{
    Event e;
    for (uint32_t handled = 0; handled < 64 && hw.nextEvent(e); ++handled) {
        switch (e.type()) {
        case TRB_TRANSFER_EVENT: transferEvent(e); break;
        case TRB_PORT_EVENT: portChange(); break;
        case TRB_SETUP_EVENT: setupEvent(e); break;
        default: break;
        }
    }
    // Always: the ERDP write also clears EHB, without which no further interrupt
    // is raised. Events left over (more than 64) raise a new one.
    hw.finishEvents();
}

void SwitchXUDC::attachInterrupt()
{
    IOService *nub = ioswitch_interrupt_nub(kXudcIntid, false);
    interrupt = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchXUDC::interruptOccurred,
                                                                         &SwitchXUDC::interruptFilter, nub, 0);
    if (!interrupt || loop->addEventSource(interrupt) != kIOReturnSuccess) {
        panic("SwitchXUDC: cannot attach the XUDC interrupt (INTID %u)", kXudcIntid);
    }
    interrupt->enable();
}

// Primary interrupt context: acknowledge ST.IP so the level line drops, then
// let the work loop drain the ring. Not ours (IP clear): nothing to do.
bool SwitchXUDC::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<SwitchXUDC *>(owner);
    if (!self->hw.takeInterrupt()) return false;
    __atomic_fetch_add(&self->interrupts, 1, __ATOMIC_RELAXED);
    return true;
}

void SwitchXUDC::interruptOccurred(OSObject *owner, IOInterruptEventSource *, int)
{
    static_cast<SwitchXUDC *>(owner)->serviceEvents();
}

// Housekeeping (no event handling: that is the interrupt's alone).
void SwitchXUDC::housekeeping(IOTimerEventSource *)
{
    // ST.IP stays set only until the filter acknowledges it, microseconds later.
    const uint32_t taken = __atomic_load_n(&interrupts, __ATOMIC_RELAXED);
    const bool pending = hw.interruptPending();
    if (pending && pendingSeen && taken == interruptsAtPending) {
        char state[160], gic[192];
        hw.describeInterrupt(state, sizeof(state));
        ioswitch_interrupt_describe(kXudcIntid, gic, sizeof(gic));
        panic("SwitchXUDC: XUDC interrupt lost (INTID %u): %s, %s", kXudcIntid, state, gic);
    }
    pendingSeen = pending;
    interruptsAtPending = taken;
    // gated_handleUSBCableConnect() returns early (and latches nothing) until the
    // configurator's description and the functions are in: repeat it until the
    // family first puts us on the bus. Later disconnects are the family's choice.
    uint64_t now = nowMs();
    if (!everOnBus && now - lastCableKick >= 200) {
        lastCableKick = now;
        handleUSBCableConnect();
    }
    if (!configuratorReported && now - startedAt >= 5000) checkConfigurator();
    timer->setTimeoutMS(everOnBus ? kWatchdogMS : 200);
}

// Without a matching IOUSBDeviceConfigurator nothing is ever put on the bus.
void SwitchXUDC::checkConfigurator()
{
    configuratorReported = true;
    OSIterator *clients = getClientIterator();
    bool found = false;
    while (clients && !found) {
        OSObject *client = clients->getNextObject();
        if (!client) break;
        found = !strcmp(client->getMetaClass()->getClassName(), "IOUSBDeviceConfigurator");
    }
    OSSafeReleaseNULL(clients);
    if (!found) {
        OSObject *type = getProperty("ConfigurationType");
        OSString *name = OSDynamicCast(OSString, type);
        IOLog("SwitchXUDC: no IOUSBDeviceConfigurator after 5 s (ConfigurationType %s)\n",
              name ? name->getCStringNoCopy() : "?");
    }
}

// hekate _xusb_handle_port_change() order; reset and connect-done go to the family.
void SwitchXUDC::portChange()
{
    uint32_t sc = hw.portStatus();
    bool ltssm = hw.stateChangeRequested();
    if (sc & PORTSC_PRC) {
        hw.clearPortChange(sc, PORTSC_PRC);
        if (!(sc & PORTSC_PR) && onBus) busReset();
    }
    if (sc & PORTSC_WPR) hw.releaseLtssm();
    if (sc & PORTSC_WRC) hw.clearPortChange(sc, PORTSC_WRC);
    sc = hw.portStatus();
    if (sc & PORTSC_CSC) {
        hw.clearPortChange(sc, PORTSC_CSC);
        if (!(sc & PORTSC_CCS) && speed != kSpeedNone) {  // host gone: drop the configuration
            speed = kSpeedNone;
            ep0Cancel(kIOReturnAborted);
            handleUSBReset();
        }
    }
    if (ltssm) hw.releaseLtssm();
    sc = hw.portStatus();
    if (sc & PORTSC_PLC) {
        hw.clearPortChange(sc, PORTSC_PLC);
        uint32_t link = sc & PORTSC_PLS_MASK;
        if (link == PORTSC_PLS_U3 && !suspended && speed != kSpeedNone) {
            suspended = true;
            handleUSBSuspend();
        } else if (link == PORTSC_PLS_U0 && suspended) {
            suspended = false;
            handleUSBResume();
        }
    }
    sc = hw.portStatus();
    if (sc & PORTSC_CEC) hw.clearPortChange(sc, PORTSC_CEC);
}

// Bus reset done: fresh EP0 (Linux tegra_xudc_reset), then the family's reset
// (address 0, default state, configuration dropped: deactivateEndpoint for
// every pipe) and connection done with the negotiated speed.
void SwitchXUDC::busReset()
{
    ep0Cancel(kIOReturnAborted);
    hw.initEndpoint(0, EP_CONTROL, 64, 0);
    if (!hw.reloadEndpoint(0)) IOLog("SwitchXUDC: EP0 reload timed out\n");
    suspended = false;
    handleUSBReset();
    speed = ((hw.portStatus() >> 10) & 0xf) == 1 ? kSpeedFull : kSpeedHigh;
    handleUSBConnectionDone();
}

// ---------------------------------------------------------------------------
// EP0.
void SwitchXUDC::setupEvent(const Event &e)
{
    memcpy(setup, e.d, sizeof(setup));
    sequence = e.setupSequence();
    hw.setHalt(0, false);
    IOUSBDeviceControlRequest *request = controlRequest();
    if (sequence == 0xfffe || sequence == 0xffff || !request) {  // Tegra210 invalid sequence (tegra-xudc.c)
        hw.setHalt(0, true);
        return;
    }
    // Stages we own are superseded; a family request still pending is aborted
    // by the family itself (premature setup -> abortEndpoint -> handleAbortEndpoint).
    if (stage == Stage::DataOut || stage == Stage::StatusOut) stage = Stage::Idle;
    request->inputSetupPacket(setup);
    request->processSetupPacket();
}

void SwitchXUDC::handleControlRequestStateChange(int, int newState)
{
    IOUSBDeviceControlRequest *request = controlRequest();
    if (!request || stage != Stage::Idle) return;
    DmaBuffer &buffer = endpoints[0].bounce;
    if (newState == kStateOutData || newState == kStateOutDataLong) {
        ep0Length = request->setupLength() < buffer.size ? request->setupLength() : buffer.size;
        cacheFlush(buffer.va, ep0Length ? ep0Length : 1);
        stage = Stage::DataOut;
        ep0Trb = hw.push(0, buffer.pa, ep0Length, trbType(TRB_DATA) | TRB_ISP | TRB_IOC);
        hw.doorbell(0, sequence);
    } else if (newState == kStateOutStatus) {
        stage = Stage::StatusOut;
        ep0Trb = hw.push(0, 0, 0, trbType(TRB_STATUS) | TRB_IOC);
        hw.doorbell(0, sequence);
    }
}

// EP0 IN requests queue: a data stage that is a multiple of the packet size
// and shorter than wLength is followed at once by a zero-length one
// (IOUSBDeviceControlRequest::sendControlInDataGated, zlpCompletion).
IOReturn SwitchXUDC::ep0Io(_IOUSBDeviceIORequestTag *request, bool in)
{
    Endpoint &ep = endpoints[0];
    if (!in || !controlRequest()) return kIOReturnUnsupported;  // OUT data arrives through inputDataPacket
    if (get<uint64_t>(request, kReqLength) > ep.bounce.size || ep.count == kQueueDepth) return kIOReturnNoResources;
    ep.queue[(ep.head + ep.count++) % kQueueDepth] = request;
    ep0Kick();
    return kIOReturnSuccess;
}

// A zero-length IN while the family waits for the device's status (state 7)
// is the status stage; anything else is a data stage (including the ZLP).
void SwitchXUDC::ep0Kick()
{
    Endpoint &ep = endpoints[0];
    IOUSBDeviceControlRequest *control = controlRequest();
    while (stage == Stage::Idle && ep.count && control) {
        _IOUSBDeviceIORequestTag *request = ep.queue[ep.head];
        ep.head = (ep.head + 1) % kQueueDepth;
        ep.count--;
        uint32_t length = (uint32_t)get<uint64_t>(request, kReqLength);
        if (!length && control->state() == kStateInStatus) {
            stage = Stage::StatusIn;
            ep0Length = 0;
            ep0Trb = hw.push(0, 0, 0, trbType(TRB_STATUS) | TRB_IOC | TRB_DIR_IN);
        } else if (!copy(request, ep.bounce.va, length, true)) {
            finish(request, kIOReturnDMAError, 0);
            continue;
        } else {
            cacheFlush(ep.bounce.va, length ? length : 1);
            stage = Stage::DataIn;
            ep0Length = length;
            ep0Trb = hw.push(0, ep.bounce.pa, length, trbType(TRB_DATA) | TRB_ISP | TRB_IOC | TRB_DIR_IN);
        }
        ep0Request = request;
        hw.doorbell(0, sequence);
    }
}

void SwitchXUDC::ep0Complete(const Event &e)
{
    if (stage == Stage::Idle || e.trb() != ep0Trb) return;  // a superseded control transfer
    bool ok = e.code() == COMP_SUCCESS || e.code() == COMP_SHORT_PACKET;
    uint32_t actual = ok && e.residue() <= ep0Length ? ep0Length - e.residue() : 0;
    Stage done = stage;
    stage = Stage::Idle;
    _IOUSBDeviceIORequestTag *request = ep0Request;
    ep0Request = nullptr;
    IOUSBDeviceControlRequest *control = controlRequest();
    DmaBuffer &buffer = endpoints[0].bounce;
    switch (done) {
    case Stage::DataIn: finish(request, ok ? kIOReturnSuccess : kIOReturnAborted, actual); break;  // -> state 8
    case Stage::StatusIn: finish(request, ok ? kIOReturnSuccess : kIOReturnAborted, 0); break;     // -> state 1
    case Stage::DataOut:
        if (ok && control) {
            cacheFlush(buffer.va, actual ? actual : 1);
            control->inputDataPacket(buffer.va, actual);
        }
        break;
    case Stage::StatusOut:
        if (ok && control) control->inputDataPacket(buffer.va, 0);
        break;
    case Stage::Idle: break;
    }
    ep0Kick();
}

void SwitchXUDC::ep0Cancel(IOReturn status)
{
    Endpoint &ep = endpoints[0];
    if (stage != Stage::Idle) hw.dropQueued(0);
    stage = Stage::Idle;
    _IOUSBDeviceIORequestTag *pending[kQueueDepth + 1];
    uint32_t count = 0;
    if (ep0Request) pending[count++] = ep0Request;
    ep0Request = nullptr;
    for (uint32_t i = 0; i < ep.count; ++i) pending[count++] = ep.queue[(ep.head + i) % kQueueDepth];
    ep.head = ep.count = 0;
    for (uint32_t i = 0; i < count; ++i) finish(pending[i], status, 0);
}

// ---------------------------------------------------------------------------
// Data endpoints: one transfer at a time per endpoint through a bounce buffer.
IOReturn SwitchXUDC::ioGated(_IOUSBDeviceIORequestTag *request)
{
    if (!onBus) return kIOReturnNotReady;
    int address = get<int32_t>(request, kReqEndpoint);
    if (!(address & 0x7f)) return ep0Io(request, address & 0x80);
    uint32_t dci = dciFor(address);
    Endpoint &ep = endpoints[dci];
    if (dci >= kMaxDci || !ep.active) return kIOReturnNotReady;
    if (get<uint8_t>(request, kReqIsoc)) return kIOReturnUnsupported;
    if (get<uint64_t>(request, kReqLength) > ep.bounce.size || ep.count == kQueueDepth) return kIOReturnNoResources;
    ep.queue[(ep.head + ep.count++) % kQueueDepth] = request;
    kick(dci);
    return kIOReturnSuccess;
}

void SwitchXUDC::kick(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    if (!ep.active || ep.busy || !ep.count || hw.halted(dci)) return;
    _IOUSBDeviceIORequestTag *request = ep.queue[ep.head];
    uint32_t length = (uint32_t)get<uint64_t>(request, kReqLength);
    bool in = dci & 1;
    if (in && !copy(request, ep.bounce.va, length, true)) {
        ep.head = (ep.head + 1) % kQueueDepth;
        ep.count--;
        finish(request, kIOReturnDMAError, 0);
        return kick(dci);
    }
    cacheFlush(ep.bounce.va, length ? length : 1);
    ep.busyLength = length;
    ep.busyTrb = hw.push(dci, ep.bounce.pa, length, trbType(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    ep.busy = true;
    hw.doorbell(dci);
}

void SwitchXUDC::transferEvent(const Event &e)
{
    uint32_t dci = e.dci();
    if (dci == 0) return ep0Complete(e);
    if (dci >= kMaxDci) return;
    Endpoint &ep = endpoints[dci];
    if (!ep.busy || e.trb() != ep.busyTrb) return;  // dropped by an abort
    ep.busy = false;
    _IOUSBDeviceIORequestTag *request = ep.queue[ep.head];
    ep.head = (ep.head + 1) % kQueueDepth;
    ep.count--;
    bool ok = e.code() == COMP_SUCCESS || e.code() == COMP_SHORT_PACKET;
    uint32_t actual = ok && e.residue() <= ep.busyLength ? ep.busyLength - e.residue() : 0;
    IOReturn status = ok ? kIOReturnSuccess : e.code() == kStallCode ? kIOReturnNotResponding : kIOReturnIOError;
    if (ok && !(dci & 1) && actual) {
        cacheFlush(ep.bounce.va, actual);
        if (!copy(request, ep.bounce.va, actual, false)) status = kIOReturnDMAError;
    }
    finish(request, status, actual);  // the function may queue its next transfer from here
    kick(dci);
}

// Requests are taken off the queue before completion: completions re-enter ioGated().
void SwitchXUDC::cancel(uint32_t dci, IOReturn status)
{
    Endpoint &ep = endpoints[dci];
    _IOUSBDeviceIORequestTag *pending[kQueueDepth];
    uint32_t count = ep.count;
    for (uint32_t i = 0; i < count; ++i) pending[i] = ep.queue[(ep.head + i) % kQueueDepth];
    ep.head = ep.count = 0;
    if (ep.busy) {
        hw.dropQueued(dci);
        ep.busy = false;
    }
    for (uint32_t i = 0; i < count; ++i) finish(pending[i], status, 0);
}

void SwitchXUDC::finish(_IOUSBDeviceIORequestTag *request, IOReturn status, uint32_t actual)
{
    put<int32_t>(request, kReqStatus, status);
    put<uint32_t>(request, kReqActual, actual);  // completeEndpointIO reads it as 32 bits
    completeEndpointIO(request);
}

bool SwitchXUDC::copy(_IOUSBDeviceIORequestTag *request, uint8_t *bytes, uint32_t length, bool toDevice)
{
    if (!length) return true;
    IOMemoryDescriptor *md = get<IOMemoryDescriptor *>(request, kReqBuffer);
    if (!md || md->prepare() != kIOReturnSuccess) return false;
    IOByteCount done = toDevice ? md->readBytes(0, bytes, length) : md->writeBytes(0, bytes, length);
    md->complete();
    return done == length;
}

// ---------------------------------------------------------------------------
// Pure-virtual hardware methods.
void SwitchXUDC::goOnBus(bool on)
{
    if (__atomic_load_n(&gHostOwnsPort, __ATOMIC_ACQUIRE)) {  // the host has the pad: remember the wish
        wantBus = on;
        everOnBus |= on;  // the family is ready: no more cable kicks
        return;
    }
    setBus(on);
}

void SwitchXUDC::setBus(bool on)
{
    if (on == onBus) return;
    if (on) {
        ep0Cancel(kIOReturnAborted);
        hw.initEndpoint(0, EP_CONTROL, 64, 0);
        hw.reloadEndpoint(0);
        hw.connect();
        onBus = everOnBus = true;
        return;
    }
    onBus = false;
    hw.disconnect();
    ep0Cancel(kIOReturnAborted);
    for (uint32_t dci = 2; dci < kMaxDci; ++dci) cancel(dci, kIOReturnAborted);
    speed = kSpeedNone;
    suspended = false;
}

void SwitchXUDC::softDisconnect(uint32_t)
{
    goOnBus(false);
}

// The pad changed hands (switchxudc_set_host_owns_port, on this work loop). To the host: off the bus as a
// cable pull would be, the family's wish kept. Back: on the bus again if the family still wants it.
IOReturn SwitchXUDC::portOwnerChanged()
{
    const bool host = __atomic_load_n(&gHostOwnsPort, __ATOMIC_ACQUIRE);
    if (host && onBus) {
        setBus(false);
        wantBus = true;
    } else if (!host && wantBus) {
        wantBus = false;
        setBus(true);
    }
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::portOwnerAction(OSObject *owner, void *, void *, void *, void *)
{
    return static_cast<SwitchXUDC *>(owner)->portOwnerChanged();
}

extern "C" void switchxudc_set_host_owns_port(bool host)
{
    __atomic_store_n(&gHostOwnsPort, host, __ATOMIC_RELEASE);
    SwitchXUDC *controller = __atomic_load_n(&SwitchXUDC::running, __ATOMIC_ACQUIRE);
    if (controller && controller->getWorkLoop())
        controller->getWorkLoop()->runAction(&SwitchXUDC::portOwnerAction, controller);
}

void SwitchXUDC::forceReEnumeration()
{
    goOnBus(false);
    IOSleep(100);
    goOnBus(true);
}

IOReturn SwitchXUDC::handleStallEndpoint(int address, bool stall)
{
    uint32_t dci = dciFor(address);
    if (dci >= kMaxDci) return kIOReturnBadArgument;
    if (!dci) {
        if (stall) ep0Cancel(kIOReturnAborted);
        hw.setHalt(0, stall);
    } else if (stall) {
        hw.setHalt(dci, true);
    } else {
        hw.resetSequence(dci);
        kick(dci);
    }
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::handleAbortEndpoint(int address)
{
    uint32_t dci = dciFor(address);
    if (dci >= kMaxDci) return kIOReturnBadArgument;
    if (!dci)
        ep0Cancel(kIOReturnAborted);
    else
        cancel(dci, kIOReturnAborted);
    return kIOReturnSuccess;
}

// SET_ADDRESS takes effect before the status stage (hekate, AppleUSBXDCI).
IOReturn SwitchXUDC::handleSetAddressPreStatusPhase(int address)
{
    hw.setAddress(address & 0x7f);
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::resetEndpointDataToggle(int address)
{
    uint32_t dci = dciFor(address);
    if (!dci || dci >= kMaxDci) return kIOReturnSuccess;
    hw.resetSequence(dci);
    kick(dci);
    return kIOReturnSuccess;
}

// AppleUSBXDCI::provideEndpointIDsForConfiguration: per function interface,
// each alternate setting numbers its pipes from the next free IN/OUT endpoint;
// the widest alternate setting decides how many numbers the interface uses.
IOReturn SwitchXUDC::provideEndpointIDsForConfiguration(OSArray *interfaces, uint8_t *ids)
{
    if (!interfaces || !ids) return kIOReturnBadArgument;
    uint32_t nextIn = 1, nextOut = 1, written = 0;
    for (unsigned i = 0; i < interfaces->getCount(); ++i) {
        auto *interface = reinterpret_cast<IOUSBDeviceInterface *>(interfaces->getObject(i));
        if (!interface) continue;
        uint32_t widestIn = 0, widestOut = 0;
        int alternates = interface->getAlternateSettingCount();
        for (int alternate = 0; alternate < alternates; ++alternate) {
            uint32_t in = 0, out = 0;
            int pipes = interface->getPipeCountForAlternateSetting((uint8_t)alternate);
            for (int p = 0; p < pipes; ++p) {
                IOUSBDevicePipe *pipe = interface->getPipeForAlternateSetting(p, (uint8_t)alternate);
                int direction = pipe ? pipe->getDirection() : -1;
                if (direction == 1) {
                    if (nextIn + in > 15) return kIOReturnNoResources;
                    ids[written++] = (uint8_t)(0x80 | (nextIn + in++));
                } else if (direction == 0) {
                    if (nextOut + out > 15) return kIOReturnNoResources;
                    ids[written++] = (uint8_t)(nextOut + out++);
                }
            }
            if (in > widestIn) widestIn = in;
            if (out > widestOut) widestOut = out;
        }
        nextIn += widestIn;
        nextOut += widestOut;
    }
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::activateEndpoint(int address, int type, int maxPacketSize, int interval, int)
{
    uint32_t dci = dciFor(address);
    if (!dci) return kIOReturnSuccess;  // EP0 is always ours
    if (dci >= kMaxDci || maxPacketSize <= 0) return kIOReturnBadArgument;
    bool in = address & 0x80;
    uint32_t kind, period = 0;
    if (type == kTypeBulk) {
        kind = in ? EP_BULK_IN : EP_BULK_OUT;
    } else if (type == kTypeInterrupt) {
        kind = in ? EP_INTR_IN : EP_INTR_OUT;
        uint32_t value = interval < 1 ? 1 : (uint32_t)interval;
        period = speed == kSpeedFull ? log2Floor(value * 8) : (value > 16 ? 16 : value) - 1;  // 2^n x 125 us
    } else {
        return kIOReturnUnsupported;
    }
    Endpoint &ep = endpoints[dci];
    if (!ep.bounce.va && !ep.bounce.allocate(kBounceBytes)) return kIOReturnNoMemory;
    ep.head = ep.count = 0;
    ep.busy = false;
    hw.initEndpoint(dci, kind, (uint32_t)maxPacketSize, period);
    if (!hw.reloadEndpoint(dci)) return kIOReturnTimeout;
    ep.active = true;
    hw.run();  // hekate starts the controller at SET_CONFIGURATION
    return kIOReturnSuccess;
}

IOReturn SwitchXUDC::deactivateEndpoint(int address)
{
    uint32_t dci = dciFor(address);
    if (!dci || dci >= kMaxDci) return kIOReturnSuccess;
    endpoints[dci].active = false;
    cancel(dci, kIOReturnAborted);
    hw.disableEndpoint(dci);
    return kIOReturnSuccess;
}

// AppleUSBXDCI::createIOBuffer: pageable kernel buffer; data is copied through
// our bounce buffers, so placement does not matter.
IOBufferMemoryDescriptor *SwitchXUDC::createIOBuffer(IOOptionBits options, uint64_t capacity)
{
    return IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options | kIOMemoryPageable, capacity,
                                                            ~(uint64_t)PAGE_MASK);
}

// ---------------------------------------------------------------------------
// Late registration: SwitchXUDC's metaclass needs the loaded original family.
namespace {
const OSMetaClass *lookupClass(const char *text)
{
    const OSSymbol *name = OSSymbol::withCString(text);
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    return meta;
}

void setData(OSDictionary *dict, const char *key, const void *bytes, unsigned length)
{
    OSData *data = OSData::withBytes(bytes, length);
    if (data) {
        dict->setObject(key, data);
        data->release();
    }
}
}

// Provider nub: the properties prepareDefaults() reads from the device tree on
// a real iPad (OSData, searched up the service plane). 0x05ac:0x12ab is the
// iPad usbmux identity; the configuration selects IOUSBDeviceConfigurator's
// "standardBringup" personality (one AppleUSBMux interface). The boot-arg
// IOUSBDeviceController-configuration=<type> overrides it.
static IOService *makePort()
{
    OSDictionary *dict = OSDictionary::withCapacity(8);
    if (!dict) return nullptr;
    const uint32_t vendor = 0x05ac, product = 0x12ab;
    setData(dict, "vendor-id", &vendor, sizeof(vendor));
    setData(dict, "product-id", &product, sizeof(product));
    setData(dict, "product-string", "iPad", 5);
    setData(dict, "manufacturer-string", "Apple Inc.", 11);
    setData(dict, "configuration-string", "standardBringup", 16);
    IOService *port = new IOService;
    bool ok = port && port->init(dict) && port->attach(IOService::getResourceService());
    dict->release();
    if (!ok) {
        OSSafeReleaseNULL(port);
        return nullptr;
    }
    port->setName("usb-device");
    return port;
}

extern "C" int switchxudc_start(void)
{
    static uint32_t state;  // 0 untouched, 1 starting, 2 running, 3 failed
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return expected == 2 ? 0 : -1;
    auto fail = [](const char *why) {
        IOLog("SwitchXUDC: %s\n", why);
        __atomic_store_n(&state, 3, __ATOMIC_RELEASE);
        return -1;
    };
    if (OSKext::loadKextWithIdentifier("com.apple.iokit.IOUSBDeviceFamily", false) != KERN_SUCCESS)
        return fail("IOUSBDeviceFamily did not load");
    if (OSKext::loadKextWithIdentifier("com.apple.driver.AppleUSBDeviceMux", false) != KERN_SUCCESS)
        IOLog("SwitchXUDC: AppleUSBDeviceMux did not load\n");
    const OSMetaClass *parent = lookupClass("IOUSBDeviceController");
    if (parent != &IOUSBDeviceController::gMetaClass || parent->getClassSize() != sizeof(IOUSBDeviceController))
        return fail("original IOUSBDeviceController mismatch");
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return fail("preModLoad failed");
    switchxudc_static_init();
    if (OSMetaClass::postModLoad(handle) != KERN_SUCCESS || lookupClass("SwitchXUDC") != &SwitchXUDC::gMetaClass ||
        SwitchXUDC::gMetaClass.getSuperClass() != parent)
        return fail("SwitchXUDC registration failed");
    IOService *port = makePort();
    auto *controller = new SwitchXUDC;
    if (!port || !controller || !controller->init() || !controller->attach(port)) {
        OSSafeReleaseNULL(controller);
        return fail("service setup failed");
    }
    // Once the original start ran, the family holds references: never released.
    if (!controller->start(port)) return fail("start failed");
    __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    return 0;
}
