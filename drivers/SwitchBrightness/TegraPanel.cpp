// SPDX-License-Identifier: BSD-2-Clause-Patent
// DCS commands to the Switch OLED panel while the display controller scans out; see TegraPanel.h.
// DC and DSI offsets are 32-bit word indices as in hekate bdk/display/di.h (DISPLAY_A(off) is 0x54200000 + off * 4,
// DSI(off) is 0x54300000 + off * 4); the packet sequence is hekate di.c display_dsi_vblank_write(), the panel commands
// are hekate's PANEL_SAM_AMS699VC01 paths and switchroot L4T's panel-s-720p-7-0.dtsi (init and suspend command lists).
//
// Timing. Hekate polls DC_CMD_INT_STATUS and makes every write of the sequence microseconds after a DC A FRAME_END. A
// thread that sleeps on frame ends cannot: the display's interrupt action runs under its work-loop gate, often many
// milliseconds late, and a frame end that latched before the enable could release the packet in the same frame. So the
// thread only hands the packet over and sleeps; the display calls tegra_panel_frame_end_filter() from its DC A interrupt
// filter (primary interrupt context) at every FRAME_END, and the filter plays one step per frame end:
//   1  DSI_VIDEO_MODE_CONTROL = CMD_PKT_VID_ENABLE | LINE_TYPE(4): host packets ride the video stream in a blanking line
//      (hekate _display_dsi_wait_vblank(true))
//   2  the packet words into DSI_WR_DATA, back to back, a frame end after the enable as in hekate
//   3  nothing: one frame more than hekate, so that a write that interrupt latency pushed past the blanking lines has
//      gone out by the reset as well
//   4  14 us (L4T tegra_dsi_wait_frame_end: v_ref_to_sync lines), the DSI syncpoint block soft reset, 300 us, clear,
//      300 us, DSI_VIDEO_MODE_CONTROL = 0 (hekate _display_dsi_wait_vblank(false), L4T tegra_dsi_syncpt_reset)
// DC_DISP_DISPLAY_DBG_TIMING (L4T dc_reg.h, its tegra_dc_get_v_count) shows whether the raster is still in vertical
// blanking at step 4; if interrupt latency let it leave, the step waits for the next frame end, at most twice, and then
// goes ahead (the first command that had to logs one line). switch-backlight-syncpt=0 leaves the syncpoint reset out
// of step 4, as L4T's video-mode path tegra_dsi_end_host_cmd_v_blank_video does.
// The OLED's 1000 x 1300 pixel frame at 78 MHz has 20 blanking lines of 12.8 us (256 us); hekate's step 4 lasts 614 us,
// so its reset is released and the video mode restored in the first active lines, here as in hekate. A command takes
// four frames (67 ms).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "SwitchBrightness.h"
#include "TegraPanel.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

