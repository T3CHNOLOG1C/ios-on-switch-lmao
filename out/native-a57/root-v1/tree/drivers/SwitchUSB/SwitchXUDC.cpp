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
// driver) implements, on top of XudcHardware. USB2 high/full speed; bulk and interrupt endpoints; no isochronous. Data
// moves between the controller and the functions' own buffers, scatter-gather, many requests in flight (section "Data
// endpoints").
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
#include "XudcTransfer.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <mach/kmod.h>
#include <os/log.h>
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

constexpr uint32_t kEp0Depth = 16;
constexpr uint32_t kEp0Bytes = 0x1000;  // descriptors and control data
// Data endpoints (section "Data endpoints"): requests per endpoint, on the ring or waiting for room; the physically
// contiguous runs one request may have (more: the serial path); TRBs one request may take; the bounce pool (4 KiB
// pages, XudcTransfer.h); the serial path's buffer, one TRB at a time as before r17.
constexpr uint32_t kQueueDepth = 64;
constexpr uint32_t kMaxRuns = 128;
constexpr uint32_t kMaxPieces = kDataRingTrbs - 2;
constexpr uint32_t kPoolBytes = 512 * 1024, kPoolPages = kPoolBytes / kBouncePageBytes;
constexpr uint32_t kSerialBytes = 0x10000;
constexpr uint64_t kReach = 1ull << 32;  // the XUDC's DMA reach as used here (XudcTransfer.h)
constexpr uint32_t kStatsMs = 2000;      // the stats line: at most this often, only after data moved
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

        // A data endpoint's request (section "Data endpoints"): its memory, prepared from admission to completion; its
        // TRBs on the ring (first index, count); the serial path's progress; the OUT residue probe's IOC on TRB 0.
        struct Request {
        _IOUSBDeviceIORequestTag *tag;
        IOMemoryDescriptor *md;
        uint32_t length, first, trbs, done;
        bool prepared, probe;
    };
    // queue: kQueueDepth requests from head, the first onRing of them with TRBs on the ring (`used` TRBs in all).
    // serial: the head request runs alone one TRB at a time through `serial` (serialTrb, serialLength in flight).
    struct Endpoint {
        bool active, in, serialBusy;
        uint32_t mps, head, count, onRing, used, serialTrb, serialLength;
        Request *queue;
        DmaBuffer serial;
    };
    // Bytes, requests and bounced bytes per direction (0 OUT, 1 IN), requests by the serial path, why packets were
    // bounced (XudcTransfer.h PlanStats), interrupts, events, time in the filter, the event drain and queueing.
    struct Stats {
        uint64_t bytes[2], requests[2], bounced[2], serial[2];
        PlanStats why;
        uint32_t events;
        uint64_t ticks;
    };
    enum class Stage : uint8_t { Idle, DataIn, StatusIn, DataOut, StatusOut };

    Hardware hw;
    IOWorkLoop *loop;
    IOTimerEventSource *delivery;
    IOFilterInterruptEventSource *interrupt;
    Endpoint endpoints[kMaxDci];
    _IOUSBDeviceIORequestTag *ep0Queue[kEp0Depth];
    uint32_t ep0Head, ep0Count;
    DmaBuffer ep0Buffer;
    DmaBuffer pool;  // bounce pages (XudcTransfer.h), below 4 GiB
    uint8_t poolUsed[kPoolPages];
    uint32_t poolFree, poolCursor;
    bool poolFreed;   // pages came back since the last kickWaiting (another endpoint may be waiting for them)
    uint8_t residue;  // OUT residue reading (XudcTransfer.h kResidue*), learned once per boot
    Stats st, stLogged;
    uint64_t statsAt, statsWindow;
    // Scratch of admit() and transferEvent() (work loop only, never nested: neither calls out before it is done).
    Run scratchRuns[kMaxRuns];
    Piece scratchPieces[kMaxPieces];
    uint32_t scratchLengths[kMaxPieces];
    uint32_t interruptsLogged;
    volatile uint64_t filterTicks;
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
    enum class Admit : uint8_t { Queued, Wait, Serial, Failed };
    void kick(uint32_t dci);
    void kickWaiting();
    Admit admit(uint32_t dci, Request &r);
    void startSerial(uint32_t dci);
    void serialPiece(uint32_t dci);
    void serialEvent(uint32_t dci, const Event &e);
    void complete(uint32_t dci, uint32_t k, IOReturn status, uint32_t actual);
    void releaseTrbs(uint32_t dci, Request &r, bool copyBack, uint32_t actual);
    int poolTake();
    void poolGive(uint64_t pa);
    bool inPool(uint64_t pa) const { return pool.va && pa >= pool.pa && pa < (uint64_t)pool.pa + pool.size; }
    void logStats(uint64_t now);
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
    if (!loop || !delivery || !ep0Buffer.allocate(kEp0Bytes)) {
        snprintf(why, whySize, "out of memory");
        return false;
    }
    // The bounce pool, taken at start-up while a contiguous run below 4 GiB is easy to find (as SwitchUSB's host pool).
    // Without it every packet that cannot go direct takes the serial path.
    if (pool.allocate(kPoolBytes))
        poolFree = kPoolPages;
    else
        IOLog("SwitchUSB: no %u KiB bounce pool below 4 GiB: requests that need one go one TRB at a time\n",
              kPoolBytes / 1024);
    nanoseconds_to_absolutetime((uint64_t)kStatsMs * 1000000ULL, &statsWindow);
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
    for (Endpoint &ep : endpoints) {
        ep.serial.release();
        if (ep.queue) IODelete(ep.queue, Request, kQueueDepth);
        ep.queue = nullptr;
    }
    ep0Buffer.release();
    pool.release();
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
    const uint64_t entered = mach_absolute_time();
    if (!self->hw.takeInterrupt()) return false;
    __atomic_fetch_add(&self->interrupts, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&self->filterTicks, mach_absolute_time() - entered, __ATOMIC_RELAXED);
    return true;
}

