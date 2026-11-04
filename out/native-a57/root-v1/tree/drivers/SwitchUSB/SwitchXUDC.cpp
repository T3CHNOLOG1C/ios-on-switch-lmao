// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXUDC: the Tegra X1 XUSB device controller as a backend of Apple's original IOUSBDeviceController
// (com.apple.iokit.IOUSBDeviceFamily 2.0.0, 21H461).
//
// Apple's code does everything above the controller, as on an iPad:
//   * the family (prelinked) builds descriptors, runs the EP0 request state machine (IOUSBDeviceControlRequest) and
//     publishes one IOUSBDeviceInterface per function;
//   * configd's USBDeviceConfiguration plug-in reads the controller's ConfigurationType (the provider's
//     configuration-string, /arm-io/usb-device in the ADT) and sets the description from
//     /System/Library/AppleUSBDevice/USBDeviceConfiguration.plist, with the UDID as serial number;
//   * the functions' drivers register: AppleUSBDeviceMux (prelinked) for AppleUSBMux (usbmuxd, lockdownd, Xcode,
//     Finder, atc sync), ptpd (launched by launchd on the PTP interface) for photos;
//   * once every function has registered, the family's startUSBStack asks getConnectionSpeed(): anything but -1 means
//     a cable, so it calls handleUSBCableConnect(), which calls goOnBus(true). Without a cable detector (the BM92T36
//     is not ours yet, see SwitchUSB.h) the port always reports a cable: the pull-up goes on once the functions are
//     ready, and a host plugged in later simply resets the bus.
// This file implements the 18 pure-virtual hardware methods the in-cache reference backend (AppleUSBXDCI, a DWC3
// driver) implements, on top of XudcHardware. USB2 high/full speed; bulk and interrupt endpoints; no isochronous.
//
// Contract (symbolized macOS IOUSBDeviceFamily, checked against the 21H461 code):
// - setup packets go to controlRequest()->inputSetupPacket() + processSetupPacket();
// - io() requests reach ioGated() inside the family's command gate on our work loop; completion = status + actual
//   into the request, then completeEndpointIO(); a nonzero ioGated() return means "not queued";
// - EP0: the family sends IN data and the device's IN status as io(0x80), tells us OUT data stages (state 5/6) and
//   the host's OUT status (state 8) through handleControlRequestStateChange(); both end in inputDataPacket().
// All calls run on one IOWorkLoop: the family's command gate and the XUDC interrupt (GIC SPI 44 = INTID 76: Linux
// tegra210.dtsi, OLED DTB xudc@700d0000) share it, so nothing here needs a lock. The interrupt drains the event ring
// (Linux tegra_xudc_irq); nothing else does. No timer runs in steady state. The one lost-interrupt check is armed only
// on positive evidence that an event is due: the PD controller reports a cable from a host (switchxudc_host_expected,
// SwitchUSB.cpp), and the host's bus reset must follow. 2 s later ST.IP is looked at once; pending with no interrupt
// taken (and still so 50 ms later) means the interrupt does not reach the CPU, and that panics with one line. No cable,
// or a charger that never resets the bus, leaves ST.IP clear: nothing happens.
//
// Host mode (SwitchUSB.cpp) takes USB2 pad 0 from us with switchxudc_set_host_owns_port(true): we go off the bus as a
// cable pull would, keep the family's wish to be on it, and go back when the pad is returned.
#include "OriginalIOUSBDeviceFamily.h"
#include "XudcHardware.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <string.h>
#include "../ArmGIC/IOSwitchInterrupts.h"
#include "SwitchUSB.h"

using namespace xudc;

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void switchxudc_static_init(void) __asm__("__GLOBAL__sub_I_SwitchXUDC.cpp");
}

namespace {
// _IOUSBDeviceIORequestTag fields (IOUSBDeviceController::io, completeEndpointIO; 21H461 code).
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

// IOUSBDeviceControlRequest states that need the controller (see OriginalIOUSBDeviceFamily.h).
enum : int { kStateOutData = 5, kStateOutDataLong = 6, kStateInStatus = 7, kStateOutStatus = 8 };
// USB transfer types as passed to activateEndpoint.
enum : int { kTypeBulk = 2, kTypeInterrupt = 3 };
constexpr int kSpeedFull = 1, kSpeedHigh = 2, kSpeedNone = -1;

constexpr uint32_t kQueueDepth = 16;
constexpr uint32_t kEp0Bytes = 0x1000;      // descriptors and control data
constexpr uint32_t kBounceBytes = 0x10000;  // per data endpoint; longer requests go through in 64 KiB pieces
constexpr uint32_t kXudcIntid = 32 + 44;
constexpr uint32_t kDeliveryCheckMS = 2000, kDeliveryConfirmMS = 50;

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
}  // namespace

