// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDPOut: DisplayPort over the USB-C port (dock, USB-C DP adapters), Tegra DPAUX1 + SOR1 + DC B.
//
// iOS side. iPadOS 17 on iPad7,1 has one IOMobileFramebuffer (AppleCLCD, disp0,t8011, whose own eDP
// link is AppleT8011DPTXController/IODisplayPortFamily) and no second display pipe: an external screen
// on this iPad is a mirror, made by the Lightning Digital AV Adapter from an H.264 stream (AppleAVE2) or
// by AirPlay; QuartzCore's wired-display path (IOMobileFramebufferEnableHotPlugDetectNotifications,
// DigitalOut modes, +[CADisplay TVOutDisplay]) needs an IOMFB service with a second CLCD that this
// kernelcache never creates. So the Switch mirrors in hardware, which is what iPadOS itself would show:
// display controller B scans out the same landscape surface display controller A rotates onto the
// panel (NativeScanout.h, boot-arg ioswitch-hw-rotate: 1280x720 block-linear at the framebuffer carve-
// out), scaled to the sink's mode, through SOR1 in DisplayPort mode. iOS sees nothing new; nothing in
// NativeLegacyDisplay changes.
//
// Hardware (OLED DTB host1x/dc@54240000 "head1", sor1, dpaux1): DC B 0x54240000 (SPI 74), SOR1 0x54580000
// (SPI 76), DPAUX1 0x54040000 (SPI 11 = INTID 43), HPD on pad hdmi_int_dp_hpd_pcc1 in function "dp"
// (DPAUX1 sees it directly: DP_AUXSTAT.HPD_STATUS and PLUG/UNPLUG/IRQ interrupts), 4 lanes, lane xbar
// 2 1 0 3 4. The BM92T36 enters the alt mode and switches the PI3USB30532 mux (SwitchPD.cpp:
// DP_CFG_AND_START_HPD), then reports switch_dp_alt_mode(true, lanes); HPD itself comes from the sink.
//
// This file: DPAUX1 bring-up (clock, pad in AUX mode with the DT prod value, powered), the HPD and AUX
// interrupts, AUX native and I2C-over-AUX transactions completed by TX_DONE (L4T dpaux.c polls
// TRANSACTREQ; the interrupt exists and is used here), sink probing (DPCD receiver capabilities, EDID
// preferred mode). The SOR1 link (PLLDP, pad power-up, lane sequencer, link training) and DC B
// (raster from the EDID mode, window A on the shared surface, SOR attach) follow L4T sor.c/dp.c/
// dp_lt.c and are the next step: start() reports the sink and the mode it would drive.
#include "SwitchUSBC.h"
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include "../ArmGIC/IOSwitchInterrupts.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

namespace {
constexpr uint64_t kCarPA = 0x60006000, kDpauxPA = 0x54040000;
constexpr uint32_t kDpauxIntid = 32 + 11;
enum : uint32_t {  // CAR: DPAUX1 is clock 207 (bank Y bit 15), SOR_SAFE 222 (Y bit 30), L4T clk-tegra210.c ids
    // RST_DEV_Y_SET/CLR are 0x2a8/0x2ac; 0x2a4 is RST_DEVICES_Y itself (TegraPCIe.cpp, SwitchAudio.cpp).
    CLK_ENB_Y_SET = 0x29c,
    RST_DEV_Y_SET = 0x2a8,
    RST_DEV_Y_CLR = 0x2ac,
    Y_DPAUX1 = 1u << 15,
    Y_SOR_SAFE = 1u << 30,
};
enum : uint32_t {  // DPAUX word indices (L4T dpaux_regs.h)
    INTR_EN_AUX = 0x1,
    INTR_AUX = 0x5,
    AUXDATA_WRITE = 0x9,
    AUXDATA_READ = 0x19,
    AUXADDR = 0x29,
    AUXCTL = 0x2d,
    AUXSTAT = 0x31,
    HPD_CONFIG = 0x3d,
    HPD_IRQ_CONFIG = 0x41,
    HYBRID_PADCTL = 0x49,
    HYBRID_SPARE = 0x4d,
    INTR_PLUG = 1u << 0,
    INTR_UNPLUG = 1u << 1,
    INTR_IRQ = 1u << 2,
    INTR_TX_DONE = 1u << 3,
    AUXCTL_ADDRESS_ONLY = 1u << 8,
    AUXCTL_TRANSACTREQ = 1u << 16,
    AUXCTL_RST = 1u << 31,
    CMD_I2CWR = 0u << 12,
    CMD_I2CRD = 1u << 12,
    CMD_MOTWR = 4u << 12,
    CMD_MOTRD = 5u << 12,
    CMD_AUXWR = 8u << 12,
    CMD_AUXRD = 9u << 12,
    AUXSTAT_HPD = 1u << 28,
    AUXSTAT_REPLY_MASK = 0xfu << 16,
    AUXSTAT_DEFER = 2u << 16,
    AUXSTAT_I2CDEFER = 8u << 16,
    AUXSTAT_ERRORS = 0xfu << 8,
    PADCTL_MODE_I2C = 1u,
    PADCTL_I2C_RCV = 3u << 14,
};
constexpr uint32_t kPadProdMask = 0x37fe, kPadProdValue = 0x23a2;  // DT dpaux1 prod_c_dpaux_dp (reg 0x124 = word 0x49)
constexpr unsigned kAuxRetries = 7;
constexpr uint64_t kAuxTimeoutNs = 10ull * 1000 * 1000;

struct Mode {
    unsigned pixelKHz, width, height, hblank, vblank, hsyncOffset, hsyncWidth, vsyncOffset, vsyncWidth;
};
}

