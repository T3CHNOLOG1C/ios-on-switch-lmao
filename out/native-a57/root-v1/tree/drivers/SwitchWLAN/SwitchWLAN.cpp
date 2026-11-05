// SPDX-License-Identifier: BSD-2-Clause-Patent
// Switch OLED Wi-Fi radio: the BCM4356A3 FullMAC chip on Tegra PCIe root port 1, its firmware booted from the
// kernel image, driven over Broadcom's msgbuf rings with the chip's MSI, and a station on top: scans, joins of open
// and WPA2-Personal networks (our own supplicant, SwitchWLANWpa.cpp), the data path to en0 (SwitchWLANNet.cpp),
// and a watchdog that brings the chip back when it resets itself.
//
// Opt-in: nothing here runs unless the boot-arg switch-wlan=1 is set (switch_wlan_publish returns at once
// otherwise), so a radio that misbehaves cannot stop the device booting. One worker thread owns the chip: bring-up
// (UPHY calibration, link training and the firmware's start take seconds), then every request from the network
// side (they only queue work here), joins and their key handshakes, the watchdog and recoveries. The work loop
// takes the MSI: completions, firmware events and received frames. Milestone lines:
//   SwitchWLAN: PCIe link up on port 1 (Gen1 x1): 14e4:43ec, BAR0 ... BAR2 ...
//   SwitchWLAN: firmware: wl0: ... version 7.35.252.108 ... / radio up, MAC ... / en0 ready
//   SwitchWLAN: joining '<ssid>' (WPA2) ... / joined '<ssid>' on channel C, -R dBm / link lost (reason N)
// A failed bring-up stage prints "SwitchWLAN: stopped at <stage>: <why>" and leaves the radio idle; the boot goes on.
// Interrupt rules (drivers/ArmGIC/IOSwitchInterrupts.h): no polled mode. The MSI line that cannot be attached, or
// a chip interrupt latched but never delivered, panics with one line naming the registers and the GIC state. The
// watchdog is not a polling fallback: once a second it looks at what no interrupt reports (the firmware's crash
// flags, a bus reading all ones, the chip's memory decoding turned off by a self-reset, all seen by nxk on Erista).
//
// Sources: MyEcoria/nxk user/wlan (MIT OR Apache-2.0) main.rs (bring-up, `check`, `lost`, `recover`) and wl.rs
// (`init`, `scan`, `join`, `apply`, `add_key`, `disconnect`, `link`); Linux brcmfmac (CTCaer switch-l4t-kernel-4.9)
// fwil_types.h/cfg80211.c for the commands and layouts.
#include <IOKit/IOCommandGate.h>
#include <os/log.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include "../ArmGIC/IOSwitchInterrupts.h"
#include "SwitchWLANAwdlRadio.h"
#include "SwitchWLANCrypto.h"
#include "SwitchWLANPrivate.h"
#include "SwitchWLANStation.h"
#include "SwitchWLANWpa.h"

// firmware.S
extern "C" const uint8_t switch_wlan_fw_image[], switch_wlan_fw_nvram[];
extern "C" const uint64_t switch_wlan_fw_image_size, switch_wlan_fw_nvram_size;

namespace {

// Tegra210 PCIe "msi" interrupt: GIC SPI 99, level high (Linux tegra210.dtsi pcie@1003000; the OLED DTB).
constexpr uint32_t kMsiIntid = 131;
constexpr uint32_t kIoctlTimeoutMs = 2000, kScanTimeoutMs = 15000, kJoinTimeoutMs = 20000, kRecheckMs = 50;
constexpr uint32_t kWatchdogMs = 1000, kRssiRefreshMs = 10000, kTxWaitMs = 1000, kFlowWaitMs = 1000, kStatsMs = 5000;
constexpr uint32_t kRetryFirstMs = 2000, kRetryMaxMs = 60000;

// Firmware commands (brcmfmac fwil_types.h / nxk wl.rs).
constexpr uint32_t WLC_UP = 2, WLC_DOWN = 3, WLC_SET_INFRA = 20, WLC_SCAN = 50, WLC_DISASSOC = 52, WLC_SET_PM = 86;
constexpr uint32_t WLC_GET_RSSI = 127, WLC_GET_BSS_INFO = 136, WLC_GET_VAR = 262, WLC_SET_VAR = 263;
// Firmware events (brcmfmac fweh.h).
enum : uint32_t {
    E_SET_SSID = 0,
    E_JOIN = 1,
    E_AUTH = 3,
    E_DEAUTH = 5,
    E_DEAUTH_IND = 6,
    E_ASSOC = 7,
    E_DISASSOC = 11,
    E_DISASSOC_IND = 12,
    E_LINK = 16,
    E_EAPOL_MSG = 25,
    E_IF = 54,
    E_ESCAN_RESULT = 69,
};
constexpr uint32_t kEventsWanted[] = {E_SET_SSID, E_JOIN,         E_AUTH, E_DEAUTH,    E_DEAUTH_IND, E_ASSOC,
                                      E_DISASSOC, E_DISASSOC_IND, E_LINK, E_EAPOL_MSG, E_IF,         E_ESCAN_RESULT};
// The firmware's own AWDL (Infineon WHD numbering, as in drivers/SwitchWLAN/awdl/awdl_bcm.h): AW, role, generic,
// then 111-120 (frames, status, AW start/end/extension, peer cache). Asked for only while AWDL is open.
constexpr uint32_t kAwdlEvents[] = {96, 97, 98, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120};
bool isAwdlEvent(uint32_t type)
{
    return (type >= 96 && type <= 98) || (type >= 111 && type <= 120);
}
constexpr uint32_t kStatusPartial = 8, kLinkUp = 1;
constexpr uint32_t kWpa2AuthPsk = 0x80, kAesEnabled = 4, kReasonLeaving = 3;
// brcmf_wsec_key_le (164 bytes): index, len, data[32], ..., algo @112, flags @116, iv_initialized @132,
// rxiv {hi u32, lo u16} @140, ea @156.
constexpr size_t kKeySize = 164, kKeyAlgo = 112, kKeyFlags = 116, kKeyIvInit = 132, kKeyRxIv = 140, kKeyEa = 156;
constexpr uint32_t kAlgoAesCcm = 4, kPrimaryKey = 1u << 1;
constexpr uint16_t kEthPLinkCtl = 0x886C;

// Printable SSID for a log line.
void printable(const uint8_t *in, size_t length, char *out)
{
    size_t i = 0;
    for (; i < length && i < 32; ++i) out[i] = (in[i] >= 0x20 && in[i] < 0x7F) ? static_cast<char>(in[i]) : '?';
    out[i] = 0;
}

uint32_t channelToMhz(uint32_t channel)
{
    if (channel == 14) return 2484;
    return channel <= 14 ? 2407 + 5 * channel : 5000 + 5 * channel;
}

void randomBytes(uint8_t *out, size_t length)
{
    read_random(out, static_cast<u_int>(length));
}

// One brcmf_bss_info_le (nxk parse_bss), as escan results and WLC_GET_BSS_INFO give them: its own length, then the
// BSSID, beacon interval, capabilities, SSID, channel (the control channel, else the chanspec's), RSSI and the
// information elements. False if the record does not fit in `room` bytes.
bool parseBss(const uint8_t *b, size_t room, swlan_bss &s)
{
    if (room < 124) return false;
    const uint32_t length = swlan::le32(b + 4);
    if (length < 124 || length > room) return false;
    memset(&s, 0, sizeof(s));
    memcpy(s.bssid, b + 8, 6);
    s.beacon_int = swlan::le16(b + 14);
    s.caps = swlan::le16(b + 16);
    s.ssid_len = b[18] > 32 ? 32 : b[18];
    memcpy(s.ssid, b + 19, s.ssid_len);
    const uint32_t channel = b[88] ? b[88] : (swlan::le16(b + 72) & 0xFF);
    s.freq_mhz = channelToMhz(channel);
    const int16_t rssi = static_cast<int16_t>(swlan::le16(b + 78));
    s.signal_dbm = static_cast<int8_t>(rssi < -128 ? -128 : (rssi > 0 ? 0 : rssi));
    const uint32_t ieOffset = swlan::le16(b + 116), ieLength = swlan::le32(b + 120);
    if (ieOffset + ieLength <= length) {
        s.ie_len = static_cast<uint16_t>(ieLength < sizeof(s.ie) ? ieLength : sizeof(s.ie));
        memcpy(s.ie, b + ieOffset, s.ie_len);
    }
    return true;
}

uint64_t deadlineIn(uint32_t ms)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    return deadline;
}

}  // namespace

class SwitchWLAN : public IOService {
OSDeclareDefaultStructors(SwitchWLAN)

    public : bool start(IOService *provider) override;

    // The station interface (SwitchWLANStation.h), any thread: each takes the gate briefly.
    void setEvents(const swlan_events *events);
    int requestScan(const uint8_t *ssid, size_t length);
    int bssCount();
    int bssGet(int index, swlan_bss *out);
    int currentBss(swlan_bss *out);
    int requestJoin(const uint8_t *ssid, size_t ssidLength, const uint8_t *bssid, const uint8_t *key, size_t keyLength,
                    bool keyIsPmk);
    void requestLeave();
    int requestPower(bool on);
    int transmit(const uint8_t *frame, size_t length);
    void status(swlan_status *out);
    void counters(char *text, size_t size);
    void setMac(const uint8_t mac[6]) { memcpy(mac_, mac, 6); }

    // The firmware's own AWDL for SwitchWLANAwdl.cpp (SwitchWLANAwdlRadio.h §6). Open/close and the iovars wait for
    // the firmware: thread context only, never the work loop. The callbacks are only stored here; the work loop calls
    // them with the gate held (the stack never calls back into a gated call while holding its own lock).
    int awdlOpen(const swlan_awdl_caps **caps);
    void awdlClose();
    int awdlVar(bool set, int32_t bsscfg, const char *name, const void *in, size_t inLength, void *out,
                size_t outLength, size_t *returned);
    int awdlTransmit(const uint8_t *eth, size_t length);
    void awdlCallbacks(swlan_fw_event_fn events, void *eventsContext, swlan_eth_rx_fn rx, void *rxContext,
                       swlan_evt_fn loss, void *lossContext);

private:
    struct JoinParams {
        uint8_t ssid[32];
        size_t ssidLength;
        uint8_t bssid[6];
        bool hasBssid, hasKey, keyIsPmk;
        uint8_t key[64];
        size_t keyLength;
    };
    struct FwEvent {
        uint32_t type, status, reason, flags;
        uint8_t addr[6];
    };
    struct EapolFrame {
        uint16_t length;
        uint8_t frame[512];
    };

    IOWorkLoop *loop_ = nullptr;
    IOCommandGate *gate_ = nullptr;
    IOInterruptEventSource *msi_ = nullptr;
    swlan::TegraPcie pcie_;
    swlan::TegraPcie::Endpoint ep_ = {};
    swlan::Chip chip_;
    swlan::Msgbuf mb_;
    // Chip state: up_ while the rings are served; gone_/halted_ as the MSI path found it.
    bool up_ = false, gone_ = false, halted_ = false, netStarted_ = false;
    uint8_t mac_[6] = {};
    uint64_t interrupts_ = 0;
    char why_[512] = {};
    const swlan_events *events_ = nullptr;