class SwitchXUDC : public IOUSBDeviceController {
    OSDeclareDefaultStructors(SwitchXUDC)

        struct Endpoint {
        bool active, busy;
        uint32_t busyTrb, busyLength, done;  // the piece in flight, and the bytes of the head request moved so far
        DmaBuffer bounce;
        _IOUSBDeviceIORequestTag *queue[kQueueDepth];
        uint32_t head, count;
    };
    enum class Stage : uint8_t { Idle, DataIn, StatusIn, DataOut, StatusOut };

    Hardware hw;
    IOWorkLoop *loop;
    IOTimerEventSource *delivery;
    IOFilterInterruptEventSource *interrupt;
    Endpoint endpoints[kMaxDci];
    Stage stage;
    _IOUSBDeviceIORequestTag *ep0Request;
    uint32_t ep0Trb, ep0Length;
    uint8_t setup[8];
    uint16_t sequence;
    int speed;
    bool onBus, suspended, hostPresent, reportedOnBus, hostOwnsPort, wantBus;
    uint8_t deliveryStage;
    volatile uint32_t interrupts;  // ST.IP acknowledged by the filter
    uint32_t interruptsAtCheck;

    void serviceEvents();
    void attachInterrupt();
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void interruptOccurred(OSObject *owner, IOInterruptEventSource *sender, int count);
    void armDeliveryCheck(uint32_t ms, uint8_t nextStage);
    void deliveryCheck(IOTimerEventSource *);
    void portEvent();
    void setupEvent(const Event &e);
    void transferEvent(const Event &e);
    void ep0Complete(const Event &e);
    IOReturn ep0Io(_IOUSBDeviceIORequestTag *request, bool in);
    void ep0Kick();
    void ep0Cancel(IOReturn status);
    void kick(uint32_t dci);
    void cancel(uint32_t dci, IOReturn status);
    void finish(_IOUSBDeviceIORequestTag *request, IOReturn status, uint32_t actual);
    bool copy(_IOUSBDeviceIORequestTag *request, uint32_t offset, uint8_t *bytes, uint32_t length, bool toDevice);
    void setBus(bool on);

public:
    bool begin(IOService *provider, char *why, size_t whySize);
    static SwitchXUDC *running;
    IOReturn hostExpected();
    IOReturn portOwner(bool host);
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
    int getConnectionSpeed() override { return speed == kSpeedNone ? kSpeedHigh : speed; }  // always a cable
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
};
OSDefineMetaClassAndStructors(SwitchXUDC, IOUSBDeviceController) SwitchXUDC *SwitchXUDC::running;

// ---------------------------------------------------------------------------
// Lifecycle.
// Everything that can fail without side effects on the family happens here, before start().
bool SwitchXUDC::begin(IOService *provider, char *why, size_t whySize)
{
    stage = Stage::Idle;
    speed = kSpeedNone;
    if (!hw.map()) {
        snprintf(why, whySize, "MMIO map failed");
        return false;
    }
    loop = IOWorkLoop::workLoop();
    delivery = IOTimerEventSource::timerEventSource(
        this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &SwitchXUDC::deliveryCheck));
    if (!loop || !delivery || !endpoints[0].bounce.allocate(kEp0Bytes)) {
        snprintf(why, whySize, "out of memory");
        return false;
    }
    return hw.powerOn(why, whySize);
}

bool SwitchXUDC::start(IOService *provider)
{
    // The family builds its command gate, pool and timers on getWorkLoop().
    if (!IOUSBDeviceController::start(provider)) return false;
    if (loop->addEventSource(delivery) != kIOReturnSuccess) return false;
    attachInterrupt();  // sleeps until the GIC is up; panics if the interrupt cannot be had
    readyForHost();     // control request, USB device description defaults, registerService()
    return true;
}

void SwitchXUDC::stop(IOService *provider)
{
    if (delivery) delivery->cancelTimeout();
    if (interrupt) interrupt->disable();
    setBus(false);
    if (delivery && loop) loop->removeEventSource(delivery);
    if (interrupt && loop) loop->removeEventSource(interrupt);
    IOUSBDeviceController::stop(provider);
}