namespace {
constexpr vm_offset_t kCarPA = 0x60006000, kDisplayAPA = 0x54200000, kDsiPA = 0x54300000;
constexpr vm_size_t kDcMapBytes = 0x2000;  // through DC_DISP_DISPLAY_DBG_TIMING
constexpr uint32_t kDisplayAInterrupt =
    105;  // GIC SPI 73 (hekate irq.h IRQ_DISPLAY, mainline tegra210.dtsi dc@54200000)

// Clock and reset controller, byte offsets (hekate bdk/soc/clock.h): DISP1 is L27, DSI is H16.
enum : uint32_t { RST_DEVICES_L = 0x004, RST_DEVICES_H = 0x008, CLK_OUT_ENB_L = 0x010, CLK_OUT_ENB_H = 0x014 };
constexpr uint32_t kDisp1 = 1u << 27, kDsiClock = 1u << 16;

// Display controller A, word indices. DISPLAY_COMMAND bits 6:5 are the controller mode, 0 = stopped.
enum : uint32_t {
    DC_CMD_DISPLAY_COMMAND = 0x32,
    DC_CMD_INT_STATUS = 0x37,
    DC_CMD_INT_MASK = 0x38,
    DC_CMD_INT_ENABLE = 0x39,
    DC_COM_SCRATCH_REGISTER_B = 0x326,
    DC_DISP_DISPLAY_DBG_TIMING = 0x4f6,
};
constexpr uint32_t kDisplayModeMask = 3u << 5, kFrameEndInt = 1u << 1;
constexpr uint32_t kDbgVBlank = 1u << 15;  // DBG_TIMING DBG_V_BLANK; V_COUNT is bits 14:0, H_COUNT bits 30:16

// DSI A, word indices.
enum : uint32_t {
    DSI_INCR_SYNCPT_CNTRL = 0x01,
    DSI_WR_DATA = 0x0a,
    DSI_POWER_CONTROL = 0x0b,
    DSI_CONTROL = 0x10,
    DSI_STATUS = 0x15,
    DSI_VIDEO_MODE_CONTROL = 0x4e,
};
constexpr uint32_t kSyncptSoftReset = 1u << 0, kDsiPowerEnable = 1u << 0, kDsiVideoEnable = 1u << 1;
constexpr uint32_t kCommandsInVideo = 1u << 0, kOnLine4 = 4u << 1;  // DSI_CMD_PKT_VID_ENABLE, DSI_DSI_LINE_TYPE(4)
constexpr uint32_t kLineBufferErrors = 3u << 8;  // DSI_STATUS LB_OVERFLOW (8), LB_UNDERFLOW (9), L4T dsi_regs.h

// MIPI DSI data types and DCS commands (hekate di.h, Linux include/video/mipi_display.h).
constexpr uint32_t kShortWrite = 0x05, kShortWriteParam = 0x15, kLongWrite = 0x39;
constexpr uint32_t kEnterSleep = 0x10, kExitSleep = 0x11, kDisplayOff = 0x28, kDisplayOn = 0x29;
constexpr uint32_t kSetBrightness = 0x51, kControlDisplay = 0x53;
constexpr uint32_t kBrightnessControl = 1u << 5, kDimmingControl = 1u << 3;  // 0x28, as hekate and L4T send it

constexpr unsigned kFrameTimeoutUS = 50000;  // per frame end: three frames at 60 Hz
constexpr unsigned kResetDeferrals = 2;      // frame ends step 4 may wait for vertical blanking

// The one command out, shared with the filter. The thread fills it and publishes step = kQueued; from then on only the
// filter writes it until it publishes kDone.
enum : uint32_t { kIdle, kQueued, kEnabled, kWritten, kSettled, kDone };
struct Command {
    uint32_t step;
    uint32_t words[2];
    unsigned count;  // 0: no packet, the command is the next frame end
    bool syncpt;     // step 4 resets the DSI syncpoint block (switch-backlight-syncpt, default 1)
    unsigned deferrals;
    uint32_t timing;  // DC_DISP_DISPLAY_DBG_TIMING at step 4
    uint32_t status;  // DSI_STATUS at step 4
};
Command gCommand;
uint64_t gFrames;       // frame ends the filter took while a command was out (atomic)
bool gFrameEndsDriven;  // the display calls the filter (atomic)

IOLock *volatile gLock;  // serializes commands
volatile uint32_t *gCar, *gDc, *gDsi;
bool gControlSent, gHaveLevel, gSyncptReset = true;
unsigned gLevel;

IOLock *makeLock(IOLock *volatile *slot)
{
    if (!*slot) {
        IOLock *lock = IOLockAlloc();
        if (lock && !OSCompareAndSwapPtr(nullptr, lock, slot)) IOLockFree(lock);
    }
    return *slot;
}

// Under the lock. Reading a gated or reset block would stall the bus, so the clocks come first. A stopped DC
// (NativeLegacyDisplay's display power off) sends no stream for a packet to ride.
bool displayRunning()
{
    if (!gCar) {
        gDc = (volatile uint32_t *)ml_io_map(kDisplayAPA, kDcMapBytes);
        gDsi = (volatile uint32_t *)ml_io_map(kDsiPA, 0x1000);
        gCar = (volatile uint32_t *)ml_io_map(kCarPA, 0x1000);
        unsigned syncpt = 1;
        PE_parse_boot_argn("switch-backlight-syncpt", &syncpt, sizeof(syncpt));
        gSyncptReset = syncpt != 0;
    }
    if (!gCar || !gDc || !gDsi) return false;
    if (!(gCar[CLK_OUT_ENB_L / 4] & kDisp1) || (gCar[RST_DEVICES_L / 4] & kDisp1) ||
        !(gCar[CLK_OUT_ENB_H / 4] & kDsiClock) || (gCar[RST_DEVICES_H / 4] & kDsiClock)) {
        return false;
    }
    return (gDsi[DSI_POWER_CONTROL] & kDsiPowerEnable) && (gDsi[DSI_CONTROL] & kDsiVideoEnable) &&
           (gDc[DC_CMD_DISPLAY_COMMAND] & kDisplayModeMask);
}

uint32_t step()
{
    return __atomic_load_n(&gCommand.step, __ATOMIC_ACQUIRE);
}
uint64_t frames()
{
    return __atomic_load_n(&gFrames, __ATOMIC_ACQUIRE);
}

uint64_t frameDeadline()
{
    uint64_t deadline;
    clock_interval_to_deadline(kFrameTimeoutUS, kMicrosecondScale, &deadline);
    return deadline;
}

[[noreturn]] void lostFrameEnd(const char *what)
{
    char gic[192];
    switch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
    panic("TegraPanel: %s: no DC A frame end in 50 ms (step %u): INT_STATUS %#x MASK %#x ENABLE %#x CMD %#x DBG_TIMING "
          "%#x, %s",
          what, step(), gDc[DC_CMD_INT_STATUS], gDc[DC_CMD_INT_MASK], gDc[DC_CMD_INT_ENABLE],
          gDc[DC_CMD_DISPLAY_COMMAND], gDc[DC_DISP_DISPLAY_DBG_TIMING], gic);
}

// Under the lock, the display running. Hands the filter `count` packet words (none: the command is the next frame end)
// and sleeps until it has played them. Each frame end the filter takes wakes this thread (thread_wakeup is safe in
// primary interrupt context; the wait is asserted before the state is checked, so none is missed).
void play(const uint32_t *words, unsigned count, const char *what)
{
    if (!__atomic_load_n(&gFrameEndsDriven, __ATOMIC_ACQUIRE)) {
        panic("TegraPanel: %s: the display reports no DC A frame ends (its vblank source is off: "
              "sheng-vsync/switch-vblank, "
              "or it does not call tegra_panel_frame_end_filter)",
              what);
    }
    const uint32_t enable = gDc[DC_CMD_INT_ENABLE], mask = gDc[DC_CMD_INT_MASK];
    if (!(enable & mask & kFrameEndInt)) {
        panic("TegraPanel: %s: DC A FRAME_END is not enabled and unmasked (INT_ENABLE %#x MASK %#x)", what, enable,
              mask);
    }
    for (unsigned i = 0; i < count; ++i) gCommand.words[i] = words[i];
    gCommand.count = count;
    gCommand.syncpt = gSyncptReset;
    gCommand.deferrals = 0;
    const uint32_t before = gDsi[DSI_STATUS];
    uint64_t seen = frames(), deadline = frameDeadline();
    __atomic_store_n(&gCommand.step, kQueued, __ATOMIC_RELEASE);
    for (;;) {
        assert_wait_deadline((event_t)&gCommand, THREAD_UNINT, deadline);
        wait_result_t result = THREAD_AWAKENED;
        if (step() == kDone || frames() != seen) {
            clear_wait(current_thread(), THREAD_AWAKENED);
        } else {
            result = thread_block(THREAD_CONTINUE_NULL);
        }
        if (step() == kDone) break;
        if (frames() != seen) {
            seen = frames();
            deadline = frameDeadline();
        } else if (result == THREAD_TIMED_OUT) {
            lostFrameEnd(what);
        }
    }
    __atomic_store_n(&gCommand.step, kIdle, __ATOMIC_RELAXED);
    if (!count) return;
    static bool lateReset, lineBuffer;
    if (!(gCommand.timing & kDbgVBlank) && !lateReset) {
        lateReset = true;
        IOLog("TegraPanel: %s: step 4 ran outside vertical blanking after %u frame ends (DBG_TIMING %#x)\n", what,
              gCommand.deferrals, gCommand.timing);
    }
    if ((gCommand.status & ~before & kLineBufferErrors) && !lineBuffer) {
        lineBuffer = true;
        IOLog("TegraPanel: %s: DSI line buffer over/underflow (DSI_STATUS %#x before, %#x after)\n", what, before,
              gCommand.status);
    }
}

// hekate display_dsi_vblank_write() for an already encoded packet.
int sendPacket(const uint32_t *words, unsigned count, const char *what)
{
    if (!displayRunning()) return -1;
    play(words, count, what);
    return 0;
}

int sendCommand(uint32_t command, const char *what)
{
    const uint32_t word = command << 8 | kShortWrite;
    return sendPacket(&word, 1, what);
}

int sendCommand(uint32_t command, uint32_t parameter, const char *what)
{
    const uint32_t word = (command | parameter << 8) << 8 | kShortWriteParam;
    return sendPacket(&word, 1, what);
}

// DCS long write: header (3 bytes: command + 2 parameter bytes), then the payload, big-endian level.
int sendBrightness(unsigned level)
{
    const uint32_t words[2] = {3u << 8 | kLongWrite, kSetBrightness | (level >> 8) << 8 | (level & 0xff) << 16};
    return sendPacket(words, 2, "brightness");
}

// Under the lock.
int setBrightness(unsigned level)
{
    int result = 0;
    if (!gControlSent) {
        result = sendCommand(kControlDisplay, kBrightnessControl | kDimmingControl, "brightness control");
        gControlSent = result == 0;
    }
    if (!result) result = sendBrightness(level);
    return result;
}

int bootLevel()
{
    if (!displayRunning()) return -1;
    const uint32_t duty = gDc[DC_COM_SCRATCH_REGISTER_B];
    if (!duty || duty > 255) return -1;
    return (int)(duty * TEGRA_PANEL_BRIGHTNESS_MAX / 255);
}

template <typename F> int locked(F body)
{
    IOLock *lock = makeLock(&gLock);
    if (!lock) return -1;
    IOLockLock(lock);
    const int result = body();
    IOLockUnlock(lock);
    return result;
}
}  // namespace