void SwitchXUDC::interruptOccurred(OSObject *owner, IOInterruptEventSource *, int)
{
    static_cast<SwitchXUDC *>(owner)->serviceEvents();
}

void SwitchXUDC::serviceEvents()
{
    const uint64_t entered = mach_absolute_time();
    Event e;
    for (uint32_t handled = 0; handled < 64 && hw.nextEvent(e); ++handled) {
        ++st.events;
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
    kickWaiting();
    const uint64_t now = mach_absolute_time();
    st.ticks += now - entered;
    if (now - statsAt >= statsWindow) logStats(now);
}

// Work loop: one os_log line per kStatsMs at most, only after data moved (never the console): MB/s, requests and what
// was bounced per direction, interrupts and events per second, and the driver's own CPU time (the interrupt filter, the
// event drain and the queueing in ioGated, which includes prepare() and the bounce copies) as a share of one core.
void SwitchXUDC::logStats(uint64_t now)
{
    const uint32_t irqs = __atomic_load_n(&interrupts, __ATOMIC_RELAXED);
    st.ticks += __atomic_exchange_n(&filterTicks, 0, __ATOMIC_RELAXED);
    const Stats &o = stLogged;
    if (st.bytes[0] == o.bytes[0] && st.bytes[1] == o.bytes[1]) {
        statsAt = now;
        stLogged = st;
        interruptsLogged = irqs;
        return;
    }
    uint64_t ns = 0, busy = 0;
    absolutetime_to_nanoseconds(now - statsAt, &ns);
    absolutetime_to_nanoseconds(st.ticks - o.ticks, &busy);
    if (!ns) ns = 1;
    auto rate = [ns](uint64_t bytes) { return bytes * 100000ULL / ns; };  // hundredths of a MB/s (10^6 bytes)
    const uint64_t in = rate(st.bytes[1] - o.bytes[1]), out = rate(st.bytes[0] - o.bytes[0]);
    const uint64_t cpu = busy * 10000ULL / ns;  // hundredths of a percent of one core
    static const char *const kResidueName[] = {"unknown", "per TRB", "per TD", "broken"};
    os_log(OS_LOG_DEFAULT,
           "SwitchXUDC: stats %llu ms: IN %llu.%02llu MB/s (%llu requests, %llu KiB bounced, %llu serial), OUT "
           "%llu.%02llu MB/s (%llu requests, %llu KiB bounced, %llu serial); %llu interrupts/s, %llu events/s; CPU "
           "%llu.%02llu%% of one core (filter, events, queueing); bounced packets: %u breaks, %u 64K, %u above 4G, %u "
           "cache-line edges; bounce pages free %u/%u; OUT residue %s",
           ns / 1000000, in / 100, in % 100, st.requests[1] - o.requests[1], (st.bounced[1] - o.bounced[1]) / 1024,
           st.serial[1] - o.serial[1], out / 100, out % 100, st.requests[0] - o.requests[0],
           (st.bounced[0] - o.bounced[0]) / 1024, st.serial[0] - o.serial[0],
           (uint64_t)(irqs - interruptsLogged) * 1000000000ULL / ns,
           (uint64_t)(st.events - o.events) * 1000000000ULL / ns, cpu / 100, cpu % 100,
           st.why.discontinuity - o.why.discontinuity, st.why.boundary - o.why.boundary, st.why.high - o.why.high,
           st.why.edge - o.why.edge, poolFree, pool.va ? kPoolPages : 0, kResidueName[residue & 3]);
    stLogged = st;
    interruptsLogged = irqs;
    statsAt = now;
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
    DmaBuffer &buffer = ep0Buffer;
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
    if (!in || !controlRequest()) return kIOReturnUnsupported;  // OUT data arrives through inputDataPacket
    if (get<uint64_t>(request, kReqLength) > ep0Buffer.size || ep0Count == kEp0Depth) return kIOReturnNoResources;
    ep0Queue[(ep0Head + ep0Count++) % kEp0Depth] = request;
    ep0Kick();
    return kIOReturnSuccess;
}

// A zero-length IN while the family waits for the device's status (state 7) is the status stage; anything else is
// a data stage (including the ZLP).
void SwitchXUDC::ep0Kick()
{
    IOUSBDeviceControlRequest *control = controlRequest();
    while (stage == Stage::Idle && ep0Count && control) {
        _IOUSBDeviceIORequestTag *request = ep0Queue[ep0Head];
        ep0Head = (ep0Head + 1) % kEp0Depth;
        ep0Count--;
        uint32_t length = (uint32_t)get<uint64_t>(request, kReqLength);
        if (!length && control->state() == kStateInStatus) {
            stage = Stage::StatusIn;
            ep0Length = 0;
            ep0Trb = hw.push(0, 0, 0, trbType(TRB_STATUS) | TRB_IOC | TRB_DIR_IN);
        } else if (!copy(request, 0, ep0Buffer.va, length, true)) {
            finish(request, kIOReturnDMAError, 0);
            continue;
        } else {
            cacheFlush(ep0Buffer.va, length ? length : 1);
            stage = Stage::DataIn;
            ep0Length = length;
            ep0Trb = hw.push(0, ep0Buffer.pa, length, trbType(TRB_DATA) | TRB_ISP | TRB_IOC | TRB_DIR_IN);
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
    DmaBuffer &buffer = ep0Buffer;
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
    if (stage != Stage::Idle) hw.dropQueued(0);
    stage = Stage::Idle;
    _IOUSBDeviceIORequestTag *pending[kEp0Depth + 1];
    uint32_t count = 0;
    if (ep0Request) pending[count++] = ep0Request;
    ep0Request = nullptr;
    for (uint32_t i = 0; i < ep0Count; ++i) pending[count++] = ep0Queue[(ep0Head + i) % kEp0Depth];
    ep0Head = ep0Count = 0;
    for (uint32_t i = 0; i < count; ++i) finish(pending[i], status, 0);
}

// ---------------------------------------------------------------------------
// Data endpoints (r17-iodma). The controller moves data straight between the bus and the functions' own memory: a
// request (AppleUSBDeviceMux's are kernel virtual ranges over its mbufs, IOMemoryDescriptor::withAddressRanges) is
// prepared once, its physical runs become chained TRBs (XudcTransfer.h plan: whole packets per TRB, IOC on the last,
// ISP on OUT), and up to kQueueDepth requests sit on a kDataRingTrbs ring at once, so the controller goes from one to
// the next without the CPU. Each completion (one event per request, interrupt moderation coalescing bursts) finishes
// its request and lets the next ones on (completion-driven: ioGated and the event drain both call kick). Packets that
// cannot go direct (XudcTransfer.h: across a break, a 64 KiB boundary or 4 GiB, an OUT cache-line edge) go through
// 4 KiB pages of the bounce pool, copied in before an IN and out after an OUT; a request with more runs than kMaxRuns
// or more TRBs than the ring holds, an OUT one too small to probe the residue, or every multi-TRB OUT one once the
// residue proved broken, takes the serial path: alone on the ring, one TRB at a time through the endpoint's 64 KiB
// buffer, as every request did before r17. Cache: the controller does not snoop, so an IN's memory is cleaned before
// the doorbell and an OUT's cleaned and invalidated before it and invalidated again over what arrived
// (IOMemoryDescriptor::performOperation, as SwitchUSB's host mapper does). Everything runs on the work loop.

int SwitchXUDC::poolTake()
{
    if (!poolFree) return -1;
    for (uint32_t n = 0; n < kPoolPages; ++n) {
        const uint32_t page = (poolCursor + n) % kPoolPages;
        if (poolUsed[page]) continue;
        poolUsed[page] = 1;
        --poolFree;
        poolCursor = (page + 1) % kPoolPages;
        return (int)page;
    }
    return -1;
}

void SwitchXUDC::poolGive(uint64_t pa)
{
    const uint32_t page = (uint32_t)((pa - pool.pa) / kBouncePageBytes);
    if (page < kPoolPages && poolUsed[page]) {
        poolUsed[page] = 0;
        ++poolFree;
        poolFreed = true;
    }
}

// The pool is shared: pages one endpoint's completions free may be what another's head request waits for (Admit::Wait
// with nothing of its own in flight, so no completion of its own would ever kick it). After an event drain or a cancel
// gave pages back, every data endpoint with requests off the ring tries again.
void SwitchXUDC::kickWaiting()
{
    if (!poolFreed || !onBus) return;  // off the bus (setBus cancelling every endpoint): nothing to start
    poolFreed = false;
    for (uint32_t dci = 2; dci < kMaxDci; ++dci) {
        const Endpoint &ep = endpoints[dci];
        if (ep.active && ep.queue && !ep.serialBusy && ep.onRing < ep.count) kick(dci);
    }
}

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
    const uint64_t entered = mach_absolute_time();
    Request &r = ep.queue[(ep.head + ep.count++) % kQueueDepth];
    r = {request,
         get<IOMemoryDescriptor *>(request, kReqBuffer),
         (uint32_t)get<uint64_t>(request, kReqLength),
         0,
         0,
         0,
         false,
         false};
    kick(dci);
    st.ticks += mach_absolute_time() - entered;
    return kIOReturnSuccess;
}

// Requests in order onto the ring while it and the pool have room; one doorbell for the lot. A request that cannot be
// prepared fails when it reaches the head (the ones before it complete first, in order); one for the serial path
// starts once the ring has drained.
void SwitchXUDC::kick(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    bool queued = false;
    while (ep.active && !ep.serialBusy && ep.onRing < ep.count && !hw.halted(dci)) {
        Request &r = ep.queue[(ep.head + ep.onRing) % kQueueDepth];
        const Admit a = admit(dci, r);
        if (a == Admit::Queued) {
            queued = true;
            continue;
        }
        if (a == Admit::Wait || ep.onRing) break;  // room comes with completions; the rest waits for the head
        if (a == Admit::Serial) {
            startSerial(dci);
            break;
        }
        complete(dci, 0, kIOReturnDMAError, 0);  // Failed, at the head
    }
    if (queued) hw.doorbell(dci);
}

// One request onto the ring (see the section comment). Queued, or why not.
SwitchXUDC::Admit SwitchXUDC::admit(uint32_t dci, Request &r)
{
    Endpoint &ep = endpoints[dci];
    const bool out = !ep.in;
    if (!r.md) return r.length ? Admit::Failed : Admit::Serial;
    if (!r.prepared) {
        if (r.md->prepare() != kIOReturnSuccess) return Admit::Failed;
        r.prepared = true;
    }
    Run *const runs = scratchRuns;
    uint32_t runCount = 0, offset = 0;
    while (offset < r.length && runCount < kMaxRuns) {
        IOByteCount segment = 0;
        const addr64_t pa = r.md->getPhysicalSegment(offset, &segment, kIOMemoryMapperNone);
        if (!pa || !segment) return Admit::Failed;
        const uint32_t take = segment < r.length - offset ? (uint32_t)segment : r.length - offset;
        if (runCount && runs[runCount - 1].pa + runs[runCount - 1].length == pa)
            runs[runCount - 1].length += take;
        else
            runs[runCount++] = {pa, take};
        offset += take;
    }
    if (offset < r.length) return Admit::Serial;
    const bool probe = out && residue == kResidueUnknown && r.length >= 2 * ep.mps;
    const uint32_t room = kDataRingTrbs - 2 - ep.used;
    Piece *const pieces = scratchPieces;
    PlanStats why = {};
    const uint32_t n =
        plan(runs, runCount, r.length, ep.mps, out, probe, kReach, pieces, room < kMaxPieces ? room : kMaxPieces, &why);
    if (!n) {
        PlanStats ignored = {};
        const bool fits = plan(runs, runCount, r.length, ep.mps, out, probe, kReach, pieces, kMaxPieces, &ignored);
        return fits ? Admit::Wait : Admit::Serial;
    }
    if (out && n > 1 && (residue == kResidueBroken || (residue == kResidueUnknown && !probe))) return Admit::Serial;
    uint32_t bounces = 0;
    for (uint32_t i = 0; i < n; ++i) bounces += pieces[i].bounce;
    if (bounces > poolFree) return bounces > (pool.va ? kPoolPages : 0) ? Admit::Serial : Admit::Wait;
    for (uint32_t i = 0; i < n; ++i) {
        Piece &piece = pieces[i];
        if (!piece.bounce) continue;
        const uint32_t page = (uint32_t)poolTake();
        uint8_t *va = pool.va + page * kBouncePageBytes;
        piece.pa = pool.pa + page * kBouncePageBytes;
        if (!out) r.md->readBytes(piece.offset, va, piece.length);
        cacheFlush(va, piece.length ? piece.length : 1);
    }
    if (r.length) r.md->performOperation(out ? kIOMemoryIncoherentIOFlush : kIOMemoryIncoherentIOStore, 0, r.length);
    const uint32_t base = trbType(TRB_NORMAL) | (out ? TRB_ISP : 0);
    for (uint32_t i = 0; i < n; ++i) {
        const bool last = i + 1 == n;
        const uint32_t flags = base | (last ? TRB_IOC : TRB_CHAIN) | (probe && i == 0 && !last ? TRB_IOC : 0);
        const uint32_t index = hw.pushData(dci, pieces[i].pa, pieces[i].length, n - 1 - i, flags);
        if (!i) r.first = index;
    }
    r.trbs = n;
    r.probe = probe && n > 1;
    ep.used += n;
    ++ep.onRing;
    const uint32_t d = out ? 0 : 1;
    st.bounced[d] += why.bounceBytes;
    st.why.discontinuity += why.discontinuity;
    st.why.boundary += why.boundary;
    st.why.high += why.high;
    st.why.edge += why.edge;
    return Admit::Queued;
}

// The TRBs of an on-ring request given back: what arrived in the client's memory invalidated first (OUT: a line the
// CPU fetched meanwhile is stale, and a bounced packet's bytes written into it would carry it back over the data), then
// bounce pages copied out (OUT, up to `actual`) and freed.
void SwitchXUDC::releaseTrbs(uint32_t dci, Request &r, bool copyBack, uint32_t actual)
{
    Endpoint &ep = endpoints[dci];
    if (copyBack && actual) r.md->performOperation(kIOMemoryIncoherentIOFlush, 0, actual);
    uint32_t index = r.first, offset = 0;
    for (uint32_t i = 0; i < r.trbs; ++i, index = hw.nextIndex(dci, index)) {
        const uint64_t pa = hw.trbBuffer(dci, index);
        const uint32_t length = hw.trbLength(dci, index);
        if (inPool(pa)) {
            uint8_t *va = pool.va + (pa - pool.pa);
            if (copyBack && offset < actual) {
                const uint32_t got = actual - offset < length ? actual - offset : length;
                cacheFlush(va, got);
                r.md->writeBytes(offset, va, got);
            }
            poolGive(pa);
        }
        offset += length;
    }
    ep.used -= r.trbs;
    r.trbs = 0;
}

// The k-th on-ring request (0 but for a lost event) and those before it are done: completed in order, the function
// told (it may queue its next transfer from there), and the ring refilled.
void SwitchXUDC::complete(uint32_t dci, uint32_t k, IOReturn status, uint32_t actual)
{
    Endpoint &ep = endpoints[dci];
    for (uint32_t i = 0; i <= k && ep.count; ++i) {
        Request r = ep.queue[ep.head];
        const bool mine = i == k;
        const IOReturn result = mine ? status : ep.in ? kIOReturnSuccess : kIOReturnIOError;
        const uint32_t moved = mine ? actual : ep.in ? r.length : 0;
        if (ep.onRing) {
            releaseTrbs(dci, r, !ep.in && result == kIOReturnSuccess, moved);
            --ep.onRing;
        }
        if (r.prepared) r.md->complete();
        ep.head = (ep.head + 1) % kQueueDepth;
        ep.count--;
        const uint32_t d = ep.in ? 1 : 0;
        st.bytes[d] += moved;
        ++st.requests[d];
        finish(r.tag, result, moved);
    }
    kick(dci);
}

// The serial path (the section comment): the head request alone, one TRB at a time through ep.serial.
void SwitchXUDC::startSerial(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    Request &r = ep.queue[ep.head];
    if (!ep.serial.va && !ep.serial.allocate(kSerialBytes)) return complete(dci, 0, kIOReturnNoMemory, 0);
    if (r.md && !r.prepared) {
        if (r.md->prepare() != kIOReturnSuccess) return complete(dci, 0, kIOReturnDMAError, 0);
        r.prepared = true;
    }
    r.done = 0;
    ep.serialBusy = true;
    ++st.serial[ep.in ? 1 : 0];
    serialPiece(dci);
}

void SwitchXUDC::serialPiece(uint32_t dci)
{
    Endpoint &ep = endpoints[dci];
    Request &r = ep.queue[ep.head];
    const uint32_t left = r.length - r.done, piece = left < ep.serial.size ? left : ep.serial.size;
    if (ep.in && piece && r.md->readBytes(r.done, ep.serial.va, piece) != piece) {
        ep.serialBusy = false;
        return complete(dci, 0, kIOReturnDMAError, 0);
    }
    cacheFlush(ep.serial.va, piece ? piece : 1);
    ep.serialLength = piece;
    ep.serialTrb = hw.pushData(dci, ep.serial.pa, piece, 0, trbType(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    ep.used = 1;
    hw.doorbell(dci);
}

void SwitchXUDC::serialEvent(uint32_t dci, const Event &e)
{
    Endpoint &ep = endpoints[dci];
    uint32_t index;
    if (!hw.trbIndex(dci, e.trb(), &index) || index != ep.serialTrb) return;  // dropped by an abort
    Request &r = ep.queue[ep.head];
    const bool ok = e.code() == COMP_SUCCESS || e.code() == COMP_SHORT_PACKET;
    const uint32_t actual = ok && e.residue() <= ep.serialLength ? ep.serialLength - e.residue() : 0;
    IOReturn status = ok ? kIOReturnSuccess : e.code() == COMP_STALL ? kIOReturnNotResponding : kIOReturnIOError;
    if (ok && !ep.in && actual) {
        cacheFlush(ep.serial.va, actual);
        if (r.md->writeBytes(r.done, ep.serial.va, actual) != actual) status = kIOReturnDMAError;
    }
    r.done += actual;
    ep.used = 0;
    if (status == kIOReturnSuccess && r.done < r.length && actual == ep.serialLength) return serialPiece(dci);
    ep.serialBusy = false;
    const uint32_t moved = r.done;
    st.bounced[ep.in ? 1 : 0] += moved;
    complete(dci, 0, status, moved);
}

// One transfer event of a data endpoint: the request whose TRBs hold it, its actual length (IN: all of it; OUT:
// XudcTransfer.h outEvent, which also learns the residue reading from the probe), completion.
void SwitchXUDC::transferEvent(const Event &e)
{
    uint32_t dci = e.dci();
    if (dci == 0) return ep0Complete(e);
    if (dci >= kMaxDci) return;
    Endpoint &ep = endpoints[dci];
    if (ep.serialBusy) return serialEvent(dci, e);
    uint32_t index;
    if (!hw.trbIndex(dci, e.trb(), &index)) return;
    for (uint32_t k = 0; k < ep.onRing; ++k) {
        Request &r = ep.queue[(ep.head + k) % kQueueDepth];
        uint32_t at = r.first, pos = 0;
        while (pos < r.trbs && at != index) {
            at = hw.nextIndex(dci, at);
            ++pos;
        }
        if (pos == r.trbs) continue;
        const bool shortPacket = e.code() == COMP_SHORT_PACKET;
        if (e.code() != COMP_SUCCESS && !shortPacket)
            return complete(dci, k, e.code() == COMP_STALL ? kIOReturnNotResponding : kIOReturnIOError, 0);
        if (ep.in) return complete(dci, k, kIOReturnSuccess, e.residue() <= r.length ? r.length - e.residue() : 0);
        uint32_t *const lengths = scratchLengths;
        for (uint32_t i = 0, t = r.first; i < r.trbs; ++i, t = hw.nextIndex(dci, t)) lengths[i] = hw.trbLength(dci, t);
        const OutResult o = outEvent(residue, lengths, r.trbs, pos, e.residue(), shortPacket);
        if (o.learned != kResidueUnknown && o.learned != residue && residue != kResidueBroken) {
            residue = o.learned;
            IOLog("SwitchUSB: OUT transfer events report the residue %s%s\n",
                  residue == kResiduePerTd    ? "of the whole TD (as Linux tegra-xudc reads it)"
                  : residue == kResiduePerTrb ? "of the event's TRB (as xHCI hosts do)"
                                              : "as neither the TD's nor the TRB's",
                  residue == kResidueBroken ? ": multi-TRB OUT requests go one TRB at a time from now" : "");
        }
        if (!o.done) return;  // the probe's packet arrived: the rest of the TD is still coming
        return complete(dci, k, o.failed ? kIOReturnIOError : kIOReturnSuccess, o.failed ? 0 : o.actual);
    }
}

// Requests are taken off the queue before completion: completions re-enter ioGated().
void SwitchXUDC::cancel(uint32_t dci, IOReturn status)
{
    Endpoint &ep = endpoints[dci];
    if (!ep.queue) return;
    if (ep.onRing || ep.serialBusy) hw.dropQueued(dci);
    struct {
        _IOUSBDeviceIORequestTag *tag;
        IOMemoryDescriptor *prepared;
    } pending[kQueueDepth];
    const uint32_t count = ep.count;
    for (uint32_t i = 0; i < count; ++i) {
        Request &r = ep.queue[(ep.head + i) % kQueueDepth];
        if (i < ep.onRing) releaseTrbs(dci, r, false, 0);
        pending[i] = {r.tag, r.prepared ? r.md : nullptr};
    }
    ep.head = ep.count = ep.onRing = ep.used = 0;
    ep.serialBusy = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (pending[i].prepared) pending[i].prepared->complete();
        finish(pending[i].tag, status, 0);
    }
    kickWaiting();
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
    if (!ep.queue) ep.queue = IONewZero(Request, kQueueDepth);
    if (!ep.queue) return kIOReturnNoMemory;
    ep.head = ep.count = ep.onRing = ep.used = 0;
    ep.serialBusy = false;
    ep.in = in;
    ep.mps = (uint32_t)maxPacketSize;
    if (!hw.initEndpoint(dci, kind, (uint32_t)maxPacketSize, period)) return kIOReturnNoMemory;
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

// AppleUSBXDCI::createIOBuffer gives pageable memory its DART maps. Here the controller reads and writes the buffer
// itself (section "Data endpoints"): up to 64 KiB, wired and one physically contiguous run below 4 GiB, so a request on
// it is one TRB, never bounced, and prepare() has nothing to wire; larger (or should that fail) pageable as before, the
// planner taking its pages wherever they are (arm64 ignores a below-4-GiB mask without contiguity: KMA_LOMEM is x86's).
IOBufferMemoryDescriptor *SwitchXUDC::createIOBuffer(IOOptionBits options, uint64_t capacity)
{
    if (capacity && capacity <= kTrbMaxBytes) {
        const IOOptionBits wired = (options & ~(IOOptionBits)kIOMemoryPageable) | kIOMemoryPhysicallyContiguous;
        if (IOBufferMemoryDescriptor *b =
                IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, wired, capacity, 0x00000000fffff000ull))
            return b;
    }
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