void SwitchXUDC::free()
{
    OSSafeReleaseNULL(delivery);
    OSSafeReleaseNULL(interrupt);
    for (Endpoint &ep : endpoints) ep.bounce.release();
    OSSafeReleaseNULL(loop);
    IOUSBDeviceController::free();
}

// ---------------------------------------------------------------------------
// Interrupt (work loop context: the family's gate is held).
void SwitchXUDC::attachInterrupt()
{
    IOService *nub = ioswitch_interrupt_nub(kXudcIntid, false);
    interrupt = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchXUDC::interruptOccurred,
                                                                         &SwitchXUDC::interruptFilter, nub, 0);
    if (!interrupt || loop->addEventSource(interrupt) != kIOReturnSuccess) {
        panic("SwitchUSB: cannot attach the XUDC interrupt (INTID %u)", kXudcIntid);
    }
    interrupt->enable();
}

// Primary interrupt context: acknowledge ST.IP so the level line drops, then let the work loop drain the ring.
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

void SwitchXUDC::serviceEvents()
{
    Event e;
    for (uint32_t handled = 0; handled < 64 && hw.nextEvent(e); ++handled) {
        switch (e.type()) {
        case TRB_TRANSFER_EVENT: transferEvent(e); break;
        case TRB_PORT_EVENT: portEvent(); break;
        case TRB_SETUP_EVENT: setupEvent(e); break;
        default: break;
        }
    }
    // Always: the ERDP write also clears EHB, without which no further interrupt is raised. Events left over (more
    // than 64) raise a new one.
    hw.finishEvents();
}

// One deferred look at ST.IP: pending with no interrupt taken since the check was armed, and still so 50 ms later,
// means the XUDC interrupt does not reach the CPU. Nothing pending (no host, or interrupts working): done.
void SwitchXUDC::armDeliveryCheck(uint32_t ms, uint8_t nextStage)
{
    deliveryStage = nextStage;
    interruptsAtCheck = __atomic_load_n(&interrupts, __ATOMIC_RELAXED);
    delivery->setTimeoutMS(ms);
}

void SwitchXUDC::deliveryCheck(IOTimerEventSource *)
{
    const bool lost = hw.interruptPending() && __atomic_load_n(&interrupts, __ATOMIC_RELAXED) == interruptsAtCheck;
    if (!lost) {
        deliveryStage = 0;
        return;
    }
    if (deliveryStage == 1) {
        armDeliveryCheck(kDeliveryConfirmMS, 2);
        return;
    }
    char state[192], gic[192];
    hw.describeInterrupt(state, sizeof(state));
    ioswitch_interrupt_describe(kXudcIntid, gic, sizeof(gic));
    panic("SwitchUSB: XUDC interrupt lost (INTID %u): %s, %s", kXudcIntid, state, gic);
}

// ---------------------------------------------------------------------------
// Port: reset, connect, suspend (Linux tegra_xudc port handling); the family hears reset, connection done, suspend
// and resume. A host that goes away without a disconnect (the cable pulled with VBUS overridden on) shows as a
// suspend; its next reset starts over.
void SwitchXUDC::portEvent()
{
    PortChange change = hw.portChange();
    if (change.disconnected && speed != kSpeedNone) {
        ep0Cancel(kIOReturnAborted);
        speed = kSpeedNone;
        handleUSBReset();  // the family drops the configuration (deactivateEndpoint for every pipe)
    }
    if (change.disconnected && hostPresent) {
        hostPresent = false;
        IOLog("SwitchUSB: host disconnected\n");
    }
    if (change.reset && onBus) {
        ep0Cancel(kIOReturnAborted);
        stage = Stage::Idle;
        suspended = false;
        handleUSBReset();
        speed = change.fullSpeed ? kSpeedFull : kSpeedHigh;
        handleUSBConnectionDone();
        if (!hostPresent) {
            hostPresent = true;
            IOLog("SwitchUSB: host connected (%s speed)\n", speed == kSpeedHigh ? "high" : "full");
        }
    }
    if (change.suspended && !suspended && speed != kSpeedNone) {
        suspended = true;
        handleUSBSuspend();
        if (hostPresent) {
            hostPresent = false;
            IOLog("SwitchUSB: bus suspended (host asleep or cable out)\n");
        }
    } else if (change.resumed && suspended) {
        suspended = false;
        hostPresent = true;
        handleUSBResume();
    }
}