extern "C" int tegra_panel_set_brightness(unsigned level)
{
    if (level > TEGRA_PANEL_BRIGHTNESS_MAX) level = TEGRA_PANEL_BRIGHTNESS_MAX;
    return locked([level] {
        const int result = setBrightness(level);
        if (!result) {
            gLevel = level;
            gHaveLevel = true;
        }
        return result;
    });
}

extern "C" int tegra_panel_power_off(void)
{
    return locked([] {
        // Remember what was showing before the brightness goes to 0, for tegra_panel_power_on(-1).
        if (!gHaveLevel) {
            const int boot = bootLevel();
            if (boot >= 0) {
                gLevel = (unsigned)boot;
                gHaveLevel = true;
            }
        }
        int result = sendBrightness(0);
        if (result) return result;
        IOSleep(100);  // hekate _display_dsi_backlight_brightness: the panel's dimming ramp
        if ((result = sendCommand(kDisplayOff, "display off"))) return result;
        IOSleep(40);
        if ((result = sendCommand(kEnterSleep, "sleep in"))) return result;
        IOSleep(120);
        gControlSent = false;  // sent again on wake, as L4T's init list does
        return 0;
    });
}

extern "C" int tegra_panel_power_on(int level)
{
    return locked([level] {
        if (!displayRunning()) return -1;
        play(nullptr, 0, "scanout restart");
        int result = sendCommand(kExitSleep, "sleep out");
        if (result) return result;
        IOSleep(180);
        if ((result = sendCommand(kControlDisplay, kBrightnessControl | kDimmingControl, "brightness control")))
            return result;
        gControlSent = true;
        if ((result = sendCommand(kDisplayOn, "display on"))) return result;
        IOSleep(20);
        unsigned target = level >= 0 ? (unsigned)level : gHaveLevel ? gLevel : TEGRA_PANEL_BRIGHTNESS_MAX / 2;
        if (target > TEGRA_PANEL_BRIGHTNESS_MAX) target = TEGRA_PANEL_BRIGHTNESS_MAX;
        if ((result = sendBrightness(target))) return result;
        gLevel = target;
        gHaveLevel = true;
        return 0;
    });
}