class SwitchDPOut : public IOService {
    OSDeclareDefaultStructors(SwitchDPOut) volatile uint8_t *car = nullptr, *dpaux = nullptr;
    IOWorkLoop *loop = nullptr;
    IOFilterInterruptEventSource *interrupt = nullptr;
    volatile uint32_t events = 0;  // INTR_AUX bits latched by the filter for the action
    volatile uint32_t txDone = 0;  // TX_DONE count (the AUX waiter's wake-up)
    bool altMode = false, plugged = false;
    unsigned lanes = 0;

    uint32_t rd(uint32_t word) const { return *reinterpret_cast<volatile uint32_t *>(dpaux + word * 4); }
    void wr(uint32_t word, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(dpaux + word * 4) = v; }
    int aux(uint32_t cmd, uint32_t address, uint8_t *data, unsigned length);
    bool readEdid(uint8_t edid[128]);
    void probeSink();

    static bool filter(OSObject *owner, IOFilterInterruptEventSource *);
    static void action(OSObject *owner, IOInterruptEventSource *, int);

public:
    bool begin();
    void altModeChanged(bool entered, unsigned laneCount);
    IOWorkLoop *dpLoop() const { return loop; }
};
OSDefineMetaClassAndStructors(SwitchDPOut, IOService)

    namespace
{
    SwitchDPOut *gDp;
}