    // Under the gate: work for the worker, requests, station state, scan results, firmware events and EAPOL frames
    // from the work loop.
    bool work_ = false;
    bool scanRequested_ = false, joinRequested_ = false, leaveRequested_ = false, powerRequested_ = false;
    bool powerOn_ = true;
    uint8_t scanSsid_[32] = {};
    size_t scanSsidLength_ = 0;
    JoinParams join_ = {}, last_ = {};
    bool haveLast_ = false;
    swlan_status status_ = {};
    bool scanning_ = false, scanDone_ = false;
    uint32_t scanStatus_ = 0;
    // The escan's sync id, which the firmware copies into its results, and that of the last scan given up (abortScan).
    uint16_t scanSync_ = 0x5357, abortedSync_ = 0;
    // The scan table, the joined access point and the EAPOL queue live outside the object (an IOService is
    // kalloc_type'd, 16 KiB at most). A busy place has more than 32 networks in range (wifid listed 37 on r3v).
    static constexpr unsigned kBssMax = 64, kEapolMax = 8;
    swlan_bss *bss_ = nullptr, *current_ = nullptr;
    uint64_t *bssSeenNs_ = nullptr;
    unsigned bssCount_ = 0;
    FwEvent fwEvents_[16] = {};
    unsigned fwHead_ = 0, fwCount_ = 0;
    EapolFrame *eapol_ = nullptr;
    unsigned eapolHead_ = 0, eapolCount_ = 0;
    uint32_t eapolDropped_ = 0, rxToStack_ = 0, losses_ = 0, recoveries_ = 0;

    // AWDL: the firmware's cap list has "awdl"; open (event mask on, frames and events routed); the AWDL
    // interface's firmware index from its WLC_E_IF (-1 until then).
    bool capAwdl_ = false, awdlOpen_ = false;
    int awdlIfidx_ = -1;
    uint32_t awdlRxDropped_ = 0, awdlEventsIn_ = 0;
    swlan_fw_event_fn awdlEventFn_ = nullptr;
    void *awdlEventCtx_ = nullptr;
    swlan_eth_rx_fn awdlRxFn_ = nullptr;
    void *awdlRxCtx_ = nullptr;
    swlan_evt_fn awdlLossFn_ = nullptr;
    void *awdlLossCtx_ = nullptr;
    bool setEventMask(bool awdl);
    void awdlEvent(const swlan::Event &event);

    // Worker only.
    swlan::wpa::Supplicant sup_;
    bool secured_ = false;
    uint64_t nextRssiNs_ = 0, scanEndNs_ = 0, nextStatsNs_ = 0;
    // The data path's counters at the last stats line (logStats).
    struct Counts {
        uint32_t rx, tx, done, refused;
        uint64_t msis;
        swlan_net_counters net;
    } lastCounts_ = {};
    void snapshot(Counts &c);
    void logStats();

    template <typename F> void locked(F f)
    {
        gate_->runAction(
            [](OSObject *, void *argument, void *, void *, void *) -> IOReturn {
                (*static_cast<F *>(argument))();
                return kIOReturnSuccess;
            },
            &f);
    }
    void kick()
    {
        work_ = true;
        gate_->commandWakeup(&work_);
    }  // gate held
    void waitWork(uint32_t ms);

    static void thread(void *argument, wait_result_t);
    void run();
    bool bringUp();
    bool bootChip(const char **stage);
    void stop(const char *stage);
    void attachInterrupt();
    static void msiFired(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void onEvent(void *context, const swlan::Event &event);
    static void onFrame(void *context, const uint8_t *frame, size_t length, uint8_t ifidx);
    void escanResult(const swlan::Event &event);
    void keepBss(const swlan_bss &bss, uint64_t now);
    void pushEapol(const uint8_t *frame, size_t length);

    struct Call {
        uint32_t cmd;
        const uint8_t *data;
        size_t length, outLength;
        uint8_t *out;
        size_t *outReturned;
        bool ok;
    };
    static IOReturn ioctlGated(OSObject *owner, void *call, void *, void *, void *);
    bool ioctl(uint32_t cmd, const uint8_t *data, size_t length, size_t outLength, uint8_t *out, size_t *outReturned);
    bool getVar(const char *name, uint8_t *out, size_t outLength, size_t *returned);
    bool setVar(const char *name, const void *value, size_t length);
    bool cmdInt(uint32_t cmd, uint32_t value);
    bool radioInit();

    bool startScan(const uint8_t *ssid, size_t ssidLength);
    void abortScan();
    void finishScan();
    bool scanAndWait(const uint8_t *ssid, size_t ssidLength);
    bool readJoinedBss(swlan_bss &out);
    void join(const JoinParams &params);
    int joinOnce(const JoinParams &params, char *ssidText);
    bool ensureFlow(const uint8_t destination[6]);
    uint32_t sendFrame(const uint8_t *frame, size_t length);
    bool waitSent(uint32_t id);
    bool addKey(uint32_t index, const uint8_t key[16], const uint8_t *peer, const uint8_t *rsc);
    bool handleEapol(const uint8_t *frame, size_t length);
    void leave();
    void linkDown(const FwEvent &event);
    bool popEvent(FwEvent &event);
    bool popEapol(EapolFrame &frame);
    void watchdog();
    void lost(const char *why);
    bool recover();
    void setState(uint32_t state);
};

OSDefineMetaClassAndStructors(SwitchWLAN, IOService)

    namespace
{
    SwitchWLAN *gDriver;
}

bool SwitchWLAN::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    bss_ = static_cast<swlan_bss *>(IOMallocZero(kBssMax * sizeof(swlan_bss)));
    current_ = static_cast<swlan_bss *>(IOMallocZero(sizeof(swlan_bss)));
    bssSeenNs_ = static_cast<uint64_t *>(IOMallocZero(kBssMax * sizeof(uint64_t)));
    eapol_ = static_cast<EapolFrame *>(IOMallocZero(kEapolMax * sizeof(EapolFrame)));
    if (!bss_ || !current_ || !bssSeenNs_ || !eapol_) {
        IOLog("SwitchWLAN: no memory for the scan table\n");
        return false;
    }
    loop_ = IOWorkLoop::workLoop();
    gate_ = loop_ ? IOCommandGate::commandGate(this) : nullptr;
    if (!gate_ || loop_->addEventSource(gate_) != kIOReturnSuccess) {
        IOLog("SwitchWLAN: work loop setup failed\n");
        return false;
    }
    status_.state = SWLAN_STA_OFF;
    status_.power = 1;
    memcpy(status_.mac, mac_, 6);
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(&SwitchWLAN::thread, this, &thread) != KERN_SUCCESS) {
        IOLog("SwitchWLAN: worker thread failed to start\n");
        return false;
    }
    thread_deallocate(thread);
    return true;
}

void SwitchWLAN::thread(void *argument, wait_result_t)
{
    static_cast<SwitchWLAN *>(argument)->run();
    // Returning ends the thread (Call_continuation branches to thread_terminate).
}

void SwitchWLAN::stop(const char *stage)
{
    IOLog("SwitchWLAN: stopped at %s: %s\n", stage, why_);
}