extern "C" int tegra_panel_boot_brightness(void)
{
    return locked([] { return bootLevel(); });
}

extern "C" int tegra_panel_brightness(void)
{
    return __atomic_load_n(&gHaveLevel, __ATOMIC_RELAXED) ? (int)__atomic_load_n(&gLevel, __ATOMIC_RELAXED) : -1;
}

// Primary interrupt context, at every DC A FRAME_END; one step of the command out, if any (see the top of this file).
extern "C" void tegra_panel_frame_end_filter(void)
{
    if (!__atomic_load_n(&gFrameEndsDriven, __ATOMIC_RELAXED))
        __atomic_store_n(&gFrameEndsDriven, true, __ATOMIC_RELEASE);
    switch_als_frame_end();
    uint32_t next = step();
    switch (next) {
    case kQueued:
        if (!gCommand.count) {
            next = kDone;
            break;
        }
        gDsi[DSI_VIDEO_MODE_CONTROL] = kCommandsInVideo | kOnLine4;
        next = kEnabled;
        break;
    case kEnabled:
        for (unsigned i = 0; i < gCommand.count; ++i) gDsi[DSI_WR_DATA] = gCommand.words[i];
        next = kWritten;
        break;
    case kWritten: next = kSettled; break;
    case kSettled:
        IODelay(14);
        gCommand.timing = gDc[DC_DISP_DISPLAY_DBG_TIMING];
        if (!(gCommand.timing & kDbgVBlank) && gCommand.deferrals < kResetDeferrals) {
            ++gCommand.deferrals;
            break;
        }
        gCommand.status = gDsi[DSI_STATUS];
        if (gCommand.syncpt) {
            gDsi[DSI_INCR_SYNCPT_CNTRL] = kSyncptSoftReset;
            IODelay(300);
            gDsi[DSI_INCR_SYNCPT_CNTRL] = 0;
            IODelay(300);
        }
        gDsi[DSI_VIDEO_MODE_CONTROL] = 0;
        next = kDone;
        break;
    default:  // idle, or done and not yet taken back
        return;
    }
    __atomic_fetch_add(&gFrames, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&gCommand.step, next, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gCommand);
}