// ---------------------------------------------------------------------------
// EP0.
void SwitchXUDC::setupEvent(const Event &e)
{
    memcpy(setup, e.d, sizeof(setup));
    sequence = e.setupSequence();
    hw.setHalt(0, false);
    IOUSBDeviceControlRequest *request = controlRequest();
    if (sequence == 0xfffe || sequence == 0xffff || !request) {  // Tegra210 invalid sequence (tegra_xudc.c)
        hw.setHalt(0, true);
        return;
    }
    // A new setup packet ends the previous control transfer: what it left on the ring is dropped. Our own OUT stages
    // end here; a family request still pending is aborted by the family itself (premature setup -> abortEndpoint ->
    // handleAbortEndpoint -> ep0Cancel).
    if (stage != Stage::Idle) hw.dropQueued(0);
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

// EP0 IN requests queue: a data stage that is a multiple of the packet size and shorter than wLength is followed at
// once by a zero-length one (IOUSBDeviceControlRequest::sendControlInDataGated, zlpCompletion).
IOReturn SwitchXUDC::ep0Io(_IOUSBDeviceIORequestTag *request, bool in)
{
    Endpoint &ep = endpoints[0];
    if (!in || !controlRequest()) return kIOReturnUnsupported;  // OUT data arrives through inputDataPacket
    if (get<uint64_t>(request, kReqLength) > ep.bounce.size || ep.count == kQueueDepth) return kIOReturnNoResources;
    ep.queue[(ep.head + ep.count++) % kQueueDepth] = request;
    ep0Kick();
    return kIOReturnSuccess;
}

// A zero-length IN while the family waits for the device's status (state 7) is the status stage; anything else is
// a data stage (including the ZLP).
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
        } else if (!copy(request, 0, ep.bounce.va, length, true)) {
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
// Data endpoints: one request at a time per endpoint, moved through the endpoint's bounce buffer in pieces of up to
// 64 KiB (each a multiple of the packet size, so the host sees one transfer; an OUT piece that ends short ends it).
IOReturn SwitchXUDC::ioGated(_IOUSBDeviceIORequestTag *request)
{
    if (!onBus) return kIOReturnNotReady;
    int address = get<int32_t>(request, kReqEndpoint);
    if (!(address & 0x7f)) return ep0Io(request, address & 0x80);
    uint32_t dci = dciFor(address);
    if (dci >= kMaxDci || !endpoints[dci].active) return kIOReturnNotReady;
    Endpoint &ep = endpoints[dci];
    if (get<uint8_t>(request, kReqIsoc)) return kIOReturnUnsupported;
    if (get<uint64_t>(request, kReqLength) > UINT32_MAX || ep.count == kQueueDepth) return kIOReturnNoResources;
    ep.queue[(ep.head + ep.count++) % kQueueDepth] = request;
    kick(dci);
    return kIOReturnSuccess;
}

void SwitchXUDC::kick(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    while (ep.active && !ep.busy && ep.count && !hw.halted(dci)) {
        _IOUSBDeviceIORequestTag *request = ep.queue[ep.head];
        const uint32_t length = (uint32_t)get<uint64_t>(request, kReqLength);
        const uint32_t left = length - ep.done, piece = left < ep.bounce.size ? left : ep.bounce.size;
        const bool in = dci & 1;
        if (in && !copy(request, ep.done, ep.bounce.va, piece, true)) {
            ep.head = (ep.head + 1) % kQueueDepth;
            ep.count--;
            ep.done = 0;
            finish(request, kIOReturnDMAError, 0);
            continue;
        }
        cacheFlush(ep.bounce.va, piece ? piece : 1);
        ep.busyLength = piece;
        ep.busyTrb = hw.push(dci, ep.bounce.pa, piece, trbType(TRB_NORMAL) | TRB_ISP | TRB_IOC);
        ep.busy = true;
        hw.doorbell(dci);
    }
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
    const bool ok = e.code() == COMP_SUCCESS || e.code() == COMP_SHORT_PACKET;
    const uint32_t actual = ok && e.residue() <= ep.busyLength ? ep.busyLength - e.residue() : 0;
    IOReturn status = ok ? kIOReturnSuccess : e.code() == COMP_STALL ? kIOReturnNotResponding : kIOReturnIOError;
    if (ok && !(dci & 1) && actual) {
        cacheFlush(ep.bounce.va, actual);
        if (!copy(request, ep.done, ep.bounce.va, actual, false)) status = kIOReturnDMAError;
    }
    ep.done += actual;
    const uint32_t length = (uint32_t)get<uint64_t>(request, kReqLength);
    if (status == kIOReturnSuccess && ep.done < length && actual == ep.busyLength) return kick(dci);  // next piece
    const uint32_t moved = ep.done;
    ep.head = (ep.head + 1) % kQueueDepth;
    ep.count--;
    ep.done = 0;
    finish(request, status, moved);  // the function may queue its next transfer from here
    kick(dci);
}

// Requests are taken off the queue before completion: completions re-enter ioGated().
void SwitchXUDC::cancel(uint32_t dci, IOReturn status)
{
    Endpoint &ep = endpoints[dci];
    _IOUSBDeviceIORequestTag *pending[kQueueDepth];
    uint32_t count = ep.count;
    for (uint32_t i = 0; i < count; ++i) pending[i] = ep.queue[(ep.head + i) % kQueueDepth];
    ep.head = ep.count = ep.done = 0;
    if (ep.busy) {
        hw.dropQueued(dci);
        ep.busy = false;
    }
    for (uint32_t i = 0; i < count; ++i) finish(pending[i], status, 0);
}

void SwitchXUDC::finish(_IOUSBDeviceIORequestTag *request, IOReturn status, uint32_t actual)
{
    put<int32_t>(request, kReqStatus, status);
    put<uint64_t>(request, kReqActual, actual);  // read as 32 bits for bulk, 64 for isochronous
    completeEndpointIO(request);
}

bool SwitchXUDC::copy(_IOUSBDeviceIORequestTag *request, uint32_t offset, uint8_t *bytes, uint32_t length,
                      bool toDevice)
{
    if (!length) return true;
    IOMemoryDescriptor *md = get<IOMemoryDescriptor *>(request, kReqBuffer);
    if (!md || md->prepare() != kIOReturnSuccess) return false;
    IOByteCount done = toDevice ? md->readBytes(offset, bytes, length) : md->writeBytes(offset, bytes, length);
    md->complete();
    return done == length;
}

// ---------------------------------------------------------------------------
// Pure-virtual hardware methods.
void SwitchXUDC::goOnBus(bool on)
{
    if (hostOwnsPort) {  // the host has the pad: remember the wish
        wantBus = on;
        return;
    }
    setBus(on);
}

// The PD controller saw a cable from a host (SwitchUSB.cpp): its bus reset is due, so the interrupt can be checked.
IOReturn SwitchXUDC::hostExpected()
{
    if (onBus && !hostPresent && !deliveryStage) armDeliveryCheck(kDeliveryCheckMS, 1);
    return kIOReturnSuccess;
}

// The pad changes hands. To the host: off the bus as a cable pull would be, the family's wish kept. Back: on the bus
// again if the family wants it.
IOReturn SwitchXUDC::portOwner(bool host)
{
    if (host == hostOwnsPort) return kIOReturnSuccess;
    if (host) {
        wantBus = onBus;
        setBus(false);
        hostOwnsPort = true;
    } else {
        hostOwnsPort = false;
        if (wantBus) setBus(true);
    }
    return kIOReturnSuccess;
}

extern "C" void switchxudc_host_expected(void)
{
    SwitchXUDC *controller = __atomic_load_n(&SwitchXUDC::running, __ATOMIC_ACQUIRE);
    if (controller)
        controller->getWorkLoop()->runAction([](OSObject *owner, void *, void *, void *,
                                                void *) { return static_cast<SwitchXUDC *>(owner)->hostExpected(); },
                                             controller);
}

extern "C" void switchxudc_set_host_owns_port(bool host)
{
    SwitchXUDC *controller = __atomic_load_n(&SwitchXUDC::running, __ATOMIC_ACQUIRE);
    if (controller)
        controller->getWorkLoop()->runAction(
            [](OSObject *owner, void *host, void *, void *, void *) {
                return static_cast<SwitchXUDC *>(owner)->portOwner(host != nullptr);
            },
            controller, host ? controller : nullptr);
}

void SwitchXUDC::setBus(bool on)
{
    if (on == onBus) return;
    if (on) {
        ep0Cancel(kIOReturnAborted);
        hw.initEndpoint(0, EP_CONTROL, 64, 0);
        (void)hw.reloadEndpoint(0);
        hw.connect();
        onBus = true;
        if (!reportedOnBus) {
            reportedOnBus = true;
            IOLog("SwitchUSB: functions registered, pull-up on (waiting for a host)\n");
        }
        return;
    }
    onBus = false;
    hw.disconnect();
    ep0Cancel(kIOReturnAborted);
    for (uint32_t dci = 2; dci < kMaxDci; ++dci) cancel(dci, kIOReturnAborted);
    speed = kSpeedNone;
    suspended = hostPresent = false;
}

// AppleUSBXDCI::softDisconnect clears DCTL.RUN_STOP and nothing more: the family puts us back with goOnBus(true).
void SwitchXUDC::softDisconnect(uint32_t)
{
    goOnBus(false);
}

// The family's own off-and-on (userland GoOffAndOnBus). 100 ms off so the host sees the detach.
void SwitchXUDC::forceReEnumeration()
{
    if (hostOwnsPort) return;
    setBus(false);
    IOSleep(100);
    setBus(true);
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

// SET_ADDRESS takes effect before the status stage (hekate, Linux tegra_xudc_ep0_set_address).
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

// AppleUSBXDCI::provideEndpointIDsForConfiguration: per function interface, each alternate setting numbers its pipes
// from the next free IN/OUT endpoint; the widest alternate setting decides how many numbers the interface uses.
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
    ep.head = ep.count = ep.done = 0;
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

// AppleUSBXDCI::createIOBuffer: pageable kernel buffer; data is copied through our bounce buffers, so placement does
// not matter.
IOBufferMemoryDescriptor *SwitchXUDC::createIOBuffer(IOOptionBits options, uint64_t capacity)
{
    return IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options | kIOMemoryPageable, capacity,
                                                            ~(uint64_t)PAGE_MASK);
}

// ---------------------------------------------------------------------------
// Registration: SwitchXUDC's metaclass needs the loaded original family, so its static initializer is staged
// (Drivers.cmake ioswitch_late_object) and runs here.
namespace {
const OSMetaClass *lookupClass(const char *text)
{
    const OSSymbol *name = OSSymbol::withCString(text);
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    return meta;
}
}  // namespace

bool switchxudc_start(IOService *provider, char *why, size_t whySize)
{
    if (OSKext::loadKextWithIdentifier("com.apple.iokit.IOUSBDeviceFamily", false) != KERN_SUCCESS) {
        snprintf(why, whySize, "IOUSBDeviceFamily did not load (is it prelinked?)");
        return false;
    }
    if (OSKext::loadKextWithIdentifier("com.apple.driver.AppleUSBDeviceMux", false) != KERN_SUCCESS) {
        snprintf(why, whySize, "AppleUSBDeviceMux did not load (is it prelinked?)");
        return false;
    }
    const OSMetaClass *parent = lookupClass("IOUSBDeviceController");
    if (parent != &IOUSBDeviceController::gMetaClass || parent->getClassSize() != sizeof(IOUSBDeviceController)) {
        snprintf(why, whySize, "the loaded IOUSBDeviceController is not the 21H461 one");
        return false;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        snprintf(why, whySize, "preModLoad failed");
        return false;
    }
    switchxudc_static_init();
    if (OSMetaClass::postModLoad(handle) != KERN_SUCCESS || lookupClass("SwitchXUDC") != &SwitchXUDC::gMetaClass ||
        SwitchXUDC::gMetaClass.getSuperClass() != parent) {
        snprintf(why, whySize, "SwitchXUDC class registration failed");
        return false;
    }
    auto *controller = OSTypeAlloc(SwitchXUDC);
    if (!controller || !controller->init()) {
        OSSafeReleaseNULL(controller);
        snprintf(why, whySize, "out of memory");
        return false;
    }
    if (!controller->begin(provider, why, whySize)) {
        controller->release();
        return false;
    }
    if (!controller->attach(provider)) {
        controller->release();
        snprintf(why, whySize, "attach to %s failed", provider->getName());
        return false;
    }
    if (!controller->start(provider)) {
        controller->detach(provider);
        controller->release();
        snprintf(why, whySize, "IOUSBDeviceController::start failed");
        return false;
    }
    __atomic_store_n(&SwitchXUDC::running, controller, __ATOMIC_RELEASE);
    controller->release();  // the registry holds it
    return true;
}