// Primary interrupt context: acknowledge everything (W1C), wake an AUX waiter on TX_DONE, hand the HPD
// events to the work loop.
bool SwitchDPOut::filter(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<SwitchDPOut *>(owner);
    const uint32_t pending = self->rd(INTR_AUX);
    if (!pending) return false;
    self->wr(INTR_AUX, pending);
    if (pending & INTR_TX_DONE) {
        __atomic_fetch_add(&self->txDone, 1, __ATOMIC_RELEASE);
        thread_wakeup((event_t)&self->txDone);
    }
    if (pending & (INTR_PLUG | INTR_UNPLUG | INTR_IRQ)) {
        __atomic_fetch_or(&self->events, pending & (INTR_PLUG | INTR_UNPLUG | INTR_IRQ), __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

void SwitchDPOut::action(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchDPOut *>(owner);
    const uint32_t pending = __atomic_exchange_n(&self->events, 0, __ATOMIC_ACQ_REL);
    const bool now = self->rd(AUXSTAT) & AUXSTAT_HPD;
    if ((pending & INTR_UNPLUG) && !now && self->plugged) {
        self->plugged = false;
        IOLog("SwitchDPOut: sink unplugged (HPD low)\n");
    }
    if ((pending & (INTR_PLUG | INTR_IRQ)) && now) {
        const bool fresh = !self->plugged;
        self->plugged = true;
        if (fresh && self->altMode) self->probeSink();
    }
}

// One AUX transaction of 0-16 bytes (tegra_dp_aux_tx_config + read/write_chunk_locked: retries on
// timeout/receive errors and on DEFER), completed by the TX_DONE interrupt. A transaction that ends with
// TX_DONE pending but no interrupt taken means INTID 43 is not reaching the CPU: panic.
int SwitchDPOut::aux(uint32_t cmd, uint32_t address, uint8_t *data, unsigned length)
{
    const bool read = cmd == CMD_AUXRD || cmd == CMD_I2CRD || cmd == CMD_MOTRD;
    for (unsigned attempt = 0; attempt <= kAuxRetries; ++attempt) {
        if (!(rd(AUXSTAT) & AUXSTAT_HPD)) return -1;
        uint32_t ctl = (rd(AUXCTL) & ~(0xffu | AUXCTL_ADDRESS_ONLY | 0xfu << 12)) | cmd | (length ? length - 1 : 0) |
                       (length ? 0 : AUXCTL_ADDRESS_ONLY);
        wr(AUXCTL, ctl);
        wr(AUXADDR, address);
        if (!read && length) {
            uint32_t words[4] = {};
            memcpy(words, data, length);
            for (unsigned i = 0; i < 4; ++i) wr(AUXDATA_WRITE + 4 * i, words[i]);
        }
        const uint32_t before = __atomic_load_n(&txDone, __ATOMIC_ACQUIRE);
        uint64_t deadline;
        nanoseconds_to_absolutetime(kAuxTimeoutNs, &deadline);
        deadline += mach_absolute_time();
        wr(AUXCTL, ctl | AUXCTL_TRANSACTREQ);
        while (__atomic_load_n(&txDone, __ATOMIC_ACQUIRE) == before) {
            assert_wait_deadline((event_t)&txDone, THREAD_UNINT, deadline);
            if (__atomic_load_n(&txDone, __ATOMIC_ACQUIRE) != before) {
                clear_wait(current_thread(), THREAD_AWAKENED);
                break;
            }
            if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT) break;
        }
        if (__atomic_load_n(&txDone, __ATOMIC_ACQUIRE) == before) {
            if ((rd(INTR_AUX) & INTR_TX_DONE) && (rd(INTR_EN_AUX) & INTR_TX_DONE)) {
                char gic[192];
                ioswitch_interrupt_describe(kDpauxIntid, gic, sizeof(gic));
                panic("SwitchDPOut: DPAUX1 interrupt lost (INTID %u): INTR_AUX %#x AUXCTL %#x AUXSTAT %#x, %s",
                      kDpauxIntid, rd(INTR_AUX), rd(AUXCTL), rd(AUXSTAT), gic);
            }
            continue;  // no reply from the sink: retried as an AUX timeout
        }
        const uint32_t status = rd(AUXSTAT);
        if (status & AUXSTAT_ERRORS) {
            wr(AUXSTAT, status);
            continue;
        }
        const uint32_t reply = status & AUXSTAT_REPLY_MASK;
        if (reply == AUXSTAT_DEFER || reply == AUXSTAT_I2CDEFER) {
            wr(AUXSTAT, status);
            IODelay(400);
            continue;
        }
        if (reply != 0) return -1;  // NACK
        if (read && length) {
            uint32_t words[4];
            for (unsigned i = 0; i < 4; ++i) words[i] = rd(AUXDATA_READ + 4 * i);
            const unsigned got = status & 0xff;
            memcpy(data, words, got < length ? got : length);
            return (int)(got < length ? got : length);
        }
        return (int)length;
    }
    return -1;
}

// E-DDC over I2C-over-AUX: offset 0 written with MOT, 8 MOT reads of 16 bytes, then the stop.
bool SwitchDPOut::readEdid(uint8_t edid[128])
{
    uint8_t offset = 0;
    if (aux(CMD_MOTWR, 0x50, &offset, 1) != 1) return false;
    for (unsigned at = 0; at < 128;) {
        int got = aux(CMD_MOTRD, 0x50, edid + at, 16);
        if (got <= 0) return false;
        at += (unsigned)got;
    }
    aux(CMD_I2CRD, 0x50, nullptr, 0);
    uint8_t sum = 0;
    for (unsigned i = 0; i < 128; ++i) sum += edid[i];
    return sum == 0 && edid[0] == 0x00 && edid[1] == 0xff;
}

void SwitchDPOut::probeSink()
{
    uint8_t caps[16] = {};
    if (aux(CMD_AUXRD, 0x000, caps, 16) < 3) {
        IOLog("SwitchDPOut: DPCD read failed\n");
        return;
    }
    uint8_t edid[128] = {};
    Mode mode = {};
    if (readEdid(edid)) {
        const uint8_t *d = edid + 54;  // first detailed timing descriptor: the preferred mode
        mode.pixelKHz = (unsigned)(d[0] | d[1] << 8) * 10;
        mode.width = d[2] | (d[4] & 0xf0) << 4;
        mode.hblank = d[3] | (d[4] & 0x0f) << 8;
        mode.height = d[5] | (d[7] & 0xf0) << 4;
        mode.vblank = d[6] | (d[7] & 0x0f) << 8;
        mode.hsyncOffset = d[8] | (d[11] & 0xc0) << 2;
        mode.hsyncWidth = d[9] | (d[11] & 0x30) << 4;
        mode.vsyncOffset = (d[10] >> 4) | (d[11] & 0x0c) << 2;
        mode.vsyncWidth = (d[10] & 0x0f) | (d[11] & 0x03) << 4;
    }
    const unsigned rate = caps[1] == 0x14 ? 540 : caps[1] == 0x0a ? 270 : 162;
    const unsigned refresh = mode.pixelKHz && mode.width
                                 ? (unsigned)((uint64_t)mode.pixelKHz * 1000 /
                                              ((mode.width + mode.hblank) * (uint64_t)(mode.height + mode.vblank)))
                                 : 0;
    IOLog("SwitchDPOut: sink DPCD %u.%u, %u lane(s) at %u.%u Gb/s (alt mode %u lanes), preferred %ux%u@%u (%u kHz)\n",
          caps[0] >> 4, caps[0] & 0xf, caps[2] & 0x1f, rate / 100, rate % 100 / 10, lanes, mode.width, mode.height,
          refresh, mode.pixelKHz);
    // Next (L4T dp.c tegra_dc_dp_enable): PLLDP for the link rate, SOR1 pad power-up and lane sequencer
    // (sor.c tegra_sor_dp_pad_power_up, tegra_sor_power_lanes), link training (dp_lt.c: TPS1 clock
    // recovery, TPS2/3 equalization with the DT lt-data drive tables), DC B raster from `mode`, window A
    // on the hw-rotate surface scaled by DDA, tegra_dc_sor_attach. Not run yet: nothing is driven.
}

void SwitchDPOut::altModeChanged(bool entered, unsigned laneCount)
{
    altMode = entered;
    lanes = laneCount;
    if (!entered) {
        IOLog("SwitchDPOut: alt mode left\n");
        return;
    }
    if (rd(AUXSTAT) & AUXSTAT_HPD) {
        plugged = true;
        probeSink();
    }
}

// tegra_dpaux_init_data + tegra_dpaux_config_pad_mode(AUX) + the HPD interrupt set.
bool SwitchDPOut::begin()
{
    car = (volatile uint8_t *)ml_io_map(kCarPA, 0x1000);
    dpaux = (volatile uint8_t *)ml_io_map(kDpauxPA, 0x1000);
    loop = IOWorkLoop::workLoop();
    if (!car || !dpaux || !loop) {
        IOLog("SwitchDPOut: setup failed\n");
        return false;
    }
    auto cw = [&](uint32_t o, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(car + o) = v; };
    cw(RST_DEV_Y_SET, Y_DPAUX1);
    cw(CLK_ENB_Y_SET, Y_DPAUX1 | Y_SOR_SAFE);
    IODelay(5);
    cw(RST_DEV_Y_CLR, Y_DPAUX1);
    IODelay(5);
    wr(HYBRID_PADCTL, (rd(HYBRID_PADCTL) & ~kPadProdMask) | kPadProdValue);
    wr(HYBRID_PADCTL, rd(HYBRID_PADCTL) & ~(PADCTL_I2C_RCV | PADCTL_MODE_I2C));  // AUX mode before power-up
    wr(HYBRID_SPARE, 0);                                                         // PAD_PWR up
    wr(INTR_AUX, rd(INTR_AUX));
    wr(INTR_EN_AUX, INTR_PLUG | INTR_UNPLUG | INTR_IRQ | INTR_TX_DONE);

    IOService *nub = ioswitch_interrupt_nub(kDpauxIntid, false);
    interrupt = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchDPOut::action,
                                                                         &SwitchDPOut::filter, nub, 0);
    if (!interrupt || loop->addEventSource(interrupt) != kIOReturnSuccess) {
        panic("SwitchDPOut: cannot attach the DPAUX1 interrupt (INTID %u)", kDpauxIntid);
    }
    interrupt->enable();
    plugged = rd(AUXSTAT) & AUXSTAT_HPD;
    IOLog("SwitchDPOut: started (HPD %s)\n", plugged ? "high" : "low");
    return true;
}

extern "C" int switch_dp_start(void)
{
    if (gDp) return 0;
    IOService *resources = IOService::getResourceService();
    SwitchDPOut *dp = OSTypeAlloc(SwitchDPOut);
    if (!resources || !dp || !dp->init() || !dp->attach(resources) || !dp->begin()) {
        OSSafeReleaseNULL(dp);
        return -1;
    }
    dp->setName("SwitchDPOut");
    __atomic_store_n(&gDp, dp, __ATOMIC_RELEASE);
    return 0;
}

// From SwitchPD's work loop; the AUX traffic runs on ours.
extern "C" void switch_dp_alt_mode(bool entered, unsigned lanes)
{
    SwitchDPOut *dp = __atomic_load_n(&gDp, __ATOMIC_ACQUIRE);
    if (!dp) return;
    dp->dpLoop()->runAction(
        [](OSObject *owner, void *entered, void *lanes, void *, void *) -> IOReturn {
            static_cast<SwitchDPOut *>(owner)->altModeChanged(entered != nullptr, (unsigned)(uintptr_t)lanes);
            return kIOReturnSuccess;
        },
        dp, entered ? (void *)1 : nullptr, (void *)(uintptr_t)lanes);
}