void SwitchWLAN::waitWork(uint32_t ms)
{
    locked([&] {
        const uint64_t deadline = deadlineIn(ms);
        while (!work_ && gate_->commandSleep(&work_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
        work_ = false;
    });
}

void SwitchWLAN::setState(uint32_t state)
{
    locked([&] { status_.state = state; });
}

// ---- the MSI and what the work loop takes from the firmware ------------------------------------------------------

// The chip's MSI, level-high SPI 99, masked at the GIC while this runs. AFI vector latch cleared first, then the
// chip's mailbox (Msgbuf::service drops the chip's mask meanwhile, so anything new makes a fresh MSI).
void SwitchWLAN::msiFired(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = OSDynamicCast(SwitchWLAN, owner);
    if (!self) return;
    self->pcie_.msiTake();
    ++self->interrupts_;
    if (!self->up_ || self->gone_ || self->halted_) return;
    bool halted = false;
    const uint32_t status = self->mb_.service(&halted);
    if (status == 0xFFFFFFFF) {
        self->gone_ = true;
        self->kick();
    } else if (halted) {
        self->halted_ = true;
        self->kick();
    }
    self->gate_->commandWakeup(&self->mb_);
}

void SwitchWLAN::attachInterrupt()
{
    if (msi_) return;  // one owner per INTID: kept across chip recoveries
    IOService *nub = ioswitch_interrupt_nub(kMsiIntid, false);
    msi_ = nub ? IOInterruptEventSource::interruptEventSource(this, &SwitchWLAN::msiFired, nub, 0) : nullptr;
    if (!msi_ || loop_->addEventSource(msi_) != kIOReturnSuccess) {
        char gic[160];
        ioswitch_interrupt_describe(kMsiIntid, gic, sizeof(gic));
        panic("SwitchWLAN: cannot attach the PCIe MSI interrupt (INTID %u, nub %p): %s", kMsiIntid, nub, gic);
    }
    msi_->enable();
}

// Work loop. Scan results into the table; the events a join or a joined station follows, and EAPOL frames, to the
// worker.
void SwitchWLAN::onEvent(void *context, const swlan::Event &event)
{
    auto *self = static_cast<SwitchWLAN *>(context);
    // WLC_E_IF about any interface but the station's (in the header or in wl_event_data_if_t's ifidx/bssidx)
    const bool otherIf = event.type == E_IF &&
                         (event.ifidx || event.bsscfgidx || (event.length >= 5 && (event.data[0] || event.data[3])));
    if (self->awdlOpen_ && (isAwdlEvent(event.type) || otherIf)) {
        self->awdlEvent(event);
        return;
    }
    switch (event.type) {
    case E_ESCAN_RESULT: self->escanResult(event); break;
    case E_EAPOL_MSG: {
        // The EAPOL PDU alone: as the Ethernet frame the supplicant takes (sender, then the PAE ethertype).
        uint8_t frame[512] = {};
        const size_t n = event.length < sizeof(frame) - 14 ? event.length : sizeof(frame) - 14;
        memcpy(frame, self->mac_, 6);
        memcpy(frame + 6, event.addr, 6);
        frame[12] = swlan::wpa::kEthPae >> 8;
        frame[13] = swlan::wpa::kEthPae & 0xFF;
        memcpy(frame + 14, event.data, n);
        self->pushEapol(frame, 14 + n);
        break;
    }
    case E_SET_SSID:
    case E_JOIN:
    case E_AUTH:
    case E_DEAUTH:
    case E_DEAUTH_IND:
    case E_ASSOC:
    case E_DISASSOC:
    case E_DISASSOC_IND:
    case E_LINK: {
        FwEvent &e = self->fwEvents_[(self->fwHead_ + self->fwCount_) % 16];
        if (self->fwCount_ == 16)
            self->fwHead_ = (self->fwHead_ + 1) % 16;  // oldest dropped
        else
            ++self->fwCount_;
        e = {event.type, event.status, event.reason, event.flags, {}};
        memcpy(e.addr, event.addr, 6);
        self->kick();
        break;
    }
    default: break;
    }
}

void SwitchWLAN::pushEapol(const uint8_t *frame, size_t length)
{
    if (length > sizeof(eapol_[0].frame)) return;
    if (eapolCount_ == kEapolMax) {
        ++eapolDropped_;
        return;
    }
    EapolFrame &f = eapol_[(eapolHead_ + eapolCount_++) % kEapolMax];
    f.length = static_cast<uint16_t>(length);
    memcpy(f.frame, frame, length);
    kick();
}

// Work loop: a received frame. EAPOL to the supplicant, the rest to en0 once joined (a station has no carrier
// before that).
void SwitchWLAN::onFrame(void *context, const uint8_t *frame, size_t length, uint8_t ifidx)
{
    auto *self = static_cast<SwitchWLAN *>(context);
    if (ifidx) {  // another firmware interface: AWDL's, while it is open
        if (self->awdlOpen_ && self->awdlRxFn_ && static_cast<int>(ifidx) == self->awdlIfidx_)
            self->awdlRxFn_(self->awdlRxCtx_, frame, length);
        else
            ++self->awdlRxDropped_;
        return;
    }
    if (swlan::wpa::isEapol(frame, length)) {
        self->pushEapol(frame, length);
        return;
    }
    if (((frame[12] << 8) | frame[13]) == kEthPLinkCtl) return;  // firmware event frames take the event path
    if (self->status_.state == SWLAN_STA_UP && self->events_ && self->events_->rx) {
        ++self->rxToStack_;
        self->events_->rx(frame, length);
    }
}

// brcmf_escan_result_le: buflen, version, sync id, BSS count, then brcmf_bss_info_le records. A late record of a
// scan given up (abortScan, its final "aborted" one above all) would end the current scan early and empty: dropped
// by its sync id.
void SwitchWLAN::escanResult(const swlan::Event &event)
{
    const uint8_t *data = event.data;
    const uint64_t now = swlan::nowNs();
    if (event.length >= 10 && abortedSync_ && abortedSync_ != scanSync_ && swlan::le16(data + 8) == abortedSync_)
        return;
    if (event.length >= 12) {
        const unsigned count = swlan::le16(data + 10);
        uint32_t at = 12;
        for (unsigned i = 0; i < count && at < event.length; ++i) {
            swlan_bss s;
            if (!parseBss(data + at, event.length - at, s)) break;
            keepBss(s, now);
            at += swlan::le32(data + at + 4);
        }
    }
    if (event.status != kStatusPartial) {
        scanStatus_ = event.status;
        scanDone_ = true;
        kick();
    }
}

// Into the scan table (gate held). A network seen again keeps its strongest record, or a fresh one after a second;
// a new one takes a free slot, or the weakest network's once the table is full (when it is stronger).
void SwitchWLAN::keepBss(const swlan_bss &s, uint64_t now)
{
    unsigned k = 0;
    while (k < bssCount_ && memcmp(bss_[k].bssid, s.bssid, 6)) ++k;
    if (k == bssCount_ && bssCount_ < kBssMax) {
        ++bssCount_;
    } else if (k == bssCount_) {
        k = 0;
        for (unsigned i = 1; i < kBssMax; ++i)
            if (bss_[i].signal_dbm < bss_[k].signal_dbm) k = i;
        if (bss_[k].signal_dbm >= s.signal_dbm) return;
    } else if (bss_[k].signal_dbm > s.signal_dbm && now - bssSeenNs_[k] <= 1000000000ULL) {
        return;
    }
    bss_[k] = s;
    bssSeenNs_[k] = now;
}

// ---- firmware commands -------------------------------------------------------------------------------------------

// One ioctl under the gate: posted, then the gate released while the MSI path delivers the answer. A timeout
// with the chip still signalling (or the AFI's MSI latch set) after a short grace period means the interrupt
// never reached the CPU: panic. A timeout with nothing pending is the firmware's silence: reported, not fatal.
IOReturn SwitchWLAN::ioctlGated(OSObject *owner, void *argument, void *, void *, void *)
{
    auto *self = static_cast<SwitchWLAN *>(owner);
    auto *call = static_cast<Call *>(argument);
    call->ok = false;
    if (!self->up_ || self->gone_ || self->halted_) {
        snprintf(self->why_, sizeof(self->why_), "chip %s",
                 self->gone_     ? "gone from the bus"
                 : self->halted_ ? "firmware halted"
                                 : "not up");
        return kIOReturnNotReady;
    }
    if (!self->mb_.ioctlSubmit(call->cmd, call->data, call->length, call->outLength, self->why_, sizeof(self->why_)))
        return kIOReturnNoResources;
    uint64_t deadline = deadlineIn(kIoctlTimeoutMs);
    while (!self->mb_.replyReady() && !self->gone_ && !self->halted_) {
        if (self->gate_->commandSleep(&self->mb_, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) break;
    }
    if (!self->mb_.replyReady()) {
        uint32_t mailboxInt = 0, mailboxMask = 0;
        if (!self->gone_ && !self->halted_ &&
            (self->mb_.chipSignalling(&mailboxInt, &mailboxMask) || self->pcie_.msiPending())) {
            deadline = deadlineIn(kRecheckMs);
            while (!self->mb_.replyReady() &&
                   self->gate_->commandSleep(&self->mb_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
            if (!self->mb_.replyReady() &&
                (self->mb_.chipSignalling(&mailboxInt, &mailboxMask) || self->pcie_.msiPending())) {
                char gic[160], state[200];
                ioswitch_interrupt_describe(kMsiIntid, gic, sizeof(gic));
                self->pcie_.describe(state, sizeof(state));
                panic(
                    "SwitchWLAN: chip interrupt not delivered: mailbox int %08x mask %08x, %u MSIs so far; %s; GIC %s",
                    mailboxInt, mailboxMask, static_cast<unsigned>(self->interrupts_), state, gic);
            }
        }
        if (!self->mb_.replyReady()) {
            char console[200];
            self->chip_.consoleTail(console, sizeof(console));
            snprintf(self->why_, sizeof(self->why_),
                     "no answer to ioctl %u in %u ms (mailbox int %08x mask %08x, %u "
                     "MSIs); console: %s",
                     call->cmd, kIoctlTimeoutMs, mailboxInt, mailboxMask, static_cast<unsigned>(self->interrupts_),
                     console);
            return kIOReturnTimeout;
        }
    }
    if (self->mb_.replyStatus()) {
        snprintf(self->why_, sizeof(self->why_), "ioctl %u: firmware error %d", call->cmd, self->mb_.replyStatus());
        return kIOReturnError;
    }
    if (call->out) {
        const size_t n = self->mb_.replyLength() < call->outLength ? self->mb_.replyLength() : call->outLength;
        memcpy(call->out, self->mb_.replyData(), n);
        if (call->outReturned) *call->outReturned = n;
    }
    call->ok = true;
    return kIOReturnSuccess;
}

bool SwitchWLAN::ioctl(uint32_t cmd, const uint8_t *data, size_t length, size_t outLength, uint8_t *out,
                       size_t *outReturned)
{
    Call call = {cmd, data, length, outLength, out, outReturned, false};
    gate_->runAction(&SwitchWLAN::ioctlGated, &call);
    return call.ok;
}

// brcmf_fil_iovar_data_get / _set: the variable's name, NUL, then the value.
bool SwitchWLAN::getVar(const char *name, uint8_t *out, size_t outLength, size_t *returned)
{
    return ioctl(WLC_GET_VAR, reinterpret_cast<const uint8_t *>(name), strlen(name) + 1, outLength, out, returned);
}

bool SwitchWLAN::setVar(const char *name, const void *value, size_t length)
{
    uint8_t request[256];
    const size_t nameLength = strlen(name) + 1;
    if (nameLength + length > sizeof(request)) return false;
    memcpy(request, name, nameLength);
    if (length) memcpy(request + nameLength, value, length);
    return ioctl(WLC_SET_VAR, request, nameLength + length, 0, nullptr, nullptr);
}

bool SwitchWLAN::cmdInt(uint32_t cmd, uint32_t value)
{
    uint8_t le[4];
    swlan::put32(le, value);
    return ioctl(cmd, le, sizeof(le), 0, nullptr, nullptr);
}

// nxk wl.rs `init` (brcmf_c_preinit_dcmds, brcmf_config_dongle): our address, the events handled, station mode,
// no power saving, no firmware roaming (it would start a key handshake with another access point behind the
// supplicant's back; wifid decides on roams), every multicast frame passed up (en0 does not hand the firmware its
// multicast list, brcmf_netdev_set_multicast_list's "mcast_list"; without this IPv6 neighbour discovery and mDNS
// never arrive), brcmf's join preference (brcmf_c_set_joinpref_default: by RSSI, 5 GHz counted 8 dB better; the
// firmware picks the access point when wifid names none, and without it r4a sat on 2.4 GHz at -45 dBm next to the
// same network's 5 GHz at -44, sharing the air with Bluetooth), interface up; then the regulatory domain.
bool SwitchWLAN::radioInit()
{
    const uint8_t on[4] = {1, 0, 0, 0};
    // brcmf_join_pref_params {type, length, RSSI gain, band}: RSSI_DELTA (4) +8 dB on WLC_BAND_5G (1), then RSSI (1)
    const uint8_t joinPref[8] = {4, 2, 8, 1, 1, 2, 0, 0};
    bool awdl = false;
    locked([&] { awdl = awdlOpen_; });  // after a recovery an open AWDL keeps its events
    if (!setVar("cur_etheraddr", mac_, 6) || !setEventMask(awdl) || !cmdInt(WLC_SET_INFRA, 1) || !cmdInt(WLC_SET_PM, 0))
        return false;
    if (!setVar("roam_off", on, sizeof(on))) IOLog("SwitchWLAN: roam_off refused: %s\n", why_);
    if (!setVar("allmulti", on, sizeof(on))) IOLog("SwitchWLAN: allmulti refused: %s\n", why_);
    if (!setVar("join_pref", joinPref, sizeof(joinPref))) IOLog("SwitchWLAN: join_pref refused: %s\n", why_);
    if (!cmdInt(WLC_UP, 1)) return false;
    uint8_t country[12] = {};
    size_t n = 0;
    char abbrev[5] = "?", code[5] = "?";
    if (getVar("country", country, sizeof(country), &n) && n >= 12) {
        memcpy(abbrev, country, 4);
        abbrev[4] = 0;
        memcpy(code, country + 8, 4);
        code[4] = 0;
    }
    IOLog("SwitchWLAN: radio up, MAC %02x:%02x:%02x:%02x:%02x:%02x, country %s/%d (%s)\n", mac_[0], mac_[1], mac_[2],
          mac_[3], mac_[4], mac_[5], abbrev, static_cast<int>(swlan::le32(country + 4)), code);
    locked([&] {
        status_.state = powerOn_ ? SWLAN_STA_IDLE : SWLAN_STA_OFF;
        status_.power = powerOn_;
    });
    if (!powerOn_) cmdInt(WLC_DOWN, 1);
    return true;
}

// ---- bring-up, watchdog, recovery --------------------------------------------------------------------------------

// Chip identity, firmware download and start, rings, MSI: shared by the first bring-up and every recovery.
bool SwitchWLAN::bootChip(const char **stage)
{
    *stage = "chip";
    if (!chip_.attach(&pcie_, ep_.bar0, ep_.bar2, why_, sizeof(why_))) return false;
    static uint8_t nvram[4096];
    const size_t nvramLength =
        swlan::nvramConvert(switch_wlan_fw_nvram, switch_wlan_fw_nvram_size, nvram, sizeof(nvram));
    *stage = "firmware";
    if (!nvramLength) {
        snprintf(why_, sizeof(why_), "NVRAM (%llu bytes of text) does not convert", switch_wlan_fw_nvram_size);
        return false;
    }
    if (!chip_.boot(switch_wlan_fw_image, switch_wlan_fw_image_size, nvram, nvramLength, why_, sizeof(why_)))
        return false;
    *stage = "rings";
    if (!mb_.attach(&chip_, &SwitchWLAN::onEvent, &SwitchWLAN::onFrame, this, why_, sizeof(why_))) return false;
    *stage = "MSI";
    if (!pcie_.msiEnable(mb_.msiPA(), why_, sizeof(why_))) return false;
    attachInterrupt();
    locked([&] {
        gone_ = halted_ = false;
        up_ = true;
        mb_.enableInterrupts();
    });
    *stage = "ioctl ver";
    uint8_t ver[256] = {};
    size_t n = 0;
    if (!getVar("ver", ver, sizeof(ver) - 1, &n)) return false;
    for (size_t i = 0; i < n; ++i) {
        if (!ver[i] || ver[i] == '\n') {
            ver[i] = 0;
            break;
        }
        if (ver[i] < 0x20 || ver[i] >= 0x7F) ver[i] = '?';
    }
    IOLog("SwitchWLAN: firmware: %s (%llu MSIs so far)\n", reinterpret_cast<const char *>(ver),
          static_cast<unsigned long long>(interrupts_));
    // What this firmware build was compiled with, as brcmfmac reads it (brcmf_feat_firmware_capabilities): "cap"
    // names p2p, mchan, monitor, rtap, awdl and nan when present. AirDrop needs AWDL, or failing that monitor mode with
    // injection; the image's own strings cannot tell (most of the firmware is in the chip's ROM). Read once; a refusal
    // is logged and changes nothing.
    uint8_t cap[768] = {};
    size_t capLength = 0;
    if (getVar("cap", cap, sizeof(cap) - 1, &capLength)) {
        for (size_t i = 0; i < capLength; ++i) {
            if (!cap[i]) break;
            if (cap[i] < 0x20 || cap[i] >= 0x7F) cap[i] = ' ';
        }
        IOLog("SwitchWLAN: firmware capabilities: %s\n", reinterpret_cast<const char *>(cap));
        // the firmware's own AWDL (r7p8: present on 7.35.252.108); used only when the Data flag asks for it
        capAwdl_ = false;
        for (size_t i = 0; i + 4 <= capLength && cap[i]; ++i)
            if (!memcmp(cap + i, "awdl", 4) && (i == 0 || cap[i - 1] == ' ') && (cap[i + 4] == ' ' || !cap[i + 4]))
                capAwdl_ = true;
    } else {
        IOLog("SwitchWLAN: firmware capabilities not read: %s\n", why_);
    }
    *stage = "radio setup";
    return radioInit();
}

bool SwitchWLAN::bringUp()
{
    IOLog("SwitchWLAN: starting (boot-arg switch-wlan=1), bringing up PCIe port 1\n");
    if (!pcie_.bringUp(ep_, why_, sizeof(why_))) {
        stop("PCIe");
        return false;
    }
    IOLog("SwitchWLAN: PCIe link up on port 1 (Gen%u x%u): %04x:%04x, BAR0 %#x BAR2 %#x\n", ep_.linkSpeed,
          ep_.linkWidth, ep_.vendor, ep_.device, ep_.bar0, ep_.bar2);
    if (ep_.vendor != 0x14E4 || ep_.device != 0x43EC) {
        snprintf(why_, sizeof(why_), "endpoint %04x:%04x is not the BCM4356 (14e4:43ec)", ep_.vendor, ep_.device);
        stop("PCIe");
        return false;
    }
    const char *stage = "chip";
    if (!bootChip(&stage)) {
        stop(stage);
        return false;
    }
    IOLog("SwitchWLAN: BCM%x rev %u, %u cores, RAM %u KiB at %#x\n", chip_.id(), chip_.rev(), chip_.coreCount(),
          chip_.ramSize() / 1024, chip_.ramBase());
    return true;
}

// Once a second while the chip runs: what no interrupt tells (nxk `check`).
void SwitchWLAN::watchdog()
{
    char why[400] = {};
    locked([&] {
        if (!up_) return;
        if (gone_) {
            snprintf(why, sizeof(why), "chip reads all ones (interrupt)");
            return;
        }
        const uint32_t flags = chip_.liveFlags();
        if (flags == 0xFFFFFFFF) {
            snprintf(why, sizeof(why), "chip reads all ones");
        } else if (halted_ || (flags & swlan::Chip::kSharedDied)) {
            char crash[200], console[160];
            chip_.crash(crash, sizeof(crash));
            chip_.consoleTail(console, sizeof(console));
            snprintf(why, sizeof(why), "firmware died (%s); console: %s", crash, console);
        } else if (!pcie_.endpointDecodes()) {
            snprintf(why, sizeof(why), "chip reset itself (memory decoding off)");
        }
    });
    if (why[0]) lost(why);
}

// The chip stopped: reported, the rings abandoned (nothing is sent to it until it is back), the link down.
void SwitchWLAN::lost(const char *why)
{
    bool wasUp = false;
    locked([&] {
        up_ = false;
        wasUp = status_.state == SWLAN_STA_UP;
        status_.state = SWLAN_STA_OFF;
        ++losses_;
    });
    char state[200];
    pcie_.describe(state, sizeof(state));
    IOLog("SwitchWLAN: chip lost (%u so far): %s [%s]\n", losses_, why, state);
    secured_ = false;
    sup_.wipe();
    if (wasUp && events_ && events_->link_lost) events_->link_lost(-1);
    bool awdl = false;
    locked([&] {
        awdl = awdlOpen_;
        awdlIfidx_ = -1;  // the firmware's AWDL interface went with it; its rings are rebuilt at the next attach
    });
    if (awdl && awdlLossFn_) {
        const swlan_awdl_event e = {SWLAN_AWDL_CHIP_LOST, 0, 0, 0};
        awdlLossFn_(awdlLossCtx_, &e);
    }
}

// nxk `recover`: module reset under PERST#, link, endpoint (the same BARs), firmware, rings, radio; then the last
// network again with the key wifid gave for it.
bool SwitchWLAN::recover()
{
    swlan::TegraPcie::Endpoint ep = {};
    if (!pcie_.restart(ep, why_, sizeof(why_))) return false;
    if (ep.bar0 != ep_.bar0 || ep.bar2 != ep_.bar2) {
        snprintf(why_, sizeof(why_), "BARs moved to %#x and %#x", ep.bar0, ep.bar2);
        return false;
    }
    const char *stage = "chip";
    if (!bootChip(&stage)) return false;
    ++recoveries_;
    IOLog("SwitchWLAN: chip back after loss %u\n", losses_);
    locked([&] {
        if (haveLast_ && powerOn_ && !joinRequested_) {
            join_ = last_;
            joinRequested_ = true;
        }
    });
    return true;
}

// ---- scans -------------------------------------------------------------------------------------------------------

// nxk wl.rs `scan`: an escan of every channel (or of one network), active, 3 probes, 80 ms active / 200 ms passive
// per channel (with the firmware's defaults nxk never heard a 5 GHz network). Results arrive in the work loop.
bool SwitchWLAN::startScan(const uint8_t *ssid, size_t ssidLength)
{
    uint8_t request[72] = {};
    swlan::put32(request + 0, 1);  // BRCMF_ESCAN_REQ_VERSION
    swlan::put16(request + 4, 1);  // WL_ESCAN_ACTION_START
    if (ssid && ssidLength && ssidLength <= 32) {
        swlan::put32(request + 8, static_cast<uint32_t>(ssidLength));
        memcpy(request + 12, ssid, ssidLength);
    }
    memset(request + 44, 0xFF, 6);           // any BSSID
    request[50] = 2;                         // DOT11_BSSTYPE_ANY
    request[51] = 0;                         // active
    swlan::put32(request + 52, 3);           // nprobes
    swlan::put32(request + 56, 80);          // active time, ms
    swlan::put32(request + 60, 200);         // passive time, ms
    swlan::put32(request + 64, 0xFFFFFFFF);  // home time: firmware default
    swlan::put32(request + 68, 0);           // every channel
    locked([&] {
        bssCount_ = 0;
        scanDone_ = false;
        scanning_ = true;
        if (status_.state == SWLAN_STA_IDLE) status_.state = SWLAN_STA_SCANNING;
        swlan::put16(request + 6, ++scanSync_);
    });
    scanEndNs_ = swlan::nowNs() + kScanTimeoutMs * 1000000ULL;
    if (setVar("escan", request, sizeof(request))) return true;
    IOLog("SwitchWLAN: scan refused: %s\n", why_);
    locked([&] {
        scanning_ = false;
        scanDone_ = true;
        if (status_.state == SWLAN_STA_SCANNING) status_.state = SWLAN_STA_IDLE;
    });
    return false;
}

// An escan whose last result (status other than partial) has not come in the time a full scan takes: the firmware's
// scan engine is stopped (brcmf_notify_escan_complete with fw_abort: WLC_SCAN on channel -1) and the scan ends as
// aborted with what came, so the next request can start. Without this one lost event would wedge scanning for good.
void SwitchWLAN::abortScan()
{
    IOLog("SwitchWLAN: scan: no final escan result in %u ms, aborting it\n", kScanTimeoutMs);
    uint8_t params[68] = {};       // brcmf_scan_params_le
    memset(params + 36, 0xFF, 6);  // any BSSID
    params[42] = 2;                // DOT11_BSSTYPE_ANY
    swlan::put32(params + 44, 1);  // nprobes
    swlan::put32(params + 48, 0xFFFFFFFF);
    swlan::put32(params + 52, 0xFFFFFFFF);
    swlan::put32(params + 56, 0xFFFFFFFF);
    swlan::put32(params + 60, 1);       // one channel ...
    swlan::put16(params + 64, 0xFFFF);  // ... -1: abort
    if (!ioctl(WLC_SCAN, params, sizeof(params), 0, nullptr, nullptr)) IOLog("SwitchWLAN: scan abort: %s\n", why_);
    locked([&] {
        scanStatus_ = 4;  // WLC_E_STATUS_ABORT
        scanDone_ = true;
        abortedSync_ = scanSync_;
    });
}

void SwitchWLAN::finishScan()
{
    unsigned count = 0;
    uint32_t status = 0;
    locked([&] {
        scanning_ = false;
        count = bssCount_;
        status = scanStatus_;
        if (status_.state == SWLAN_STA_SCANNING) status_.state = SWLAN_STA_IDLE;
    });
    static unsigned logged;
    if (logged++ < 4 || status) IOLog("SwitchWLAN: scan: %u networks%s\n", count, status ? " (scan aborted)" : "");
    if (events_ && events_->scan_done) events_->scan_done();
}

bool SwitchWLAN::scanAndWait(const uint8_t *ssid, size_t ssidLength)
{
    if (!startScan(ssid, ssidLength)) return false;
    bool done = false;
    while (!done && swlan::nowNs() < scanEndNs_) {
        waitWork(200);
        locked([&] { done = scanDone_; });
    }
    if (!done) abortScan();
    finishScan();
    return done;
}

// ---- joins -------------------------------------------------------------------------------------------------------

// The access point the firmware associated with (brcmf_update_bss_info: WLC_GET_BSS_INFO answers a length word, then
// one brcmf_bss_info_le). Without a BSSID from wifid the firmware's own join scan picks the access point, which
// need not be the one in our table: r3v joined a -42 dBm access point on channel 1 while the table held only the
// same network's -71 dBm one on channel 6. Also kept in the scan table, so wifid's next cached scan has it.
bool SwitchWLAN::readJoinedBss(swlan_bss &out)
{
    constexpr size_t kRoom = 1500;  // msgbuf ioctls carry at most 1518 bytes
    uint8_t *info = static_cast<uint8_t *>(IOMallocZeroData(kRoom));
    if (!info) return false;
    swlan::put32(info, kRoom);
    size_t n = 0;
    const bool ok = ioctl(WLC_GET_BSS_INFO, info, 4, kRoom, info, &n) && n > 4 && parseBss(info + 4, n - 4, out);
    IOFreeData(info, kRoom);
    if (ok) locked([&] { keepBss(out, swlan::nowNs()); });
    return ok;
}

bool SwitchWLAN::popEvent(FwEvent &event)
{
    bool got = false;
    locked([&] {
        if (!fwCount_) return;
        event = fwEvents_[fwHead_];
        fwHead_ = (fwHead_ + 1) % 16;
        --fwCount_;
        got = true;
    });
    return got;
}

bool SwitchWLAN::popEapol(EapolFrame &frame)
{
    bool got = false;
    locked([&] {
        if (!eapolCount_) return;
        frame = eapol_[eapolHead_];
        eapolHead_ = (eapolHead_ + 1) % kEapolMax;
        --eapolCount_;
        got = true;
    });
    return got;
}

// The station's TX flow ring, created on first need and kept for the firmware's run (nxk `open_flow`).
bool SwitchWLAN::ensureFlow(const uint8_t destination[6])
{
    swlan::Msgbuf::Flow flow = swlan::Msgbuf::FLOW_CLOSED;
    locked([&] {
        if (mb_.flow() == swlan::Msgbuf::FLOW_CLOSED || mb_.flow() == swlan::Msgbuf::FLOW_REFUSED)
            mb_.flowCreateSubmit(destination, mac_);
        const uint64_t deadline = deadlineIn(kFlowWaitMs);
        while (mb_.flow() == swlan::Msgbuf::FLOW_PENDING &&
               gate_->commandSleep(&mb_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
        flow = mb_.flow();
    });
    if (flow != swlan::Msgbuf::FLOW_OPEN) {
        IOLog("SwitchWLAN: TX flow ring %s (status %u)\n",
              flow == swlan::Msgbuf::FLOW_REFUSED ? "refused" : "not created in time", mb_.flowStatus());
    }
    return flow == swlan::Msgbuf::FLOW_OPEN;
}

uint32_t SwitchWLAN::sendFrame(const uint8_t *frame, size_t length)
{
    if (!ensureFlow(frame)) return 0;
    uint32_t id = 0;
    locked([&] { id = up_ ? mb_.send(frame, length) : 0; });
    return id;
}

// A handshake reply on the air before its keys are installed (brcmf_netdev_wait_pend8021x): message 4 leaves in the
// clear.
bool SwitchWLAN::waitSent(uint32_t id)
{
    bool done = false;
    uint16_t status = 0;
    locked([&] {
        const uint64_t deadline = deadlineIn(kTxWaitMs);
        while (!(done = mb_.sent(id, &status)) &&
               gate_->commandSleep(&mb_, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
    });
    if (done && status) IOLog("SwitchWLAN: handshake frame sent with status %u\n", status);
    return done;
}

// A CCMP key to the firmware (brcmf_cfg80211_add_key, nxk `add_key`): pairwise for the access point, or a group key
// by index with its receive counter.
bool SwitchWLAN::addKey(uint32_t index, const uint8_t key[16], const uint8_t *peer, const uint8_t *rsc)
{
    uint8_t k[kKeySize] = {};
    swlan::put32(k + 0, index);
    swlan::put32(k + 4, 16);
    memcpy(k + 8, key, 16);
    swlan::put32(k + kKeyAlgo, kAlgoAesCcm);
    if (peer)
        memcpy(k + kKeyEa, peer, 6);
    else
        swlan::put32(k + kKeyFlags, kPrimaryKey);
    if (rsc) {
        // Receive IV: bits 16-47 of the packet number, then bits 0-15.
        memcpy(k + kKeyRxIv, rsc + 2, 4);
        memcpy(k + kKeyRxIv + 4, rsc, 2);
        k[kKeyIvInit] = 1;
    }
    const bool ok = setVar("wsec_key", k, sizeof(k));
    memset(k, 0, sizeof(k));
    if (!ok) IOLog("SwitchWLAN: %s key not installed: %s\n", peer ? "pairwise" : "group", why_);
    return ok;
}

// One EAPOL frame through the supplicant (nxk `apply`). True once pairwise and group keys are installed.
bool SwitchWLAN::handleEapol(const uint8_t *frame, size_t length)
{
    if (!secured_) return false;
    static swlan::wpa::Step step;  // worker only; 450 bytes off the stack
    if (const char *why = sup_.receive(frame, length, step)) {
        static unsigned said;
        if (said++ < 8) IOLog("SwitchWLAN: EAPOL-Key frame dropped: %s\n", why);
        return false;
    }
    if (step.replyLength) {
        const uint32_t id = sendFrame(step.reply, step.replyLength);
        if (!id)
            IOLog("SwitchWLAN: handshake reply not sent\n");
        else if (step.hasPtk || step.hasGtk)
            waitSent(id);
    }
    if (step.hasPtk) addKey(0, step.tk, sup_.aa(), nullptr);
    if (step.hasGtk) addKey(step.gtk.index, step.gtk.key, nullptr, step.gtk.rsc);
    memset(&step, 0, sizeof(step));
    return sup_.keyed();
}

// Leave the network (brcmf_link_down: WLC_DISASSOC with reason 3, "leaving").
void SwitchWLAN::leave()
{
    uint8_t scb[12] = {};
    swlan::put32(scb, kReasonLeaving);
    ioctl(WLC_DISASSOC, scb, sizeof(scb), 0, nullptr, nullptr);
    secured_ = false;
    sup_.wipe();
    locked([&] {
        if (status_.state != SWLAN_STA_OFF) status_.state = SWLAN_STA_IDLE;
    });
}

// The firmware event that ended the link, named: a deauthentication or disassociation the access point sent (its
// 802.11 reason code), one the firmware sent itself, or a link-down (brcmf WLC_E_LINK reasons: 1 beacons lost,
// 2 disassociated, 6 reassociation for a roam failed).
void SwitchWLAN::linkDown(const FwEvent &e)
{
    const char *what = e.type == E_DEAUTH_IND     ? "deauthenticated by the access point"
                       : e.type == E_DISASSOC_IND ? "disassociated by the access point"
                       : e.type == E_DEAUTH       ? "deauthentication sent"
                       : e.type == E_DISASSOC     ? "disassociation sent"
                                                  : "link down";
    char counts[200];
    counters(counts, sizeof(counts));
    IOLog("SwitchWLAN: link lost: %s, reason %u (status %u, flags %#x); %s\n", what, e.reason, e.status, e.flags,
          counts);
    secured_ = false;
    sup_.wipe();
    setState(SWLAN_STA_IDLE);
    if (events_ && events_->link_lost) events_->link_lost(static_cast<int>(e.reason));
}

// nxk wl.rs `join`, the security chosen from what the network announced: open, or WPA2-Personal (CCMP, PSK) with
// the PMK wifid derived or one from the passphrase. Returns SWLAN_RES_*.
int SwitchWLAN::joinOnce(const JoinParams &p, char *ssidText)
{
    // The network in the last scan, or a directed scan for it (a hidden network answers only by name).
    swlan_bss target = {};
    bool found = false;
    auto lookup = [&] {
        locked([&] {
            int best = -1;
            for (unsigned i = 0; i < bssCount_; ++i) {
                const swlan_bss &b = bss_[i];
                if (b.ssid_len != p.ssidLength || memcmp(b.ssid, p.ssid, p.ssidLength)) continue;
                if (p.hasBssid && memcmp(b.bssid, p.bssid, 6)) continue;
                if (best < 0 || b.signal_dbm > bss_[best].signal_dbm) best = static_cast<int>(i);
            }
            if (best >= 0) {
                target = bss_[best];
                found = true;
            }
        });
    };
    lookup();
    if (!found) {
        scanAndWait(p.ssid, p.ssidLength);
        lookup();
    }
    if (!found) {
        IOLog("SwitchWLAN: join '%s': not in range\n", ssidText);
        return SWLAN_RES_NOT_FOUND;
    }
    size_t rsnLength = 0;
    const uint8_t *rsn = swlan::wpa::element(target.ie, target.ie_len, 48, &rsnLength);
    bool wpa1 = false;
    for (size_t at = 0; at + 2 <= target.ie_len; at += 2 + target.ie[at + 1]) {
        if (target.ie[at] == 221 && target.ie[at + 1] >= 4 && at + 6 <= target.ie_len && target.ie[at + 2] == 0x00 &&
            target.ie[at + 3] == 0x50 && target.ie[at + 4] == 0xF2 && target.ie[at + 5] == 0x01)
            wpa1 = true;
    }
    const bool privacy = target.caps & (1u << 4);
    const bool secured = rsn != nullptr;
    if (!secured && (wpa1 || privacy)) {
        IOLog("SwitchWLAN: join '%s': %s networks are not supported\n", ssidText, wpa1 ? "WPA (TKIP)" : "WEP");
        return SWLAN_RES_UNSUPPORTED;
    }
    uint8_t pmk[32] = {};
    if (secured) {
        if (const char *why = swlan::wpa::checkRsn(rsn, rsnLength)) {
            IOLog("SwitchWLAN: join '%s': %s\n", ssidText, why);
            return SWLAN_RES_UNSUPPORTED;
        }
        if (!p.hasKey) return SWLAN_RES_BAD_KEY;
        if (p.keyIsPmk && p.keyLength == 32) {
            memcpy(pmk, p.key, 32);
        } else if (!swlan::crypto::pmkFromHex(p.key, p.keyLength, pmk)) {
            if (p.keyLength < 8 || p.keyLength > 63) {
                IOLog("SwitchWLAN: join '%s': passphrase of %zu characters, 8 to 63 needed\n", ssidText, p.keyLength);
                return SWLAN_RES_BAD_KEY;
            }
            swlan::crypto::passphrasePmk(p.key, p.keyLength, p.ssid, p.ssidLength, pmk);
        }
        sup_.init(pmk, mac_, randomBytes);
        memset(pmk, 0, sizeof(pmk));
    }
    secured_ = secured;

    // Stale events and EAPOL frames (an earlier scan or join) must not decide this one.
    locked([&] {
        fwCount_ = eapolCount_ = 0;
        status_.state = SWLAN_STA_AUTH;
    });
    const uint32_t wpaAuth = secured ? kWpa2AuthPsk : 0, wsec = secured ? kAesEnabled : 0, zero = 0;
    uint8_t le[4];
    bool ok = setVar("wpaie", secured ? swlan::wpa::kRsnIe : nullptr, secured ? sizeof(swlan::wpa::kRsnIe) : 0);
    if (!ok) IOLog("SwitchWLAN: wpaie: %s\n", why_);  // the firmware may build its own element
    swlan::put32(le, wpaAuth);
    ok = setVar("wpa_auth", le, 4);
    swlan::put32(le, zero);
    ok = ok && setVar("auth", le, 4);
    swlan::put32(le, wsec);
    ok = ok && setVar("wsec", le, 4);
    // brcmf_ext_join_params_le: SSID, join scan with firmware defaults, the BSSID (or any), no channel list.
    uint8_t joinParams[68] = {};
    swlan::put32(joinParams, static_cast<uint32_t>(p.ssidLength));
    memcpy(joinParams + 4, p.ssid, p.ssidLength);
    joinParams[36] = 0xFF;  // scan type: default
    for (int i = 0; i < 4; ++i) swlan::put32(joinParams + 40 + 4 * i, 0xFFFFFFFF);
    if (p.hasBssid)
        memcpy(joinParams + 56, p.bssid, 6);
    else
        memset(joinParams + 56, 0xFF, 6);
    ok = ok && setVar("join", joinParams, sizeof(joinParams));
    if (!ok) {
        IOLog("SwitchWLAN: join '%s' not started: %s\n", ssidText, why_);
        return SWLAN_RES_FAILED;
    }

    bool associated = false, link = false, keyed = !secured, ieRead = false, announced = false;
    uint8_t bssid[6] = {};
    const uint64_t end = swlan::nowNs() + kJoinTimeoutMs * 1000000ULL;
    while (swlan::nowNs() < end) {
        FwEvent e;
        while (popEvent(e)) {
            const bool hasAddr = e.addr[0] | e.addr[1] | e.addr[2] | e.addr[3] | e.addr[4] | e.addr[5];
            switch (e.type) {
            case E_SET_SSID:
                if (e.status) {
                    static const char *const failures[] = {"", "failed", "timed out", "no such network", "aborted"};
                    IOLog("SwitchWLAN: join '%s' %s (status %u)\n", ssidText,
                          e.status < 5 ? failures[e.status] : "failed", e.status);
                    return e.status == 3 ? SWLAN_RES_NOT_FOUND : e.status == 2 ? SWLAN_RES_TIMEOUT : SWLAN_RES_FAILED;
                }
                associated = true;
                if (hasAddr) memcpy(bssid, e.addr, 6);
                break;
            case E_ASSOC:
                if (!e.status && hasAddr) memcpy(bssid, e.addr, 6);
                break;
            case E_LINK:
                link = e.flags & kLinkUp;
                if (link && hasAddr) memcpy(bssid, e.addr, 6);
                break;
            case E_DEAUTH:
            case E_DEAUTH_IND:
            case E_DISASSOC:
            case E_DISASSOC_IND:
                if (secured && sup_.unconfirmed()) {
                    IOLog("SwitchWLAN: join '%s': key handshake refused, wrong password? (reason %u)\n", ssidText,
                          e.reason);
                    return SWLAN_RES_BAD_KEY;
                }
                IOLog("SwitchWLAN: join '%s': sent away by the access point (reason %u)\n", ssidText, e.reason);
                return SWLAN_RES_REJECTED;
            default: break;
            }
        }
        if (associated && link && !announced) {
            // Associated: the access point as the firmware has it, then the network side (wifid takes the link up now and
            // holds it for its clients until the join completes, connect_done, after the key handshake).
            announced = true;
            swlan_bss joined = target;
            if (!readJoinedBss(joined)) {
                IOLog("SwitchWLAN: join '%s': the joined access point's record is unreadable: %s\n", ssidText, why_);
                if (bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5]) memcpy(joined.bssid, bssid, 6);
            }
            locked([&] {
                *current_ = joined;
                memcpy(status_.bssid, joined.bssid, 6);
                memcpy(status_.ssid, p.ssid, p.ssidLength);
                status_.ssid_len = static_cast<uint8_t>(p.ssidLength);
                status_.freq_mhz = joined.freq_mhz;
                status_.signal_dbm = joined.signal_dbm;
                status_.state = secured && !keyed ? SWLAN_STA_KEYS : SWLAN_STA_ASSOC;
            });
            if (events_ && events_->associated) events_->associated();
        }
        EapolFrame f;
        while (secured && popEapol(f)) {
            // Message 2 repeats the RSN element the association request carried: the firmware's, if it built one
            // (brcmf_get_assoc_ies: assoc_info gives the request's IE length, assoc_req_ies the IEs).
            if (!ieRead) {
                ieRead = true;
                uint8_t info[64] = {}, ies[512] = {};
                size_t infoLength = 0, iesLength = 0;
                if (getVar("assoc_info", info, sizeof(info), &infoLength) && infoLength >= 4 &&
                    getVar("assoc_req_ies", ies, sizeof(ies), &iesLength)) {
                    size_t used = swlan::le32(info);
                    if (used > iesLength) used = iesLength;
                    size_t n = 0;
                    if (const uint8_t *ours = swlan::wpa::element(ies, used, 48, &n)) sup_.setIe(ours, n);
                }
            }
            keyed |= handleEapol(f.frame, f.length);
        }
        bool leaving = false;
        locked([&] { leaving = leaveRequested_; });
        if (leaving) return SWLAN_RES_FAILED;
        if (associated && link && keyed) {
            // The firmware may turn power saving back on when it associates: the access point would then hold what it
            // has for us until the next beacon (nxk saw pings of 100-170 ms).
            cmdInt(WLC_SET_PM, 0);
            ensureFlow(bssid);
            uint32_t mhz = 0;
            int32_t rssi = 0;
            uint8_t answer[4] = {};
            size_t n = 0;
            uint8_t scb[12] = {};
            const bool fresh = ioctl(WLC_GET_RSSI, scb, sizeof(scb), sizeof(answer), answer, &n) && n >= 4;
            locked([&] {
                if (fresh) {
                    const int32_t r = static_cast<int32_t>(swlan::le32(answer));
                    status_.signal_dbm = current_->signal_dbm = static_cast<int8_t>(r < -128 ? -128 : (r > 0 ? 0 : r));
                }
                mhz = status_.freq_mhz;
                rssi = status_.signal_dbm;
                status_.state = SWLAN_STA_UP;
                last_ = p;
                haveLast_ = true;
            });
            IOLog("SwitchWLAN: joined '%s' (%s) at %u MHz, %d dBm\n", ssidText, secured ? "WPA2" : "open", mhz, rssi);
            nextRssiNs_ = swlan::nowNs() + kRssiRefreshMs * 1000000ULL;
            nextStatsNs_ = swlan::nowNs() + kStatsMs * 1000000ULL;
            snapshot(lastCounts_);
            return SWLAN_RES_OK;
        }
        waitWork(100);
    }
    if (secured && sup_.unconfirmed()) {
        IOLog("SwitchWLAN: join '%s': no message 3, wrong password?\n", ssidText);
        return SWLAN_RES_BAD_KEY;
    }
    IOLog("SwitchWLAN: join '%s' timed out (associated %d, link %d, keyed %d, handshake %s)\n", ssidText, associated,
          link, keyed, secured ? (sup_.hasAa() ? "started" : "not started") : "none");
    return SWLAN_RES_TIMEOUT;
}

void SwitchWLAN::join(const JoinParams &p)
{
    char ssidText[33];
    printable(p.ssid, p.ssidLength, ssidText);
    bool wasScanning = false;
    locked([&] { wasScanning = scanning_; });
    if (wasScanning) {
        // A scan in progress finishes first (the firmware refuses a join meanwhile).
        bool done = false;
        while (!done && swlan::nowNs() < scanEndNs_) {
            waitWork(200);
            locked([&] { done = scanDone_; });
        }
        if (!done) abortScan();
        finishScan();
    }
    IOLog("SwitchWLAN: joining '%s'%s\n", ssidText,
          p.hasKey ? (p.keyIsPmk ? " with the PMK wifid gave"
                                 : " with a "
                                   "passphrase")
                   : "");
    const int result = joinOnce(p, ssidText);
    if (result != SWLAN_RES_OK) {
        uint8_t scb[12] = {};
        swlan::put32(scb, kReasonLeaving);
        ioctl(WLC_DISASSOC, scb, sizeof(scb), 0, nullptr, nullptr);
        secured_ = false;
        sup_.wipe();
    }
    locked([&] {
        status_.result = result;
        if (result != SWLAN_RES_OK && status_.state != SWLAN_STA_OFF) status_.state = SWLAN_STA_IDLE;
    });
    if (events_ && events_->connect_done) events_->connect_done(result);
}

// ---- the worker ---------------------------------------------------------------------------------------------------

void SwitchWLAN::run()
{
    if (!bringUp()) return;
    switch_wlan_net_start(mac_);
    netStarted_ = true;
    IOLog("SwitchWLAN: en0 ready, %u MSIs so far\n", static_cast<unsigned>(interrupts_));

    uint64_t nextWatchdog = 0, retryAt = 0;
    uint32_t backoff = kRetryFirstMs;
    for (;;) {
        waitWork(kWatchdogMs);
        const uint64_t now = swlan::nowNs();
        bool chipUp = false;
        locked([&] { chipUp = up_; });
        if (chipUp && now >= nextWatchdog) {
            nextWatchdog = now + kWatchdogMs * 1000000ULL;
            watchdog();
            locked([&] { chipUp = up_; });
            if (!chipUp) {
                retryAt = now + kRetryFirstMs * 1000000ULL;
                backoff = kRetryFirstMs;
            }
        }
        if (!chipUp) {
            if (now < retryAt) continue;
            if (recover()) {
                backoff = kRetryFirstMs;
            } else {
                IOLog("SwitchWLAN: recovery failed: %s; next try in %u s\n", why_, backoff / 1000);
                retryAt = swlan::nowNs() + backoff * 1000000ULL;
                backoff = backoff * 2 > kRetryMaxMs ? kRetryMaxMs : backoff * 2;
                continue;
            }
        }

        // Requests from the network side.
        bool scan = false, doJoin = false, doLeave = false, doPower = false, powerOn = true, scanFinished = false;
        bool scanOverdue = false;
        uint8_t scanSsid[32];
        size_t scanSsidLength = 0;
        JoinParams params = {};
        locked([&] {
            scan = scanRequested_ && !scanning_;
            if (scan) {
                scanRequested_ = false;
                memcpy(scanSsid, scanSsid_, sizeof(scanSsid));
                scanSsidLength = scanSsidLength_;
            }
            doJoin = joinRequested_;
            if (doJoin) {
                params = join_;
                memset(join_.key, 0, sizeof(join_.key));
                joinRequested_ = false;
            }
            doLeave = leaveRequested_;
            leaveRequested_ = false;
            doPower = powerRequested_;
            powerRequested_ = false;
            powerOn = powerOn_;
            scanFinished = scanning_ && scanDone_;
            scanOverdue = scanning_ && !scanDone_ && now >= scanEndNs_;
        });
        if (doPower) {
            if (!powerOn) {
                leave();
                cmdInt(WLC_DOWN, 1);
                setState(SWLAN_STA_OFF);
            } else {
                cmdInt(WLC_UP, 1);
                setState(SWLAN_STA_IDLE);
            }
            locked([&] { status_.power = powerOn; });
        }
        if (doLeave) leave();
        if (scanOverdue) abortScan();
        if (scanFinished || scanOverdue) finishScan();
        if (!powerOn) continue;
        if (doJoin) {
            join(params);
            memset(&params, 0, sizeof(params));
            continue;
        }
        if (scan) startScan(scanSsidLength ? scanSsid : nullptr, scanSsidLength);

        // While joined: link loss and group rekeys.
        uint32_t state = SWLAN_STA_OFF;
        locked([&] { state = status_.state; });
        FwEvent e;
        while (popEvent(e)) {
            if (state != SWLAN_STA_UP) continue;
            const bool down = (e.type == E_LINK && !(e.flags & kLinkUp)) || e.type == E_DEAUTH ||
                              e.type == E_DEAUTH_IND || e.type == E_DISASSOC || e.type == E_DISASSOC_IND;
            if (down) {
                linkDown(e);
                state = SWLAN_STA_IDLE;
            }
        }
        EapolFrame f;
        while (popEapol(f)) {
            if (state == SWLAN_STA_UP) handleEapol(f.frame, f.length);
        }
        if (state == SWLAN_STA_UP && swlan::nowNs() >= nextStatsNs_) {
            nextStatsNs_ = swlan::nowNs() + kStatsMs * 1000000ULL;
            logStats();
        }
        if (state == SWLAN_STA_UP && swlan::nowNs() >= nextRssiNs_) {
            nextRssiNs_ = swlan::nowNs() + kRssiRefreshMs * 1000000ULL;
            uint8_t scb[12] = {}, answer[4] = {};
            size_t n = 0;
            if (ioctl(WLC_GET_RSSI, scb, sizeof(scb), sizeof(answer), answer, &n) && n >= 4) {
                const int32_t rssi = static_cast<int32_t>(swlan::le32(answer));
                locked([&] {
                    status_.signal_dbm = current_->signal_dbm =
                        static_cast<int8_t>(rssi < -128 ? -128 : (rssi > 0 ? 0 : rssi));
                });
            }
        }
    }
}

// While joined, every 5 s that saw traffic: what the data path moved and lost in that window. Received: frames from
// the firmware, those passed to en0, dropped for want of an mbuf, refused by the stack's input. Sent: frames posted,
// their completions, posts refused (flow ring closed, all 32 buffers in flight, ring full), if_output drops (too
// large, or refused here). Plus the MSIs taken and the signal.
void SwitchWLAN::snapshot(Counts &c)
{
    locked([&] {
        c.rx = mb_.framesReceived();
        c.tx = mb_.framesSent();
        c.done = mb_.sendsCompleted();
        c.refused = mb_.sendsRefused();
        c.msis = interrupts_;
    });
    switch_wlan_net_counters(&c.net);
}

void SwitchWLAN::logStats()
{
    Counts now;
    snapshot(now);
    const Counts &was = lastCounts_;
    if (now.rx == was.rx && now.tx == was.tx && now.refused == was.refused &&
        now.net.tx_oversize == was.net.tx_oversize) {
        lastCounts_ = now;
        return;
    }
    int signal = 0;
    locked([&] { signal = status_.signal_dbm; });
    char out[384];  // periodic: to the log only (os_log), never the boot console
    snprintf(out, sizeof(out),
             "SwitchWLAN: stats %u ms: rx %u (en0 %u, no mbuf %u, input refused %u), tx %u done %u refused %u, "
             "if_output dropped %u oversize %u, %llu MSIs, %d dBm",
             kStatsMs, now.rx - was.rx, now.net.rx_input - was.net.rx_input, now.net.rx_no_mbuf - was.net.rx_no_mbuf,
             now.net.rx_input_errors - was.net.rx_input_errors, now.tx - was.tx, now.done - was.done,
             now.refused - was.refused, now.net.tx_refused - was.net.tx_refused,
             now.net.tx_oversize - was.net.tx_oversize, static_cast<unsigned long long>(now.msis - was.msis), signal);
    os_log(OS_LOG_DEFAULT, "%s", out);
    lastCounts_ = now;
}

// ---- the station interface ---------------------------------------------------------------------------------------

void SwitchWLAN::setEvents(const swlan_events *events)
{
    locked([&] { events_ = events; });
}

int SwitchWLAN::requestScan(const uint8_t *ssid, size_t length)
{
    int result = 0;
    locked([&] {
        if (!up_ || !powerOn_) {
            result = -1;
            return;
        }
        scanRequested_ = true;
        scanSsidLength_ = ssid && length <= 32 ? length : 0;
        if (scanSsidLength_) memcpy(scanSsid_, ssid, scanSsidLength_);
        kick();
    });
    return result;
}

int SwitchWLAN::bssCount()
{
    int n = 0;
    locked([&] { n = static_cast<int>(bssCount_); });
    return n;
}

int SwitchWLAN::bssGet(int index, swlan_bss *out)
{
    int result = -1;
    locked([&] {
        if (index < 0 || static_cast<unsigned>(index) >= bssCount_) return;
        *out = bss_[index];
        out->age_ms = static_cast<uint32_t>((swlan::nowNs() - bssSeenNs_[index]) / 1000000);
        result = 0;
    });
    return result;
}

int SwitchWLAN::currentBss(swlan_bss *out)
{
    int result = -1;
    locked([&] {
        if (status_.state < SWLAN_STA_ASSOC) return;
        *out = *current_;
        out->age_ms = 0;
        result = 0;
    });
    return result;
}

int SwitchWLAN::requestJoin(const uint8_t *ssid, size_t ssidLength, const uint8_t *bssid, const uint8_t *key,
                            size_t keyLength, bool keyIsPmk)
{
    if (!ssid || !ssidLength || ssidLength > 32 || keyLength > 64) return -1;
    int result = 0;
    locked([&] {
        if (!up_ || !powerOn_) {
            result = -1;
            return;
        }
        JoinParams &p = join_;
        memset(&p, 0, sizeof(p));
        memcpy(p.ssid, ssid, ssidLength);
        p.ssidLength = ssidLength;
        p.hasBssid = bssid && (bssid[0] | bssid[1] | bssid[2] | bssid[3] | bssid[4] | bssid[5]);
        if (p.hasBssid) memcpy(p.bssid, bssid, 6);
        p.hasKey = key && keyLength;
        if (p.hasKey) memcpy(p.key, key, keyLength);
        p.keyLength = p.hasKey ? keyLength : 0;
        p.keyIsPmk = keyIsPmk;
        joinRequested_ = true;
        leaveRequested_ = false;
        status_.result = SWLAN_RES_FAILED;
        kick();
    });
    return result;
}

void SwitchWLAN::requestLeave()
{
    locked([&] {
        leaveRequested_ = true;
        haveLast_ = false;  // no rejoin after a recovery either
        memset(&last_, 0, sizeof(last_));
        kick();
    });
}

int SwitchWLAN::requestPower(bool on)
{
    locked([&] {
        powerOn_ = on;
        powerRequested_ = true;
        if (!on) {
            haveLast_ = false;
            memset(&last_, 0, sizeof(last_));
        }
        kick();
    });
    return 0;
}

// en0's output: straight onto the flow ring under the gate (if_output runs in thread context; the gate is the work
// loop's recursive lock, so a frame sent from the work loop's own thread is fine too).
int SwitchWLAN::transmit(const uint8_t *frame, size_t length)
{
    uint32_t id = 0;
    locked([&] {
        if (up_ && status_.state == SWLAN_STA_UP) id = mb_.send(frame, length);
    });
    return id ? 0 : -1;
}

void SwitchWLAN::status(swlan_status *out)
{
    locked([&] {
        *out = status_;
        memcpy(out->mac, mac_, 6);
    });
}

void SwitchWLAN::counters(char *text, size_t size)
{
    locked([&] {
        snprintf(text, size,
                 "rx %u (to en0 %u), tx %u done %u refused %u, eapol dropped %u, MSIs %llu, losses %u "
                 "recovered %u%s",
                 mb_.framesReceived(), rxToStack_, mb_.framesSent(), mb_.sendsCompleted(), mb_.sendsRefused(),
                 eapolDropped_, static_cast<unsigned long long>(interrupts_), losses_, recoveries_,
                 awdlOpen_ ? ", AWDL open" : "");
    });
}

// ---- the firmware's own AWDL (SwitchWLANAwdl.cpp) -----------------------------------------------------------------

// The events the firmware reports (brcmf_fweh "event_msgs": one bit per event), with the AWDL ones while AWDL is open.
bool SwitchWLAN::setEventMask(bool awdl)
{
    uint8_t mask[16] = {};
    for (uint32_t e : kEventsWanted) mask[e / 8] |= static_cast<uint8_t>(1u << (e % 8));
    if (awdl)
        for (uint32_t e : kAwdlEvents) mask[e / 8] |= static_cast<uint8_t>(1u << (e % 8));
    return setVar("event_msgs", mask, sizeof(mask));
}

// Work loop, gate held: an AWDL event to the stack. WLC_E_IF names the AWDL interface's index (its frames and flow
// rings) or takes it away.
void SwitchWLAN::awdlEvent(const swlan::Event &event)
{
    ++awdlEventsIn_;
    if (event.type == E_IF && event.length >= 5) {
        const uint8_t ifidx = event.data[0], opcode = event.data[1];
        if (opcode == 1)
            awdlIfidx_ = ifidx;
        else if (opcode == 2 && static_cast<int>(ifidx) == awdlIfidx_) {
            awdlIfidx_ = -1;
            mb_.awdlClose();
        }
        IOLog("SwitchWLAN: AWDL: firmware interface %u %s (bsscfg %u, role %u)\n", ifidx,
              opcode == 1   ? "added"
              : opcode == 2 ? "removed"
                            : "changed",
              event.data[3], event.data[4]);
    }
    if (!awdlEventFn_) return;
    swlan_fw_event e = {event.type, event.status, event.reason, {}, event.ifidx, event.bsscfgidx};
    memcpy(e.addr, event.addr, 6);
    awdlEventFn_(awdlEventCtx_, &e, event.data, event.length);
}

int SwitchWLAN::awdlOpen(const swlan_awdl_caps **caps)
{
    if (loop_->onThread()) return -EDEADLK;
    bool up = false, open = false;
    locked([&] {
        up = up_;
        open = awdlOpen_;
    });
    static swlan_awdl_caps c;
    if (!up) {
        IOLog("SwitchWLAN: AWDL: not opened: the radio is not up\n");
        return -ENXIO;
    }
    if (!capAwdl_) {
        IOLog("SwitchWLAN: AWDL: not opened: the firmware's capabilities do not list awdl\n");
        return -ENODEV;
    }
    if (!mb_.awdlAvailable()) {
        IOLog("SwitchWLAN: AWDL: not opened: no flow rings for an AWDL interface (DMA area or firmware limit)\n");
        return -ENOSPC;
    }
    if (!open) {
        locked([&] { awdlOpen_ = true; });
        if (!setEventMask(true)) {
            IOLog("SwitchWLAN: AWDL: not opened: the AWDL events were refused: %s\n", why_);
            locked([&] { awdlOpen_ = false; });
            return -EIO;
        }
        IOLog("SwitchWLAN: AWDL: firmware AWDL opened (AWDL events on, %u AWDL flow rings)\n", mb_.awdlRingCount());
    }
    memset(&c, 0, sizeof(c));
    c.abi_version = SWLAN_AWDL_ABI_VERSION;
    c.has_native_awdl = 1;
    memcpy(c.self_mac, mac_, 6);
    *caps = &c;
    return 0;
}

void SwitchWLAN::awdlClose()
{
    bool was = false;
    locked([&] {
        was = awdlOpen_;
        awdlOpen_ = false;
        awdlIfidx_ = -1;
        if (up_) mb_.awdlClose();
    });
    if (was && !loop_->onThread()) setEventMask(false);
}

// "<name>\0<value>", or brcmf_fil_bsscfg_data_set's "bsscfg:<name>\0<s32 index><value>".
int SwitchWLAN::awdlVar(bool set, int32_t bsscfg, const char *name, const void *in, size_t inLength, void *out,
                        size_t outLength, size_t *returned)
{
    if (loop_->onThread()) return -EDEADLK;
    uint8_t request[1024];
    size_t n = 0;
    const size_t nameLength = strlen(name) + 1;
    if (7 + nameLength + 4 + inLength > sizeof(request)) return -EMSGSIZE;
    if (bsscfg >= 0) {
        memcpy(request, "bsscfg:", 7);
        n = 7;
    }
    memcpy(request + n, name, nameLength);
    n += nameLength;
    if (bsscfg >= 0) {
        swlan::put32(request + n, static_cast<uint32_t>(bsscfg));
        n += 4;
    }
    if (inLength) memcpy(request + n, in, inLength);
    n += inLength;
    const bool ok = set ? ioctl(WLC_SET_VAR, request, n, 0, nullptr, nullptr)
                        : ioctl(WLC_GET_VAR, request, n, outLength, static_cast<uint8_t *>(out), returned);
    if (!ok) {
        static unsigned said;
        if (said++ < 16) IOLog("SwitchWLAN: AWDL: %s %s: %s\n", set ? "set" : "get", name, why_);
        return -EIO;
    }
    return 0;
}

int SwitchWLAN::awdlTransmit(const uint8_t *eth, size_t length)
{
    int r = -1;
    locked([&] {
        if (up_ && awdlOpen_ && awdlIfidx_ >= 0) r = mb_.awdlSend(static_cast<uint8_t>(awdlIfidx_), eth, length);
    });
    return r < 0 ? -1 : 0;
}

// Only stored (any thread, no gate): pointer-sized stores, the context before its function.
void SwitchWLAN::awdlCallbacks(swlan_fw_event_fn events, void *eventsContext, swlan_eth_rx_fn rx, void *rxContext,
                               swlan_evt_fn loss, void *lossContext)
{
    if (events) {
        __atomic_store_n(&awdlEventCtx_, eventsContext, __ATOMIC_RELEASE);
        __atomic_store_n(&awdlEventFn_, events, __ATOMIC_RELEASE);
    }
    if (rx) {
        __atomic_store_n(&awdlRxCtx_, rxContext, __ATOMIC_RELEASE);
        __atomic_store_n(&awdlRxFn_, rx, __ATOMIC_RELEASE);
    }
    if (loss) {
        __atomic_store_n(&awdlLossCtx_, lossContext, __ATOMIC_RELEASE);
        __atomic_store_n(&awdlLossFn_, loss, __ATOMIC_RELEASE);
    }
}

// ---- C entry points ------------------------------------------------------------------------------------------------

extern "C" void switch_wlan_set_events(const struct swlan_events *events)
{
    if (gDriver) gDriver->setEvents(events);
}
extern "C" int switch_wlan_scan(const uint8_t *ssid, size_t ssid_len)
{
    return gDriver ? gDriver->requestScan(ssid, ssid_len) : -1;
}
extern "C" int switch_wlan_bss_count(void)
{
    return gDriver ? gDriver->bssCount() : 0;
}
extern "C" int switch_wlan_bss_get(int index, struct swlan_bss *out)
{
    return gDriver ? gDriver->bssGet(index, out) : -1;
}
extern "C" int switch_wlan_current_bss(struct swlan_bss *out)
{
    return gDriver ? gDriver->currentBss(out) : -1;
}
extern "C" int switch_wlan_connect(const uint8_t *ssid, size_t ssid_len, const uint8_t *bssid, const uint8_t *key,
                                   size_t key_len, int key_is_pmk)
{
    return gDriver ? gDriver->requestJoin(ssid, ssid_len, bssid, key, key_len, key_is_pmk != 0) : -1;
}
extern "C" void switch_wlan_disconnect(void)
{
    if (gDriver) gDriver->requestLeave();
}
extern "C" int switch_wlan_power(int on)
{
    return gDriver ? gDriver->requestPower(on != 0) : -1;
}
extern "C" int switch_wlan_tx(const uint8_t *eth, size_t length)
{
    return gDriver ? gDriver->transmit(eth, length) : -1;
}
extern "C" void switch_wlan_status_get(struct swlan_status *out)
{
    if (gDriver)
        gDriver->status(out);
    else
        memset(out, 0, sizeof(*out));
}
extern "C" void switch_wlan_counters(char *text, size_t size)
{
    if (gDriver)
        gDriver->counters(text, size);
    else if (size)
        text[0] = 0;
}

// SwitchWLANAwdlRadio.h §6, the firmware's own AWDL. The monitor+inject calls (§2-§5) keep SwitchWLANAwdl.cpp's
// weak "unavailable" definitions: this firmware has no monitor or injection.
extern "C" int swlan_awdl_open(const struct swlan_awdl_caps **caps_out)
{
    if (caps_out) *caps_out = nullptr;
    return gDriver ? gDriver->awdlOpen(caps_out) : -ENXIO;
}
extern "C" void swlan_awdl_close(void)
{
    if (gDriver) gDriver->awdlClose();
}
extern "C" int swlan_awdl_iovar_set(int32_t bsscfg, const char *name, const void *buf, size_t len)
{
    return gDriver ? gDriver->awdlVar(true, bsscfg, name, buf, len, nullptr, 0, nullptr) : -ENXIO;
}
extern "C" int swlan_awdl_iovar_get(int32_t bsscfg, const char *name, void *buf, size_t len, size_t *returned)
{
    return gDriver ? gDriver->awdlVar(false, bsscfg, name, nullptr, 0, buf, len, returned) : -ENXIO;
}
extern "C" int swlan_awdl_fw_events(swlan_fw_event_fn fn, void *ctx)
{
    if (!gDriver || !fn) return -ENXIO;
    gDriver->awdlCallbacks(fn, ctx, nullptr, nullptr, nullptr, nullptr);
    return 0;
}
extern "C" int swlan_awdl_eth_rx(swlan_eth_rx_fn fn, void *ctx)
{
    if (!gDriver || !fn) return -ENXIO;
    gDriver->awdlCallbacks(nullptr, nullptr, fn, ctx, nullptr, nullptr);
    return 0;
}
extern "C" int swlan_awdl_events(swlan_evt_fn fn, void *ctx)
{
    if (!gDriver || !fn) return -ENXIO;
    gDriver->awdlCallbacks(nullptr, nullptr, nullptr, nullptr, fn, ctx);
    return 0;
}
extern "C" int swlan_awdl_eth_tx(const uint8_t *eth, size_t len)
{
    return gDriver ? gDriver->awdlTransmit(eth, len) : -ENXIO;
}

// The radio's address where the system reads it, before userland starts: /chosen mac-address-wifi0 (MobileGestalt's
// WifiAddress; wifid derives every network's private address from it and aborts a join without it, UniXNU-sheng
// WifiProbe.cpp) and /arm-io/wlan local-mac-address (as J120's device tree has it). The packager's placeholders are
// replaced with the fuse address the chip is given.
static void publishMac(const uint8_t mac[6])
{
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        chosen->setProperty("mac-address-wifi0", const_cast<uint8_t *>(mac), 6);
        chosen->release();
    }
    if (IORegistryEntry *wlan = IORegistryEntry::fromPath("/arm-io/wlan", gIODTPlane)) {
        wlan->setProperty("local-mac-address", const_cast<uint8_t *>(mac), 6);
        wlan->release();
    }
}

// Called once from the kernel's boot path (IOSwitchRootDiag.inc, after the SD block device): the address in the device
// tree, then the driver on IOResources if the boot-arg asks for it. Returns 0 when off or started. (The bring-up
// register report, switch_wlan_probe, is no longer printed at boot.)
extern "C" int switch_wlan_publish(void)
{
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("switch-wlan", &enabled, sizeof(enabled)) || enabled != 1) return 0;
    if (gDriver) return 0;
    uint8_t mac[6];
    if (switch_wlan_mac(mac, nullptr)) {
        // The fuses were not readable (switch_wlan_mac needs the fuse clock and FUSE_VISIBLE): a locally administered
        // address, as nxk uses, rather than the NVRAM's placeholder shared by every console.
        static const uint8_t fallback[6] = {0x02, 0x4E, 0x58, 0x4B, 0x57, 0x01};
        memcpy(mac, fallback, sizeof(mac));
        IOLog("SwitchWLAN: fuses not readable, using the locally administered address 02:4e:58:4b:57:01\n");
    }
    publishMac(mac);
    // Without a platform UUID (made from /chosen unique-chip-id and chip-id) gethostuuid() waits for ever, and configd's
    // IPConfiguration start() calls it: configd never lists en0 and wifid never makes a Wi-Fi device.
    IOService *platform = IOService::getPlatform();
    if (!platform || !platform->getProvider() || !platform->getProvider()->getProperty(kIOPlatformUUIDKey))
        IOLog("SwitchWLAN: no IOPlatformUUID (ADT /chosen unique-chip-id, chip-id): configd will stall, no Wi-Fi in "
              "Settings\n");
    IOService *resources = IOService::getResourceService();
    SwitchWLAN *candidate = OSTypeAlloc(SwitchWLAN);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        IOLog("SwitchWLAN: attach failed\n");
        OSSafeReleaseNULL(candidate);
        return -1;
    }
    candidate->setMac(mac);
    gDriver = candidate;
    if (!candidate->start(resources)) {
        gDriver = nullptr;
        candidate->detach(resources);
        candidate->release();
        return -1;
    }
    return 0;
}
