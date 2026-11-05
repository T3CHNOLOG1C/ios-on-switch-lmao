#include "NativeScanout.h"  // NativeLegacyRequest.h plus the Switch's rotated scanout
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <os/log.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include "../ArmGIC/IOSwitchInterrupts.h"
#include "../TegraDisplayFlip/TegraBlockSwizzle.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"
#include "../TegraDisplayFlip/TegraScanoutRing.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUDomain.h"
#include "DisplaySmmuProbe.h"
#include "TegraDisplayPower.h"
#include "../SwitchPower/SwitchPower.h"
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned, void *, void *);
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *, IOMemoryMap *, uint32_t, uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t, uint32_t);
extern "C" void native_legacy_client_observed(IOReturn, IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned, IOReturn, unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
// drivers/TegraGM20B gpu.c: waits for every GPU submission, then cleans+invalidates the range. Weak: absent, the copy is unchanged.
// 0, or -ETIMEDOUT once the deadline passed (the range is invalidated either way); with no GPU channel it returns at once.
extern "C" int tegra_gm20b_sync_for_cpu(const void *, uint64_t, uint64_t) __attribute__((weak));
extern "C" uint64_t native_legacy_surface_width(OSObject *), native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *), native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *), native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_frame_observed(unsigned kind, uint64_t count, uint64_t minx, uint64_t miny, uint64_t maxx,
                                             uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors, const uint64_t *counts, uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool, unsigned, const void *, const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t, uint64_t, uint32_t, uint32_t, uint64_t, uint64_t, uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn, const void *, uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes);
extern "C" void native_legacy_retirement_observed(IOService *);
// GPU hand-off (boot-arg sheng-fb-gpu=1): the frame is offered to the privileged GPU client instead of being copied; the swap retires when the
// client reports the GPU copy done (native_legacy_gpu_complete). A refused offer or a failed copy uses the CPU copy below and is counted.
extern "C" bool native_legacy_gpu_offer(void *display, IOMemoryDescriptor *, uint64_t stride, uint32_t width,
                                        uint32_t height);
// The GPU driver's copy engine instead (boot-arg ioswitch-gpu-copy=1, TegraScanoutCopy.h): weak, absent the CPU copies every frame.
// Weak default that declines every offer (the frame takes the CPU copy); drivers/TegraGM20B's strong definition replaces it at
// link time. The kernel is linked statically, and ld refuses an undefined weak reference there, so the default must exist.
extern "C" __attribute__((weak)) int tegra_gm20b_scanout_copy(const struct TegraScanoutCopy *)
{
    return -1;
}
// XNU's text console scrolled by window A's start address (osfmk/console/video_console.c vc_tape; startConsoleTape below).
// Weak defaults for a kernel without it: the console keeps redrawing its text to scroll, as before.
extern "C" __attribute__((weak)) int vc_tape_attach(vm_offset_t, uint32_t, uint32_t, uint32_t,
                                                    void (*)(void *, uint32_t), int (*)(void *), int (*)(void *),
                                                    void *)
{
    return 1;
}
extern "C" __attribute__((weak)) void vc_tape_detach(void) {}
// XNU (kern_exec.c) while the one-time data migration runs: the console stays on screen and the UI's frames are drawn and
// retired but not shown (presentBlock), so the user watches the migration's progress lines. Weak: absent, never held.
extern "C" __attribute__((weak)) volatile int ioswitch_console_hold;
// XNU (spec_vnops.c, temporary): ends the boot I/O census at the first client frame; it prints from a thread call.
extern "C" __attribute__((weak)) void ioswitch_boot_io_report(void);
extern "C" __attribute__((weak)) int vc_tape_panic(void)
{
    return 0;
}
// The compositor's swapped surfaces, for a display that fetches them itself: [0] distinct surfaces seen, [1] swaps checked; for the first four, at
// [2 + 3 * k]: allocation bytes, physical segments, largest segment (KiB). The pages are wired for the count and released.
extern "C" {
volatile uint32_t native_legacy_surfaces[14];
}
static void noteSurface(OSObject *surface, IOMemoryDescriptor *memory)
{
    static OSObject *seen[4];
    native_legacy_surfaces[1] = native_legacy_surfaces[1] + 1;
    for (OSObject *known : seen)
        if (known == surface) return;
    const unsigned k = native_legacy_surfaces[0];
    native_legacy_surfaces[0] = k + 1;
    if (k >= 4) return;
    seen[k] = surface;
    if (memory->prepare() != kIOReturnSuccess) return;
    uint32_t segments = 0;
    uint64_t largest = 0;
    for (IOByteCount offset = 0, length = 0; offset < memory->getLength() && segments < 100000;
         offset += length, ++segments) {
        if (!memory->getPhysicalSegment(offset, &length, kIOMemoryMapperNone) || !length) break;
        if (length > largest) largest = length;
    }
    memory->complete();
    native_legacy_surfaces[2 + 3 * k] = uint32_t(memory->getLength());
    native_legacy_surfaces[3 + 3 * k] = segments;
    native_legacy_surfaces[4 + 3 * k] = uint32_t(largest >> 10);
    // Whether a client surface could ever be scanned in place (one physically contiguous run; the DC has no SMMU on this boot chain).
    IOLog("IOSwitch display: client surface %u: %llu bytes in %u physical segments, largest %llu KiB\n", k,
          (unsigned long long)memory->getLength(), segments, (unsigned long long)(largest >> 10));
}
extern "C" void native_legacy_gpu_fallback_observed(int status);
// Swaps retired when the GPU client took the frame (before its copy finished), and copies that then failed after such a retirement.
extern "C" {
volatile uint64_t native_legacy_early_retires, native_legacy_early_failures;
}
// Display refresh tick (boot-arg sheng-vsync=<Hz>, 0 or absent: off). The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. Display controller A's vblank interrupt drives
// it here (boot-arg ioswitch-vblank, default 1) while at least one client has vsync notifications enabled. An interrupt that cannot be had
// or stays silent panics; the work-loop timer only stands in with ioswitch-vblank=0. Events: 0 armed, 1 sent, 2 idle (no client), 3 late (timer deadline passed, cadence
// restarted), 4 timer started, 5 refresh properties reported at that rate.
extern "C" void native_legacy_vsync_notify(void *display, uint64_t timestamp, uint64_t continuousTime,
                                           uint64_t interval);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void native_legacy_vsync_observed(unsigned event, unsigned hz);
extern "C" uint64_t mach_continuous_time(void);
// SwitchBrightness (TegraPanel.cpp) plays its DSI commands at DC A frame ends, from the vblank interrupt's filter below (primary interrupt
// context). This empty default keeps the display linkable without it; its strong definition wins when SwitchBrightness is linked.
extern "C" __attribute__((weak)) void tegra_panel_frame_end_filter(void) {}
// SwitchBrightness (SwitchBacklight.cpp; display power is on unless switch-sleep=0) takes the render server's display power requests;
// this default (no SwitchBrightness, or switch-sleep=0) leaves them to the original user client.
extern "C" __attribute__((weak)) bool switch_display_power_request(bool)
{
    return false;
}
// SC7 (tegra_display_suspend/resume below) puts the panel to sleep and wakes it with TegraPanel's own power sequence, the one
// SwitchBacklight uses for display power, and powers the pipeline with TegraDisplayPower.cpp. These defaults keep the display
// linkable without either object; the strong definitions win when they are linked, and the hooks panic without them.
extern "C" __attribute__((weak)) int tegra_panel_power_off(void)
{
    panic("IOSwitch display: SC7 needs SwitchBrightness (TegraPanel)");
}
extern "C" __attribute__((weak)) int tegra_panel_power_on(int)
{
    panic("IOSwitch display: SC7 needs SwitchBrightness (TegraPanel)");
}
extern "C" __attribute__((weak)) uint32_t tegra_display_panel_id(void)
{
    panic("IOSwitch display: SC7 needs TegraDisplayPower.o in the build");
}
extern "C" __attribute__((weak)) void tegra_display_power_down(void)
{
    panic("IOSwitch display: SC7 needs TegraDisplayPower.o in the build");
}
extern "C" __attribute__((weak)) void tegra_display_power_up(void)
{
    panic("IOSwitch display: SC7 needs TegraDisplayPower.o in the build");
}
// Display controller A (hekate bdk/display/di.h, Linux drivers/gpu/drm/tegra/dc.h): 32-bit registers, word indices. INT_STATUS is write-1-
// to-clear, INT_ENABLE lets a bit latch, INT_MASK routes a latched bit to the level-high line, GIC_SPI 73 = INTID 105 (mainline
// tegra210.dtsi dc@54200000, hekate irq.h IRQ_DISPLAY). DC A's clock (CAR DISP1, L27) must run before its registers are read.
static constexpr uint64_t kDisplayAPA = 0x54200000, kCarPA = 0x60006000;
static constexpr uint32_t kDcIntStatus = 0x037, kDcIntMask = 0x038, kDcIntEnable = 0x039, kFrameEndInt = 1u << 1,
                          kVblankInt = 1u << 2;
static constexpr uint32_t kDisplayAInterrupt = 105, kCarResetL = 0x004, kCarClockL = 0x010, kCarDisp1 = 1u << 27;
// DISPLAY_COMMAND bits 6:5: 0 stop, 1 continuous display (hekate di.h DISP_CTRL_MODE_*); an assembly register, latched by GENERAL_ACT_REQ.
static constexpr uint32_t kDcDisplayCommand = 0x032, kDcModeMask = 3u << 5, kDcModeContinuous = 1u << 5;
// Window A, for the hardware rotation (boot-arg ioswitch-hw-rotate, NativeScanout.h). Word indices and bits checked against L4T
// drivers/video/tegra/dc/dc_reg.h (switch-l4t-kernel-nvidia linux-dev) and hekate bdk/display/di.h. The window registers sit at words
// 0x700-0x80d, so the controller is mapped 16 KiB deep (the interrupt registers alone fit in the first page).
//   0x040 STATE_ACCESS   READ_MUX_ACTIVE bit 0 (reads see the active copy), WRITE_MUX_ACTIVE bit 2 (kept 0: writes go to the
//                        assembly copy and only an ACT_REQ moves them)
//   0x041 STATE_CONTROL  GENERAL_ACT_REQ bit 0, WIN_A_ACT_REQ bit 1, GENERAL_UPDATE bit 8, WIN_A_UPDATE bit 9; an ACT_REQ bit
//                        reads 1 until the hardware latched it
//   0x042 WINDOW_HEADER  WINDOW_A_SELECT bit 4: which window the 0x7xx/0x8xx words address
//   0x043 REG_ACT_CONTROL  WIN_ACT_CNTR_SEL_HCOUNTER(0) bit 2: 1 latches window A on the line counter (hekate's init, can tear
//                        mid-frame), 0 at the frame boundary; L4T window.c clears it whenever an update is not hsync safe
//   0x700 WIN_OPTIONS    H_DIRECTION bit 0, SCAN_COLUMN bit 4, WIN_ENABLE bit 30;  0x70a LINE_STRIDE (bytes, bits 15:0)
//   0x705 SIZE, 0x706 PRESCALED_SIZE, 0x709 DDA_INCREMENT, 0x703 COLOR_DEPTH (0xC B8G8R8A8, 0xD R8G8B8A8): left as hekate set them
//   0x800 WINBUF_START_ADDR, 0x806 ADDR_H_OFFSET (bytes), 0x808 ADDR_V_OFFSET (lines), 0x80a UFLOW_STATUS,
//   0x80b SURFACE_KIND   PITCH 0, BLOCK 2, BLOCK_HEIGHT(log2 GOBs) in bits 6:4
// INT_STATUS/INT_ENABLE WIN_A_UF_INT bit 8: window A ran out of pixels during a line.
static constexpr uint64_t kDcMapBytes = 0x4000;
static constexpr uint32_t kDcStateAccess = 0x040, kDcStateControl = 0x041, kDcWindowHeader = 0x042,
                          kDcRegActControl = 0x043;
static constexpr uint32_t kWinOptions = 0x700, kWinColorDepth = 0x703, kWinPosition = 0x704, kWinSize = 0x705,
                          kWinPrescaled = 0x706;
static constexpr uint32_t kWinDdaIncrement = 0x709, kWinLineStride = 0x70a;
// For SC7 only (window A as it was, after the cold re-init): BUFFER_CONTROL, H/V_INITIAL_DDA, BLEND_LAYER_CONTROL (hekate di.h),
// DISP_WIN_OPTIONS (DSI_ENABLE), COM_SCRATCH_REGISTER_B (hekate's brightness duty, TegraPanel's boot level).
static constexpr uint32_t kWinBufferControl = 0x702, kWinHInitialDda = 0x707, kWinVInitialDda = 0x708,
                          kWinBlendLayer = 0x716;
static constexpr uint32_t kDcDispWinOptions = 0x402, kDcScratchB = 0x326;
static constexpr uint32_t kWinbufStartAddr = 0x800, kWinbufHOffset = 0x806, kWinbufVOffset = 0x808,
                          kWinbufUnderflow = 0x80a;
static constexpr uint32_t kWinbufSurfaceKind = 0x80b, kWinbufStartAddrHi = 0x80d;
static constexpr uint32_t kReadMuxActive = 1u << 0, kWriteMuxActive = 1u << 2, kWindowASelect = 1u << 4,
                          kWinAActHcntrSel = 1u << 2;
static constexpr uint32_t kGeneralActReq = 1u << 0, kWinAActReq = 1u << 1, kGeneralUpdate = 1u << 8,
                          kWinAUpdate = 1u << 9;
static constexpr uint32_t kHDirection = 1u << 0, kVDirection = 1u << 2, kScanColumn = 1u << 4, kWinEnable = 1u << 30,
                          kWinAUnderflowInt = 1u << 8;
// _di_winA_block: BLOCK | BLOCK_HEIGHT(4); the stride is the landscape line; H_OFFSET is the last byte of a line, where
// H_DIRECTION starts (L4T window.c: (x + w) * Bpp - 1 = 0x13FF, the value hekate quotes for Horizon's own block scanout).
// Portrait-native (IOSwitchPortrait.h): the same block layout scanned row by row, unrotated, as hekate's pitch window
// scans; kBlockScan is what the block layout adds to hekate's WIN_OPTIONS.
static constexpr uint32_t kBlockSurfaceKind = 2u | (4u << 4), kBlockLineStride = (uint32_t)kNativeBlockStride,
                          kBlockHOffset = IOSWITCH_PORTRAIT_NATIVE ? 0 : (uint32_t)kNativeBlockStride - 1,
                          kBlockScan = IOSWITCH_PORTRAIT_NATIVE ? 0 : kScanColumn | kHDirection;
static constexpr const char *kBlockScanName =
    IOSWITCH_PORTRAIT_NATIVE ? "row scan of the portrait" : "column scan of the landscape";
// The pitch window hekate hands over (_di_winA_pitch): 720x1280 at 1.0x, 2880-byte lines. The rotation changes only
// WIN_OPTIONS, LINE_STRIDE, SURFACE_KIND and ADDR_H_OFFSET; everything else must already be this or the switch would scramble.
static constexpr uint32_t kPitchSize = (1280u << 16) | 720u, kPitchPrescaled = (1280u << 16) | (720u * 4),
                          kUnitDda = 0x10001000;
// The console tape (startConsoleTape): hekate's pitch window with kTapeGap spare text lines (64 bytes each: 16 portrait columns)
// after every 2880-byte row. kTapeLines positions 64 bytes apart, then the console redraws at position 0, whose window no window
// within kTapeGap lines of the end reaches (two window extents, plus slack for a multi-line scroll).
static constexpr uint32_t kTapeLineBytes = 64, kTapeGap = 45, kTapeStride = 2880 + kTapeGap * kTapeLineBytes;
static constexpr uint64_t kTapeWindow = 1279ull * kTapeStride + 2880;
static constexpr uint32_t kTapeLines = (uint32_t)((kTapeWindow + kTapeLineBytes - 1) / kTapeLineBytes) + 2 * kTapeGap;
static constexpr uint64_t kTapeBytes =
    ((uint64_t)(kTapeLines - 1) * kTapeLineBytes + kTapeWindow + 0x3fff) & ~0x3fffull;
static constexpr unsigned kTapeWaitMS = 100;  // the console's wait for a frame end: six frames
// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spelling ('&BGA', one plane): the GPU that renders the surface here writes plain rows
// either way (no compression is implemented), so the bytes are the same.
static inline bool nativeBgra(uint32_t format, uint32_t planes)
{
    return (format == 0x42475241 && !planes) || (format == 0x26424741 && planes <= 1);
}
// Panics draw into the boot framebuffer as the pitch console does (rotation 3: IOSwitchPanicSave.c's panic screen once the
// GUI owns the display, XNU's console before that, or IOSwitchDiag's own banner), which a block-linear column scan would
// show scrambled. While window A is rotated this chains XNU's DEVELOPMENT panic hook (PE_arm_debug_panic_hook, called from
// PE_panic_hook in debug.c panic_trap_to_debugger before IOSwitchDiag's ioswitch_diag_panic, and again by
// IOSwitchPanicSave.c once the other CPUs are halted): the first call blacks the pitch area, every call puts back window
// A's four pitch registers, latched at the next frame, so the panic text lands on a readable screen. From the first call
// on, the frame-end filter and the flip leave window A alone: until debug.c halts them, the other CPUs still run this
// driver, whose next flip would scan block-linear again and whose latch check would panic over the restored registers.
// Before the first frame window A scans XNU's console tape (startConsoleTape): a panic the console still owns is drawn
// there by the console itself, which scrolls the tape without waiting; one after the console released the screen is
// IOSwitchPanicSave's, so window A goes back to the boot framebuffer as when rotated.
// Interrupts are off and other CPUs may be stopped: no locks, no allocation, no logging, only stores.
#if !(DEVELOPMENT || DEBUG)
#error "the hardware rotation chains PE_arm_debug_panic_hook, which is const outside DEVELOPMENT and DEBUG kernels"
#endif
namespace {
struct PanicRestore {
    volatile uint32_t *dc;
    volatile uint64_t *pixels;
    uint64_t bytes;
    uint32_t options, stride, kind, hOffset, start;  // start: the boot framebuffer, where the panic console draws
    void (*previous)(const char *);
    volatile bool armed;      // window A is rotated: a panic must put the pitch window back
    volatile bool tape;       // window A scans the console tape: it must too, unless the console still owns the screen
    volatile bool panicking;  // a panic began: window A is the panic's
};
PanicRestore gPanicRestore;
void restorePitchOnPanic(const char *message)
{
    const bool first = !gPanicRestore.panicking;
    gPanicRestore.panicking = true;
    __asm__ volatile("dmb ish" ::: "memory");
    if (gPanicRestore.armed || (gPanicRestore.tape && !vc_tape_panic())) {
        if (first) {
            volatile uint64_t *pixels = gPanicRestore.pixels;
            for (uint64_t i = 0; i < gPanicRestore.bytes / 8; ++i) pixels[i] = 0xff000000ff000000ull;
        }
        volatile uint32_t *dc = gPanicRestore.dc;
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcWindowHeader] = kWindowASelect;
        dc[kWinLineStride] = gPanicRestore.stride;
        dc[kWinbufSurfaceKind] = gPanicRestore.kind;
        dc[kWinbufHOffset] = gPanicRestore.hOffset;
        dc[kWinbufStartAddr] = gPanicRestore.start;
        dc[kWinbufStartAddrHi] = 0;
        dc[kWinOptions] = gPanicRestore.options;
        __asm__ volatile("dsb sy" ::: "memory");
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
    }
    if (gPanicRestore.previous) gPanicRestore.previous(message);
}
// Clean and invalidate [address, address + bytes) to the point of coherency, by VA (CTR_EL0 line size).
void cleanInvalidate(const void *address, uint64_t bytes)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 15), end = (uintptr_t)address + bytes;
    __asm__ volatile("dsb sy" ::: "memory");
    for (uintptr_t at = (uintptr_t)address & ~(line - 1); at < end; at += line)
        __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
// The MAIR attribute byte the kernel's translation of address resolves to (PAR_EL1 after AT S1E1R, as XNU's mmu_kvtop):
// 0xff write-back cacheable, 0x44 non-cacheable (write-combined), 0x00/0x04 device; 0x100 when the page is not mapped.
// Interrupts are off around it so no handler's own AT replaces PAR_EL1 in between.
uint32_t mairAttribute(const void *address)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    uint64_t par;
    __asm__ volatile("at s1e1r, %1\n\tisb\n\tmrs %0, par_el1" : "=r"(par) : "r"(address) : "memory");
    ml_set_interrupts_enabled(enabled);
    return (par & 1) ? 0x100 : (uint32_t)(par >> 56);
}
// Steady-state timing (NativeLegacyDisplay, boot-arg ioswitch-display-stats=1): one mean/max/count per stage, in timebase ticks.
struct Span {
    uint64_t sum, max;
    uint32_t count;
    void add(uint64_t ticks)
    {
        sum += ticks;
        if (ticks > max) max = ticks;
        ++count;
    }
    uint64_t meanUs() const { return count ? ticksToUs(sum / count) : 0; }
    uint64_t maxUs() const { return ticksToUs(max); }
    static uint64_t ticksToUs(uint64_t ticks)
    {
        uint64_t ns;
        absolutetime_to_nanoseconds(ticks, &ns);
        return ns / 1000;
    }
};
// Frame-time distribution (r6-lows, stats only): fixed bins, no allocation, written under the work-loop gate. 1 ms bins up to
// 64 ms, 4 ms to 128, 8 ms to 256, 64 ms to 1024, then one bin for the rest. A quantile reads back its bin's upper edge, capped
// at the largest value seen, so it is at most 1 ms high below 64 ms (presents are whole frames, 16.7 ms apart).
struct FrameHist {
    static constexpr unsigned kBins = 64 + 16 + 16 + 12 + 1;
    uint32_t bins[kBins];
    uint32_t count;
    uint64_t maxUs;
    static unsigned binOf(uint64_t us)
    {
        const uint64_t ms = us / 1000;
        if (ms < 64) return (unsigned)ms;
        if (ms < 128) return 64 + (unsigned)((ms - 64) / 4);
        if (ms < 256) return 80 + (unsigned)((ms - 128) / 8);
        if (ms < 1024) return 96 + (unsigned)((ms - 256) / 64);
        return kBins - 1;
    }
    static uint64_t upperUs(unsigned bin)
    {
        if (bin < 64) return (bin + 1) * 1000ull;
        if (bin < 80) return (64 + (bin - 63) * 4) * 1000ull;
        if (bin < 96) return (128 + (bin - 79) * 8) * 1000ull;
        if (bin < 108) return (256 + (bin - 95) * 64) * 1000ull;
        return UINT64_MAX;
    }
    void add(uint64_t us)
    {
        ++bins[binOf(us)];
        ++count;
        if (us > maxUs) maxUs = us;
    }
    // The permille-th quantile (500 median, 990 p99, 999 p99.9), in microseconds.
    uint64_t quantileUs(unsigned permille) const
    {
        if (!count) return 0;
        const uint64_t rank = ((uint64_t)count * permille + 999) / 1000;
        uint64_t seen = 0;
        for (unsigned bin = 0; bin < kBins; ++bin) {
            seen += bins[bin];
            if (seen >= rank) {
                const uint64_t upper = upperUs(bin);
                return upper < maxUs ? upper : maxUs;
            }
        }
        return maxUs;
    }
};
}
class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay) IOMemoryMap *bootMapping = nullptr;
    static constexpr uint32_t kScanFrames = 32,
                              kColorFrames = 8;  // the probe's frame records, and those of them with colours
    uint32_t scanBudget =
        0;  // kScanFrames with boot-arg ioswitch-frame-scan=1 (off: two byte-wise passes cost ~150-300 ms a frame)
    void *shadow = nullptr;
    uint64_t shadowBytes = 0;
    bool shadowValid = false;
    bool primary = false;
    bool logicalPM = false;
    bool transferred = false;
    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    const OriginalLegacySwapRequest *validated[8] = {};
    uint64_t validatedAt[8] = {};  // when each was first accepted (mach time): the client's submission
    bool isValidated(const OriginalLegacySwapRequest *request) const
    {
        if (!request) return false;
        for (auto entry : validated)
            if (entry == request) return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request)
    {
        for (auto &entry : validated)
            if (entry == request) entry = nullptr;
    }
    // True when the request is new (not a second validation of one already pending).
    bool addValidated(const OriginalLegacySwapRequest *request, uint64_t now)
    {
        if (isValidated(request)) return false;
        unsigned slot = 0;
        while (slot < 8 && validated[slot]) ++slot;
        if (slot == 8) slot = 0;
        validated[slot] = request;
        validatedAt[slot] = now;
        return true;
    }
    // The request queued behind this one in the family's tail queue (each request's next at +2960, OriginalLegacyFramebufferABI.h
    // activateOnlyPendingSwap), or none.
    static const OriginalLegacySwapRequest *queuedAfter(const OriginalLegacySwapRequest *request)
    {
        const OriginalLegacySwapRequest *next = nullptr;
        if (request) memcpy(&next, reinterpret_cast<const unsigned char *>(request) + 2960, sizeof(next));
        return next;
    }
    uint64_t submittedAt(const OriginalLegacySwapRequest *request) const
    {
        for (unsigned i = 0; i < 8; ++i)
            if (request && validated[i] == request) return validatedAt[i];
        return 0;
    }
    const OriginalLegacySwapRequest *gpuRequest = nullptr;
    uint64_t gpuLength = 0;
    bool gpuBypass = false;
    // The swap of the frame the GPU client is copying already retired (boot-arg sheng-early-retire, default 1).
    bool gpuRetired = false;
    // GPU frame copy (boot-arg ioswitch-gpu-copy=1, double-buffered hardware rotation only; TegraScanoutCopy.h): the GPU driver's copy
    // engine writes the swapped surface into the buffer window A does not scan, and its completion flips window A to it, as the CPU
    // copy's end does. gpuScanout: the outstanding offer (gpuRequest) is such a copy, not the sheng presenter's. The completion comes
    // through gpuDoneSource from any context; one not in by its deadline panics.
    bool gpuCopy = false, gpuScanout = false, gpuPresentDeferred = false;
    uint64_t gpuSequence = 0, gpuOfferedAt = 0;
    IOMemoryDescriptor *gpuSource = nullptr;  // retained from the offer to the completion
    const OriginalLegacySwapRequest *gpuPresented =
        nullptr;  // a completed copy's swap, until it is presented (after SC7: at the resume)
    IOInterruptEventSource *gpuDoneSource = nullptr;
    IOTimerEventSource *gpuDeadline = nullptr;
    int gpuStatus = 0;
    bool gpuDonePending = false;  // written by native_legacy_gpu_complete before it signals gpuDoneSource
    static constexpr unsigned kGpuCopyDeadlineMS = 1000;
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue at +5592 never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer = nullptr;
    uint64_t vsyncInterval = 0, vsyncNext = 0;
    unsigned vsyncHz = 0;
    bool vsyncArmed = false;
    unsigned vsyncSilent = 0;  // DC A frame ends without a V_BLANK while armed (watchVsync)
    static constexpr unsigned kVsyncSilentFrames = 30;
    // DC A interrupt, on the same work loop. FRAME_END stays unmasked while it runs: it feeds the panel's DSI commands and shows the
    // interrupt is alive. V_BLANK is unmasked only while a vsync client is registered. The filter clears both in INT_STATUS, which lowers
    // the line, and hands them to the action; the timer only checks once, 250 ms after the start, that frame ends arrive.
    IOFilterInterruptEventSource *vblankSource = nullptr;
    IOMemoryMap *dcMap = nullptr;
    volatile uint32_t *dc = nullptr;
    uint32_t dcEnable = 0, dcMask = 0;  // INT_ENABLE and INT_MASK as found, restored on stop
    bool scanoutStopped = false;        // display power off (setScanout): no frame ends or vblanks are expected
    uint32_t dcPending = 0;             // INT_STATUS bits from the filter, for the action
    uint64_t vblankTime = 0, vblankContinuous = 0, vblankLast = 0, frameEnds = 0;
    // Hardware rotation (boot-arg ioswitch-hw-rotate, default 1; NativeScanout.h). The CPU stops rotating: each client frame is
    // swizzled, unrotated, into the block-linear extent of the boot framebuffer and window A column-scans it. The first such
    // frame switches window A (four registers, latched at a frame boundary); the frame ends after it confirm the latch, read the
    // active registers back, and watch window A for underflow. Kick and frame ends both run under the family's work-loop gate.
    bool hwRotate = false;                // armed at start for the primary, rotated (v_rotate 3) panel
    IOMemoryMap *blockMapping = nullptr;  // the boot framebuffer's kNativeBlockBytes, write-combined
    bool rotated = false, rotationLatched = false;
    uint64_t rotationRequested = 0;  // frameEnds when the ACT_REQ went out
    uint32_t pitchOptions = 0, pitchStride = 0, pitchKind = 0, pitchHOffset = 0,
             pitchActControl = 0;    // active values before the switch
    unsigned underflowRun = 0;       // consecutive frame ends with WIN_A_UF_INT latched, once latched
    bool rotationAnnounced = false;  // the latch line is logged once, not again after every SC7 resume
    static constexpr unsigned kLatchFrames = 4, kUnderflowFrames = 30;
    // Multiple buffering (boot-arg ioswitch-scanout-double=1, default 0; only with the hardware rotation). Three block-linear
    // surfaces (two if no third could be had): 0 is the boot framebuffer's (blockMapping), 1 and 2 physically contiguous buffers
    // below 4 GiB. A frame is swizzled into one window A neither scans nor may take (ring.target()), then window A's start
    // address flips to it, latched at the next frame boundary (frame-counter latching, set by startRotation). A frame finished
    // while an older flip is still out replaces that flip (latest frame wins, TegraScanoutRing.h): it scans out at the next
    // boundary and the older frame is not shown. With every buffer held (one scanned, one asked for, one replaced and not yet
    // known to be untaken) the next frame's kick is deferred to the frame end that settles it, so a surface is written only after
    // the controller has stopped reading it. The first frame goes to surface 1 together with the switch to the rotated layout.
    // Kick, flip and frame ends all run under the family's work-loop gate.
    bool doubleBuffer = false;
    static constexpr unsigned kScanBuffers = TegraScanoutRing::kMaxBuffers;
    static constexpr int kNoBuffer = TegraScanoutRing::kNone;
    IOBufferMemoryDescriptor *scanBuffers[kScanBuffers] =
        {};                                        // surfaces 1 and 2's pages ([0] unused: the boot framebuffer)
    IOMemoryMap *scanMappings[kScanBuffers] = {};  // their write-combined views
    uint64_t scanAddresses[kScanBuffers] = {};     // their physical addresses
    TegraScanoutRing ring;                         // which surface is scanned, asked for, replaced, drawn
    // The direct-scanout gate's probe (DisplaySmmuProbe.h; DESIGN-direct-scanout.md D1-D2): once, kProbeAfterFrames frame ends
    // after window A first scanned a client frame, with nothing in flight, DC A's CRC unit and the display's SMMU domain (ASID
    // TEGRA_SMMU_ASID_DISPLAY on the "dc" swgroup) prove window A reads the scanned buffer through an IOVA slot; then all of
    // it is undone and one line says what was found. Swaps wait meanwhile (about 12 frames). Nothing scans through the SMMU
    // after it: direct scanout is the next stage.
    static constexpr uint32_t kDcCrcControl = 0x300, kDcCrcLatched = 0x329, kCrcOn = (1u << 3) | (1u << 2) | (1u << 0);
    static constexpr uint32_t kDisplay0aTranslation = 1u << 1;  // client display0a (id 1) in TRANSLATION_ENABLE_0
    static constexpr uint64_t kProbeSlot = 0x10000000ull;  // below DRAM (0x80000000): only a translation reaches it
    static constexpr unsigned kProbeTables = 16, kProbeAfterFrames = 600;
    IOBufferMemoryDescriptor *probeTables = nullptr;
    uint64_t probeTablesPa = 0, probeAt = 0;
    TegraSMMUDomain probeDomain;
    DisplaySmmuProbe probe;
    bool probeRan = false, probeUnderflow = false;
    bool probeRequest = false;  // a probe START_ADDR request (to the slot, or back) not yet seen taken
    int probeSurface = kNoBuffer;
    uint32_t probePixel = 0, probeSwgroupWas = 0;
    char probeLine[160] = {};
    bool kickDeferred = false;                  // a swap is waiting for a frame end (or for the SC7 resume)
    uint64_t flipRequested = 0, flipWaits = 0;  // frameEnds when the last flip went out; swaps that waited for a buffer
    IOTimerEventSource *flipTimer =
        nullptr;  // a flip's deadline: a lost DC A interrupt would otherwise stall the screen
    static constexpr unsigned kFlipDeadlineMS = 100;
    // XNU's console tape (startConsoleTape), from start until the first client frame scans: the pages, their write-combined view
    // and physical address, and whether the console draws there (window A scans it; read by the filter and the console's wait).
    IOBufferMemoryDescriptor *tapeBuffer = nullptr;
    IOMemoryMap *tapeMapping = nullptr;
    uint32_t tapeAddress = 0;
    volatile bool tapeAttached = false;
    bool tapeStrideSet = false;  // the console's first show() set LINE_STRIDE (under XNU's console lock)
    bool tapeSettle = false;
    uint64_t tapeSettleRequested = 0;  // a kick waits for the console's last scroll to latch
    uint64_t tapeFrames = 0;           // frame ends while attached: the console's wait event and count
    bool pitchSaved = false;           // pitch* below hold hekate's window (checkPitchWindow)
    // SC7 (tegra_display_suspend/resume): while hwSuspended the DC's clocks are off, so nothing may touch its registers; swaps are
    // deferred to the resume. Window A's assembly registers, the interrupt enables and masks, and whether the scanout ran (the
    // panel was lit) are kept for the cold re-init.
    bool hwSuspended = false, resumeLit = false;
    static constexpr uint32_t
        kWindowAState[] = {kWinColorDepth,     kWinPosition,     kWinSize,           kWinPrescaled,     kWinHInitialDda,
                           kWinVInitialDda,    kWinDdaIncrement, kWinLineStride,     kWinBufferControl, kWinBlendLayer,
                           kWinbufSurfaceKind, kWinbufStartAddr, kWinbufStartAddrHi, kWinbufHOffset,    kWinbufVOffset,
                           kWinOptions};  // WIN_OPTIONS last: the window is enabled once the rest is in place
    uint32_t savedWindow[sizeof(kWindowAState) / sizeof(kWindowAState[0])] = {};
    uint32_t savedDispWinOptions = 0, savedActControl = 0, savedScratchB = 0, savedIntEnable = 0, savedIntMask = 0;
    // Copy time, for the occasional log line (the probe's counters are never printed).
    uint64_t copyFrames = 0, copyTicksSum = 0, copyTicksMax = 0;
    // Copy-path options, read once at start, each off by default:
    //   ioswitch-fast-swizzle=1  the block-linear copy reads the source 16 bytes at a time (TegraBlockSwizzle.h) when it is aligned
    //   ioswitch-surface-maps=1  the kernel mapping of a swapped surface is kept for its next swaps (sourceMapping)
    //   ioswitch-gpu-wait-ms=N   bound on the wait for the GPU before a Metal-rendered surface is read (default 100)
    //   ioswitch-scanout-wc=1    the block-linear scanouts' kernel views are write-combined from their descriptor on (mapScanout)
    //   ioswitch-frame-scan=1    the probe's content scans of the first kScanFrames frames (scanBudget)
    //   ioswitch-gpu-copy=1      the GPU driver copies each frame (offerGpuCopy); needs ioswitch-scanout-double=1
    bool fastSwizzle = false, keepMappings = false, scanoutWC = false;  // scanoutWC: ioswitch-scanout-wc=1 (mapScanout)
    uint64_t gpuWaitNs = 100000000ULL;
    // Kept mappings: each holds a retained descriptor (so its address cannot come back as another surface's) and is reused while it is
    // among the four most recently swapped. The first copy through one takes its soft faults, later ones none. At most four surfaces
    // (about 15 MB at 1280x720) stay alive past their last swap because of this.
    struct SurfaceMapping {
        IOMemoryDescriptor *memory;
        IOMemoryMap *map;
        uint64_t used;
    };
    SurfaceMapping surfaceMappings[4] = {};
    uint64_t surfaceUses = 0;
    // Off-gate copy (boot-arg ioswitch-display-async=1, hardware rotation only): the kick hands a surface swap's GPU wait, scans and
    // swizzle to copyLoop's thread and returns, so neither the compositor's submitting thread nor the family's gate (and with it the
    // vblank action: vsync notifications, flip latches) waits for them; copyFinished, back under the gate, presents the frame and
    // retires the swap. ioswitch-display-async=2 also retires it when the copy starts, as the GPU hand-off does (sheng-early-retire):
    // the compositor renders its next frame, into its other surface, while this one is copied. The next swap still waits for the copy
    // (transferred). job is written under the gate before the hand-off and by the copy thread until copyFinished takes it back.
    unsigned asyncCopy = 0;
    IOWorkLoop *copyLoop = nullptr;
    IOInterruptEventSource *copySource = nullptr;
    struct CopyJob {
        const OriginalLegacySwapRequest *request;
        IOMemoryMap *source, *target;
        bool ownedSource, retired, scan, colors, read;
        uint64_t stride, allocation;
        uint32_t width, height;
        IOReturn status;
        bool timedOut;
        uint64_t gpu, cache, scanned, copy, readTicks;
        uint32_t attribute;
        int swizzled;  // the copy thread's results
    };
    CopyJob job = {};
    bool copyQueued = false, copyStopping = false,
         presentDeferred = false;  // under the gate; presentDeferred: finished during SC7
    // Steady-state timing (boot-arg ioswitch-display-stats=1): one line every 5 s from the frame-end action, the only periodic log
    // line. A swap's stages: queue (accepted to taken by a kick, flip wait included), map, GPU wait, cache op, content scans, copy, and
    // accepted to retired; client is the compositor's own time from a retire to its next submission (0 when it submitted before the
    // retire); vsync late is V_BLANK interrupt to notification sent; gpu-copy is a GPU frame copy's offer to its completion; attr is the
    // MAIR byte of the source and scanout views (mairAttribute).
    // All under the work-loop gate except stVblanks.
    bool stats = false;
    // Boot-args the swap path reads on every frame, parsed once at start (PE_parse_boot_argn scans the whole command line;
    // five scans a frame were the display thread's top function in kprof on r7p5).
    bool bootTransfer = false, bootEarlyRetire = true, bootRetire = false, bootCpuPresent = false, bootValidate = false;
    uint64_t statsStart = 0, lastSubmit = 0, lastRetire = 0, deferredAt = 0;
    Span stQueue = {}, stFlipWait = {}, stMap = {}, stGpu = {}, stCache = {}, stScan = {}, stCopy = {}, stRetire = {},
         stInterval = {}, stClient = {}, stVsyncLate = {};
    uint32_t stSwaps = 0, stVsyncs = 0, stGpuTimeouts = 0, stNewMaps = 0, stSourceAttr = 0x100;
    uint32_t stUnderflows = 0;  // DC A frame ends with WIN_A_UF_INT latched (window A ran dry during a line)
    uint32_t
        stFast = 0,
        stByte =
            0;  // block-linear copies by TegraBlockSwizzle.h, and by the byte loop (ioswitch-fast-swizzle off, or declined)
    uint32_t stGpuCopies = 0, stGpuDeclined = 0;
    Span stGpuCopy = {};  // GPU frame copies done, offers declined, offer to completion
    // Once per window, just before a copy, the source is read alone (readPass): next to the copy's time it shows whether the copy
    // waits on reading the surface or on writing the scanout.
    bool stReadDue = true;
    uint64_t stRead = 0;
    uint32_t stVblanks = 0;  // V_BLANKs the filter took while vsync was armed (atomic: primary interrupt context)
    // Frame pacing (r6-lows). A present is a flip DC A latched, timed at the frame end (or V_BLANK) whose interrupt showed it, as the
    // filter stamped it: the moment the new frame starts to scan. Under the gate unless said otherwise:
    //   stPresentGap  present-to-present intervals under kGapUs (longer ones are idle screens or stalls: stGaps, not in the quantiles)
    //   stVb          the same intervals in whole frames (1, 2, 3, 4 or more DC A periods)
    //   stLatency     the compositor's swap submission (validateSwap) to that present
    //   taken         the times of the swap a kick took (noteTaken), until flip() files them under its surface (marks)
    //   stLatch       flips latched at the first frame end after the request, and later
    // From the filter (primary interrupt context, one CPU): feStamp, the last FRAME_END; the DC A frame period (fePeriod*, relaxed
    // atomics, reset by the stats line); stFeFolded, frame ends that came before the action took the previous one (the gate was
    // busy for a whole frame). stFeLate is filter to action, the work loop's delay at each frame end.
    static constexpr uint64_t kGapUs = 250000;
    FrameHist stPresentGap = {}, stLatency = {};
    uint32_t stVb[4] = {}, stGaps[2] = {}, stPresents = 0, stLatch[2] = {};
    uint64_t lastPresent = 0;
    Span stFeLate = {};
    uint64_t feStamp = 0, feLast = 0, fePeriodSum = 0, fePeriodMin = UINT64_MAX, fePeriodMax = 0;
    uint32_t fePeriodN = 0, stFeFolded = 0;
    // r6-lows (r6h): swaps validateSwap refused this window and the last refusal's reason (2: not one IOSurface in layer 0 alone,
    // a multi-layer or detached-layer swap; 3: geometry; 4: surface; see validateSwap), relaxed atomics: the client's swap path.
    // A frozen screen with CA still rendering shows here as ~300 a window and 0 swaps.
    uint32_t stRejected = 0, stRejectReason = 0;
    // r14-pacing: the second stats line, "IOSwitch display pacing". Each frame's own times travel with the surface it is drawn into
    // (several frames can be in flight: one asked for, one being copied), mach time: its swap's submission (validateSwap), its kick,
    // of the time between them the part spent waiting for a free surface, and its flip. At its present, swap-to-scan splits into
    // queue (submission to kick: behind the previous copy, or waiting for a surface), copy (kick to flip: the GPU work the copy is
    // ordered after, i.e. the compositor's own rendering of the frame, then the copy) and latch (flip to the boundary that took it).
    // Every present more than 1.5 frames after the previous one (and under kGapUs) is charged to one cause, with the frames it cost:
    //   idle   the compositor turned vsync off since the previous present: it had nothing to draw (no stall)
    //   ca     its swap came at or after the boundary a one-frame interval would have used: the compositor was late (its CPU
    //          render: m2v replay, encode, CPU contention)
    //   gpu    the swap came in time, but kick to flip alone outlasted the time it had left: the compositor's GPU work (or GPU
    //          contention) ended after that boundary
    //   queue  in time, but waiting behind the previous copy or for a surface made it miss
    //   latch  the flip was asked for in time but taken a boundary later
    // Also: frames replaced before they were shown (latest frame wins), swaps retired unshown because a newer one was queued
    // behind them (collapsed), V_BLANKs folded into one notification, notifications more than 1 ms after their V_BLANK, and how
    // often the compositor turned vsync off.
    struct FrameMark {
        uint64_t submit, kick, waited, ready;
    };
    FrameMark taken = {}, marks[kScanBuffers] = {};
    enum { kLongIdle, kLongCa, kLongGpu, kLongQueue, kLongLatch, kLongCauses };
    Span stPQueue = {}, stPWait = {}, stPCopy = {}, stPLatch = {};
    uint32_t stReplaced = 0, stCollapsed = 0, stLong[kLongCauses] = {}, stLongFrames[kLongCauses] = {};
    uint32_t stVsyncFolded = 0, stVsyncLateN = 0, stVsyncIdle = 0;  // stVsyncFolded: atomic (the filter)
    uint32_t stFallbacks = 0;  // recoverFlip: DC A did not take a replaced flip as modelled (once a boot at most)
    bool idleSincePresent = false;
    // Under the gate, from flip(): the frame just written into surface i goes out now.
    void markFlip(int i)
    {
        if (!stats || i < 0) return;
        marks[i] = taken;
        marks[i].ready = mach_absolute_time();
        taken = {};
    }
    // Under the gate: a flip latched at stamp (mach time); latchedAt is frameEnds - flipRequested (1: at the first frame end); mark
    // is the presented frame's own times (all 0 if not known).
    void notePresent(uint64_t stamp, uint64_t latchedAt, const FrameMark &mark)
    {
        if (stamp) noteMissedFrame(stamp);
        if (!stats || !stamp) return;
        ++stPresents;
        ++stLatch[latchedAt <= 1 ? 0 : 1];
        if (mark.submit && stamp > mark.submit) {
            stLatency.add(Span::ticksToUs(stamp - mark.submit));
            if (mark.kick >= mark.submit && mark.ready >= mark.kick && stamp >= mark.ready) {
                stPQueue.add(mark.kick - mark.submit);
                stPWait.add(mark.waited);
                stPCopy.add(mark.ready - mark.kick);
                stPLatch.add(stamp - mark.ready);
            }
        }
        if (lastPresent && stamp > lastPresent) {
            const uint64_t us = Span::ticksToUs(stamp - lastPresent);
            if (us >= kGapUs)
                ++stGaps[us < 1000000 ? 0 : 1];
            else {
                stPresentGap.add(us);
                const uint64_t period = vsyncInterval ? Span::ticksToUs(vsyncInterval) : 16667;
                const uint64_t frames = (us + period / 2) / period;
                ++stVb[frames <= 1 ? 0 : frames >= 4 ? 3 : frames - 1];
                if (frames >= 2) noteLong((uint32_t)frames, mark);
            }
        }
        lastPresent = stamp;
        idleSincePresent = false;
    }
    // A present frames DC A periods after the previous one (lastPresent, not yet moved on): its cause, see above.
    void noteLong(uint32_t frames, const FrameMark &mark)
    {
        uint64_t period = vsyncInterval;
        if (!period) nanoseconds_to_absolutetime(16666667ull, &period);
        const uint64_t due = lastPresent + period;  // the boundary a one-frame interval would have used
        unsigned cause;
        if (!mark.submit || mark.submit >= due)
            cause = idleSincePresent ? kLongIdle : kLongCa;
        else if (mark.ready > due)
            cause = mark.ready - mark.kick > due - mark.submit ? kLongGpu : kLongQueue;
        else
            cause = kLongLatch;
        ++stLong[cause];
        stLongFrames[cause] += frames - 1;
    }
    // A present two or more refresh periods after the one before it, right after one that came a single period
    // after its own (a full-rate animation broke cadence; a steady 30 Hz client is not a miss), and while frames keep
    // coming (gaps under kGapUs; longer ones are a screen that went still): the CPU and GPU governors go to their top
    // points for a while (SwitchPower.h). Independent of ioswitch-display-stats; its own stamps, under the gate.
    uint64_t missLast = 0, missFrames = 0;
    void noteMissedFrame(uint64_t stamp)
    {
        const uint64_t last = missLast, before = missFrames;
        missLast = stamp;
        missFrames = 0;
        if (!last || stamp <= last) return;
        const uint64_t us = Span::ticksToUs(stamp - last);
        if (us >= kGapUs) return;
        const uint64_t period = vsyncInterval ? Span::ticksToUs(vsyncInterval) : 16667;
        missFrames = (us + period / 2) / period;
        if (missFrames >= 2 && before == 1) switch_perf_hint(SWITCH_PERF_HINT_FRAME_MISS);
    }
    bool vsyncWanted() const
    {
        const void *head = nullptr;
        memcpy(&head, reinterpret_cast<const unsigned char *>(this) + 5592, sizeof(head));
        return head;
    }
    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync()
    {
        if (!vsyncTimer || vsyncArmed || !vsyncWanted()) return;
        vsyncArmed = true;
        if (vblankSource) {
            // A V_BLANK latched while it was masked is stale: drop it, then let the next one through.
            __atomic_fetch_and(&dcPending, ~kVblankInt, __ATOMIC_RELAXED);
            // Powered down for SC7: the resume's scanout restart unmasks V_BLANK for an armed tick.
            if (!hwSuspended) {
                dc[kDcIntStatus] = kVblankInt;
                dc[kDcIntMask] = dc[kDcIntMask] | kVblankInt;
            }
            native_legacy_vsync_observed(0, vsyncHz);
            return;
        }
        vsyncNext = mach_absolute_time() + vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);
        native_legacy_vsync_observed(0, vsyncHz);
    }
    static void vsyncFired(OSObject *owner, IOTimerEventSource *)
    {
        static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();
    }
    void vsyncTick()
    {
        if (vblankSource) {
            // A polled panel command that overlapped the start may have restored INT_ENABLE without our bits; it is over by now.
            if (frameEnds) {
                dc[kDcIntEnable] = dc[kDcIntEnable] | kVblankInt | kFrameEndInt;
                return;
            }
            char gic[192];
            ioswitch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
            panic("IOSwitch display: no DC A frame end in 250 ms: INT_STATUS %#x MASK %#x ENABLE %#x, %s",
                  dc[kDcIntStatus], dc[kDcIntMask], dc[kDcIntEnable], gic);
        }
        vsyncArmed = false;
        if (!vsyncTimer) return;
        if (!vsyncWanted()) {
            native_legacy_vsync_observed(2, vsyncHz);
            noteVsyncIdle();
            return;
        }
        const uint64_t now = mach_absolute_time();
        native_legacy_vsync_notify(this, now, mach_continuous_time(), vsyncInterval);
        native_legacy_vsync_observed(1, vsyncHz);
        vsyncNext += vsyncInterval;
        if (vsyncNext <= now) {
            vsyncNext = now + vsyncInterval;
            native_legacy_vsync_observed(3, vsyncHz);
        }
        vsyncArmed = true;
        vsyncTimer->wakeAtTime(vsyncNext);
    }
    // Primary interrupt context.
    static bool vblankFilter(OSObject *owner, IOFilterInterruptEventSource *)
    {
        NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
        const uint32_t status = self->dc[kDcIntStatus] & (kVblankInt | kFrameEndInt);
        if (!status) return false;
        self->dc[kDcIntStatus] = status;
        (void)self->dc[kDcIntStatus];  // read back: the line is low before ArmCpu's EOI
        if (gPanicRestore.panicking)
            return false;  // window A is the panic's (restorePitchOnPanic): no flip, no DSI command
        if (status & kVblankInt) {
            self->vblankTime = mach_absolute_time();
            self->vblankContinuous = mach_continuous_time();
        }
        // The panel's DSI command steps, timed from the frame end as hekate does (the line is already low; the last step takes ~620 us).
        // A console waiting to scroll (waitTapeFrame) wakes at every frame end while it draws on the tape.
        if (status & kFrameEndInt) {
            tegra_panel_frame_end_filter();
            if (self->tapeAttached) {
                __atomic_fetch_add(&self->tapeFrames, 1ull, __ATOMIC_RELEASE);
                thread_wakeup((event_t)&self->tapeFrames);
            }
            if (self->stats) {
                // r6-lows: the frame end's time (the present stamp), the DC A period, and a frame end the action has not taken yet.
                const uint64_t now = mach_absolute_time(), last = self->feLast;
                __atomic_store_n(&self->feStamp, now, __ATOMIC_RELEASE);
                self->feLast = now;
                // Only consecutive frame ends (a stopped scanout or SC7 in between is no period).
                if (last && now > last && (!self->vsyncInterval || now - last < 2 * self->vsyncInterval)) {
                    const uint64_t period = now - last;
                    __atomic_fetch_add(&self->fePeriodSum, period, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&self->fePeriodN, 1u, __ATOMIC_RELAXED);
                    if (period < __atomic_load_n(&self->fePeriodMin, __ATOMIC_RELAXED))
                        __atomic_store_n(&self->fePeriodMin, period, __ATOMIC_RELAXED);
                    if (period > __atomic_load_n(&self->fePeriodMax, __ATOMIC_RELAXED))
                        __atomic_store_n(&self->fePeriodMax, period, __ATOMIC_RELAXED);
                }
                if (__atomic_load_n(&self->dcPending, __ATOMIC_ACQUIRE) & kFrameEndInt)
                    __atomic_fetch_add(&self->stFeFolded, 1u, __ATOMIC_RELAXED);
            }
        }
        // Two V_BLANKs before the action runs (the gate busy, e.g. with a copy) fold into one notification; the stats count both.
        if ((status & kVblankInt) && self->stats && self->vsyncArmed) {
            __atomic_fetch_add(&self->stVblanks, 1u, __ATOMIC_RELAXED);
            if (__atomic_load_n(&self->dcPending, __ATOMIC_ACQUIRE) & kVblankInt)
                __atomic_fetch_add(&self->stVsyncFolded, 1u, __ATOMIC_RELAXED);
        }
        __atomic_fetch_or(&self->dcPending, status, __ATOMIC_RELEASE);
        return true;
    }
    static void vblankFired(OSObject *owner, IOInterruptEventSource *, int)
    {
        static_cast<NativeLegacyDisplay *>(owner)->vblankTick();
    }
    void vblankTick()
    {
        const uint32_t status = __atomic_exchange_n(&dcPending, 0u, __ATOMIC_ACQUIRE);
        if (hwSuspended) return;  // a status taken just before the SC7 power-down: the registers are unclocked now
        if (status & kFrameEndInt) frameEnds = frameEnds + 1;
        if ((status & kFrameEndInt) && stats) {
            const uint64_t stamp = __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE), now = mach_absolute_time();
            if (stamp && now > stamp) stFeLate.add(now - stamp);
        }
        if (status & kVblankInt) vblankNotify();
        watchVsync(status);
        // After the compositor's vsync, since a latched flip starts the next frame's copy right here. A flip is also looked for at
        // V_BLANK (unmasked while a vsync client is registered), in case the controller latches after its FRAME_END.
        if ((status & kFrameEndInt) && rotated)
            checkRotation();
        else if ((status & kVblankInt) && rotationLatched && ring.pending != kNoBuffer)
            checkFlip(false);
        if ((status & kFrameEndInt) && tapeSettle) checkTapeSettled();
        if (status & kFrameEndInt) probeFrameEnd();
        if ((status & kFrameEndInt) && stats) noteStatsWindow();
    }
    // At a frame end, under the gate: after 5 s, the one stats line for the window, then a new window. The line is formatted here and
    // logged as one string: the kernel's os_log encodes scalar arguments into a 256-byte buffer (libkern/os/log.h
    // OS_LOG_BUFFER_MAX_SIZE) and drops a message whose arguments do not fit (log_encode.c, ENOMEM), which is what became of this
    // line's 33 arguments on 10-31; a string argument grows into the 1 KiB log memory instead.
    void noteStatsWindow()
    {
        const uint64_t now = mach_absolute_time();
        uint64_t window = 0;
        nanoseconds_to_absolutetime(5000000000ULL, &window);
        if (!statsStart || now - statsStart < window) {
            if (!statsStart) statsStart = now;
            return;
        }
        const uint64_t us = Span::ticksToUs(now - statsStart);
        const uint32_t vblanks = __atomic_exchange_n(&stVblanks, 0u, __ATOMIC_RELAXED);
        auto rate = [us](uint32_t n) {
            return (unsigned long long)((uint64_t)n * 10000000ULL / (us ? us : 1));
        };  // per second, in tenths
        char line[960];
        size_t at = 0;
        auto add = [&](const char *format, auto... values) {
            if (at < sizeof(line)) at += (size_t)snprintf(line + at, sizeof(line) - at, format, values...);
        };
        auto span = [&](const char *name, const Span &value) {
            add(" %s %llu/%llu", name, (unsigned long long)value.meanUs(), (unsigned long long)value.maxUs());
        };
        add("%llu ms: %u swaps %llu.%llu/s,", (unsigned long long)(us / 1000), stSwaps, rate(stSwaps) / 10,
            rate(stSwaps) % 10);
        span("interval", stInterval);
        span("client", stClient);
        span("submit-retire", stRetire);
        span("(queue", stQueue);
        span("flip-wait", stFlipWait);
        span("map", stMap);
        span("gpu", stGpu);
        span("cache", stCache);
        span("scan", stScan);
        span("copy", stCopy);
        span("gpu-copy", stGpuCopy);
        add(" read %llu", (unsigned long long)Span::ticksToUs(stRead));
        add(") us mean/max; copy fast %u byte %u gpu %u (declined %u); vsync %u sent %llu.%llu/s of %u vblanks,",
            stFast, stByte, stGpuCopies, stGpuDeclined, stVsyncs, rate(stVsyncs) / 10, rate(stVsyncs) % 10, vblanks);
        span("late", stVsyncLate);
        add(" us; %u gpu timeouts, %u new maps, %u underflow frames; attr source %#x target %#x", stGpuTimeouts,
            stNewMaps, stUnderflows, stSourceAttr,
            hwRotate ? mairAttribute(reinterpret_cast<const void *>(drawSurface()->getVirtualAddress())) : 0x100u);
        // r6-lows frame pacing: presents (latched flips) and their intervals under 250 ms as quantiles, in whole DC A frames, and
        // the counts over 1.5x and 2x the period (2+ and 3+ frames: presents are whole frames apart); idle or stalled gaps of
        // 250 ms-1 s and over 1 s; swap submission to present; the measured DC A period; frame-end work-loop delay, frame ends
        // folded into one action, swaps that waited for a flip, flips latched at the first frame end or later.
        auto quantiles = [&](const char *name, const FrameHist &h) {
            const uint64_t top = h.maxUs;
            add(" %s p50 %llu p99 %llu p99.9 %llu max %llu.%llu ms", name,
                (unsigned long long)(h.quantileUs(500) / 1000), (unsigned long long)(h.quantileUs(990) / 1000),
                (unsigned long long)(h.quantileUs(999) / 1000), (unsigned long long)(top / 1000),
                (unsigned long long)(top % 1000 / 100));
        };
        add("; present %u (%u timed):", stPresents, stPresentGap.count);
        quantiles("interval", stPresentGap);
        add(", frames 1:%u 2:%u 3:%u 4+:%u, >1.5x %u >2x %u, gaps %u/%u;", stVb[0], stVb[1], stVb[2], stVb[3],
            stVb[1] + stVb[2] + stVb[3], stVb[2] + stVb[3], stGaps[0], stGaps[1]);
        quantiles("swap-to-scan", stLatency);
        {
            const uint64_t sum = __atomic_exchange_n(&fePeriodSum, 0ull, __ATOMIC_RELAXED);
            const uint32_t n = __atomic_exchange_n(&fePeriodN, 0u, __ATOMIC_RELAXED);
            const uint64_t low = __atomic_exchange_n(&fePeriodMin, UINT64_MAX, __ATOMIC_RELAXED),
                           high = __atomic_exchange_n(&fePeriodMax, 0ull, __ATOMIC_RELAXED);
            uint64_t meanNs = 0;
            if (n) absolutetime_to_nanoseconds(sum / n, &meanNs);
            add("; frame %llu.%03llu ms (%llu-%llu us, %u)", (unsigned long long)(meanNs / 1000000),
                (unsigned long long)(meanNs / 1000 % 1000), (unsigned long long)(n ? Span::ticksToUs(low) : 0),
                (unsigned long long)Span::ticksToUs(high), n);
        }
        add(", fe-late %llu/%llu us, folded %u, flip-waited %u, latch 1st %u later %u",
            (unsigned long long)stFeLate.meanUs(), (unsigned long long)stFeLate.maxUs(),
            __atomic_exchange_n(&stFeFolded, 0u, __ATOMIC_RELAXED), stFlipWait.count, stLatch[0], stLatch[1]);
        add(", rejected %u (reason %u)", __atomic_exchange_n(&stRejected, 0u, __ATOMIC_RELAXED),
            __atomic_load_n(&stRejectReason, __ATOMIC_RELAXED));
        os_log(OS_LOG_DEFAULT, "IOSwitch display stats: %s", line);  // log only, never the boot console
        // r14-pacing: swap-to-scan by stage, long presents by cause (presents/frames lost), vsync delivery (see FrameMark).
        at = 0;
        add("%u presents: swap-to-scan = queue %llu/%llu (surface wait %llu/%llu) + copy %llu/%llu + latch %llu/%llu "
            "us "
            "mean/max",
            stPresents, (unsigned long long)stPQueue.meanUs(), (unsigned long long)stPQueue.maxUs(),
            (unsigned long long)stPWait.meanUs(), (unsigned long long)stPWait.maxUs(),
            (unsigned long long)stPCopy.meanUs(), (unsigned long long)stPCopy.maxUs(),
            (unsigned long long)stPLatch.meanUs(), (unsigned long long)stPLatch.maxUs());
        add("; %u buffers%s, replaced %u, collapsed %u", doubleBuffer ? ring.buffers : 1u,
            !doubleBuffer || ring.replace ? ""
            : stFallbacks                 ? " (fell back to double buffering now)"
                                          : " (double-buffered)",
            stReplaced, stCollapsed);
        add("; long %u: idle %u/%u ca %u/%u gpu %u/%u queue %u/%u latch %u/%u",
            stLong[kLongIdle] + stLong[kLongCa] + stLong[kLongGpu] + stLong[kLongQueue] + stLong[kLongLatch],
            stLong[kLongIdle], stLongFrames[kLongIdle], stLong[kLongCa], stLongFrames[kLongCa], stLong[kLongGpu],
            stLongFrames[kLongGpu], stLong[kLongQueue], stLongFrames[kLongQueue], stLong[kLongLatch],
            stLongFrames[kLongLatch]);
        add("; vsync folded %u late>1ms %u idle %u", __atomic_exchange_n(&stVsyncFolded, 0u, __ATOMIC_RELAXED),
            stVsyncLateN, stVsyncIdle);
        os_log(OS_LOG_DEFAULT, "IOSwitch display pacing: %s", line);  // log only, never the boot console
        stPQueue = stPWait = stPCopy = stPLatch = Span{};
        stReplaced = stCollapsed = stVsyncLateN = stVsyncIdle = stFallbacks = 0;
        for (auto &n : stLong) n = 0;
        for (auto &n : stLongFrames) n = 0;
        stPresentGap = stLatency = FrameHist{};
        stFeLate = Span{};
        for (auto &n : stVb) n = 0;
        stGaps[0] = stGaps[1] = stPresents = stLatch[0] = stLatch[1] = 0;
        stQueue = stFlipWait = stMap = stGpu = stCache = stScan = stCopy = stRetire = stInterval = stClient =
            stVsyncLate = stGpuCopy = Span{};
        stSwaps = stVsyncs = stGpuTimeouts = stNewMaps = stUnderflows = stFast = stByte = stGpuCopies = stGpuDeclined =
            0;
        stSourceAttr = 0x100;
        statsStart = now;
        stReadDue = true;
        stRead = 0;
    }
    void vblankNotify()
    {
        if (!vsyncArmed) return;
        if (!vsyncWanted()) {
            dc[kDcIntMask] = dc[kDcIntMask] & ~kVblankInt;
            vsyncArmed = false;
            native_legacy_vsync_observed(2, vsyncHz);
            noteVsyncIdle();
            return;
        }
        // The panel's own period between consecutive vblanks; the nominal one after a pause.
        const uint64_t stamp = vblankTime;
        const uint64_t interval = vblankLast && stamp > vblankLast && stamp - vblankLast < 4 * vsyncInterval
                                      ? stamp - vblankLast
                                      : vsyncInterval;
        vblankLast = stamp;
        native_legacy_vsync_notify(this, stamp, vblankContinuous, interval);
        native_legacy_vsync_observed(1, vsyncHz);
        if (stats) {
            ++stVsyncs;
            const uint64_t late = mach_absolute_time() - stamp;
            stVsyncLate.add(late);
            if (Span::ticksToUs(late) > 1000) ++stVsyncLateN;
        }
    }
    // The compositor has no vsync client any more: it has nothing to draw until it registers again.
    void noteVsyncIdle()
    {
        if (!stats) return;
        ++stVsyncIdle;
        idleSincePresent = true;
    }
    // Every DC A action, under the gate. The render server is a vsync client (kern_exec.c gives backboardd CA_FORCE_TIMER=1, so
    // QuartzCore's IOMFBServer::set_vsync_enabled takes IOMobileFramebufferEnableVSyncNotifications instead of its CADisplayTimer)
    // and draws nothing until its first vsync: its registration is found here, at the first frame end after it, whether or not it
    // went through kickSwapQueue (no swap comes to do it). Armed, V_BLANK must come every frame: kVsyncSilentFrames frame ends
    // without one panic, since the compositor would wait for ever.
    void watchVsync(uint32_t status)
    {
        if (status & kVblankInt) vsyncSilent = 0;
        if (!(status & kFrameEndInt)) return;
        if (!vsyncArmed) {
            vsyncSilent = 0;
            armVsync();
            return;
        }
        if (++vsyncSilent < kVsyncSilentFrames) return;
        char gic[192];
        ioswitch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
        panic("IOSwitch display: no DC A V_BLANK in %u frame ends with vsync armed: INT_STATUS %#x MASK %#x ENABLE "
              "%#x, %s",
              vsyncSilent, dc[kDcIntStatus], dc[kDcIntMask], dc[kDcIntEnable], gic);
    }
    static bool displayClocked()
    {
        IODeviceMemory *memory = IODeviceMemory::withRange(kCarPA, 0x1000);
        IOMemoryMap *car = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        if (!car) return false;
        const volatile uint32_t *regs = reinterpret_cast<const volatile uint32_t *>(car->getVirtualAddress());
        const bool running = (regs[kCarClockL / 4] & kCarDisp1) && !(regs[kCarResetL / 4] & kCarDisp1);
        car->release();
        return running;
    }
    bool startVblank(IOWorkLoop *loop)
    {
        unsigned enabled = 1;
        PE_parse_boot_argn("ioswitch-vblank", &enabled, sizeof(enabled));
        if (!enabled) return false;
        if (!displayClocked()) panic("IOSwitch display: DC A clock off or in reset, no vblank interrupt");
        IOService *nub = ioswitch_interrupt_nub(kDisplayAInterrupt, false);
        IODeviceMemory *memory = IODeviceMemory::withRange(kDisplayAPA, kDcMapBytes);
        dcMap = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        if (!dcMap) panic("IOSwitch display: cannot map DC A");
        dc = reinterpret_cast<volatile uint32_t *>(dcMap->getVirtualAddress());
        vblankSource = IOFilterInterruptEventSource::filterInterruptEventSource(
            this, &NativeLegacyDisplay::vblankFired, &NativeLegacyDisplay::vblankFilter, nub, 0);
        if (!vblankSource || loop->addEventSource(vblankSource) != kIOReturnSuccess)
            panic("IOSwitch display: cannot attach the DC A interrupt (INTID %u)", kDisplayAInterrupt);
        dcEnable = dc[kDcIntEnable];
        dcMask = dc[kDcIntMask];
        dc[kDcIntStatus] = kVblankInt | kFrameEndInt;
        dc[kDcIntEnable] = dcEnable | kVblankInt | kFrameEndInt;
        dc[kDcIntMask] = (dcMask & ~kVblankInt) | kFrameEndInt;
        vblankSource->enable();
        vsyncTimer->setTimeoutMS(250);
        IOLog("IOSwitch display: vsync from the DC A vblank interrupt (INTID %u)\n", kDisplayAInterrupt);
        return true;
    }
    // Under the work-loop gate.
    void stopVblank()
    {
        if (!vblankSource) return;
        if (hwSuspended) panic("IOSwitch display: stopped while powered down for SC7 (no tegra_display_resume)");
        stopRotation();
        vblankSource->disable();
        if (IOWorkLoop *loop = getWorkLoop()) loop->removeEventSource(vblankSource);
        OSSafeReleaseNULL(vblankSource);
        dc[kDcIntMask] = dcMask;
        dc[kDcIntEnable] = dcEnable;
        dc[kDcIntStatus] = kVblankInt | kFrameEndInt;
        dc = nullptr;
        OSSafeReleaseNULL(dcMap);
        dcPending = 0;
    }
    void startVsync()
    {
        unsigned hz = 0;
        PE_parse_boot_argn("sheng-vsync", &hz, sizeof(hz));
        if (!hz || hz > 240 || !primary || !bootMapping || vsyncTimer) return;
        IOWorkLoop *loop = getWorkLoop();
        if (!loop) return;
        IOTimerEventSource *timer = IOTimerEventSource::timerEventSource(this, &NativeLegacyDisplay::vsyncFired);
        if (!timer) return;
        if (loop->addEventSource(timer) != kIOReturnSuccess) {
            timer->release();
            return;
        }
        nanoseconds_to_absolutetime(1000000000ull / hz, &vsyncInterval);
        vsyncHz = hz;
        vsyncTimer = timer;
        // A client may already have registered: arm under the gate, like the registration does. The vblank interrupt starts under the
        // gate too, so no registration slips in between.
        loop->runAction(
            [](OSObject *owner, void *a0, void *, void *, void *) -> IOReturn {
                NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
                if (!self->startVblank(static_cast<IOWorkLoop *>(a0))) native_legacy_vsync_observed(4, self->vsyncHz);
                self->armVsync();
                return kIOReturnSuccess;
            },
            this, loop);
    }
    void stopVsync()
    {
        if (IOWorkLoop *loop = vblankSource ? getWorkLoop() : nullptr) {
            loop->runAction(
                [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                    static_cast<NativeLegacyDisplay *>(owner)->stopVblank();
                    return kIOReturnSuccess;
                },
                this);
        }
        if (!vsyncTimer) return;
        vsyncTimer->cancelTimeout();
        if (IOWorkLoop *loop = getWorkLoop()) loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);
        vsyncArmed = false;
    }
    // After startVsync. The rotation is only for the Switch's rotated panel (v_rotate 3, the only rotated mode
    // native_legacy_video_mode accepts); an unrotated firmware framebuffer has nothing to turn.
    void startHardwareRotation()
    {
        unsigned enabled = 1;
        PE_parse_boot_argn("ioswitch-hw-rotate", &enabled, sizeof(enabled));
        unsigned twin = 0;
        PE_parse_boot_argn("ioswitch-scanout-double", &twin, sizeof(twin));
        PE_Video video = {};
        if (enabled != 1 || !primary || !bootMapping || PE_current_console(&video) || video.v_rotate != 3) {
            if (twin == 1 && primary)
                IOLog("IOSwitch display: ioswitch-scanout-double=1 ignored: it needs the hardware rotation "
                      "(ioswitch-hw-rotate)\n");
            return;
        }
        unsigned gpu = 0;
        PE_parse_boot_argn("sheng-fb-gpu", &gpu, sizeof(gpu));
        if (gpu == 1)
            panic("IOSwitch display: ioswitch-hw-rotate=1 with sheng-fb-gpu=1: the GPU presenter writes the pitch "
                  "layout");
        if (!vblankSource)
            panic("IOSwitch display: ioswitch-hw-rotate=1 needs DC A frame ends to confirm the latch (sheng-vsync or "
                  "ioswitch-vblank off)");
        scanoutWC = bootFlag("ioswitch-scanout-wc");
        blockMapping = mapScanout(bootMapping->getPhysicalAddress());
        if (!blockMapping || !blockMapping->getVirtualAddress() || blockMapping->getLength() < kNativeBlockBytes)
            panic("IOSwitch display: cannot map the %#llx-byte block-linear scanout at %#llx", kNativeBlockBytes,
                  (uint64_t)bootMapping->getPhysicalAddress());
        hwRotate = true;
        if (twin == 1) startDoubleBuffer();
        // The memory type each CPU view of the scanout really got (MAIR byte: 0x44 write-combined, 0x00/0x04 device): the copy's
        // store rate depends on it.
        IOLog("IOSwitch display: hardware rotation armed: block-linear %ux%u at %#llx%s, window A switches at the "
              "first client frame; "
              "memory type %#x (pitch view %#x%s)\n",
              kNativeBlockWidth, kNativeBlockHeight, (uint64_t)bootMapping->getPhysicalAddress(),
              !doubleBuffer       ? ""
              : ring.buffers == 3 ? " and two more buffers (triple-buffered, latest frame wins)"
                                  : " and a second buffer (double-buffered)",
              mairAttribute(reinterpret_cast<const void *>(blockMapping->getVirtualAddress())),
              mairAttribute(reinterpret_cast<const void *>(bootMapping->getVirtualAddress())),
              scanoutWC ? ", ioswitch-scanout-wc" : "");
    }
    // A kernel view of a block-linear scanout surface (kNativeBlockBytes at physical address pa), write-combined. As before, the cache
    // mode is asked for at map time on a plain device range. With ioswitch-scanout-wc=1 it is also fixed when the descriptor is made:
    // for a physical descriptor IOKit takes the memory type from the descriptor's own cache mode (the default, for RAM the kernel does
    // not manage, is device memory), and the GPU driver found a map-time kIOMapWriteCombineCache alone left its mappings device
    // memory (TegraGM20BUserClient.cpp kPoolCacheMode). Device memory takes every 16-byte store as its own bus write.
    IOMemoryMap *mapScanout(uint64_t pa, uint64_t bytes = kNativeBlockBytes) const
    {
        IOMemoryDescriptor *memory = nullptr;
        if (scanoutWC) {
            IOAddressRange range = {pa, bytes};
            memory = IOMemoryDescriptor::withAddressRanges(
                &range, 1,
                IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryTypePhysical64) |
                    (IOOptionBits(kIOWriteCombineCache) << kIOMemoryBufferCacheShift),
                TASK_NULL);
        } else
            memory = IODeviceMemory::withRange(pa, bytes);
        IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
        OSSafeReleaseNULL(memory);
        return map;
    }
    // At start, while 3.75 MiB runs of contiguous pages are easy to find. The DC has no SMMU on this boot chain and START_ADDR_HI
    // stays 0, so each buffer is physically contiguous below 4 GiB; 128 KiB aligned (the mask's low zero bits), as a Maxwell big
    // page. Surface 1 is required (as before); without surface 2 the display double-buffers, said in one line.
    bool startScanBuffer(unsigned i, bool required)
    {
        const IOOptionBits options = IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous) |
                                     IOOptionBits(kIOMemoryMapperNone);
        IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, options, kNativeBlockBytes, 0x00000000fffe0000ULL);
        IOByteCount run = 0;
        const uint64_t pa = buffer ? buffer->getPhysicalSegment(0, &run, kIOMemoryMapperNone) : 0;
        IOMemoryMap *mapping = nullptr;
        if (pa && run >= kNativeBlockBytes && !(pa & 0x1ffff) && pa + kNativeBlockBytes <= (1ull << 32) &&
            buffer->getBytesNoCopy()) {
            // The allocator zeroed the pages through the kernel's cacheable alias. Write those lines back and drop them now,
            // before the write-combined mapping writes the same memory around the cache; nothing writes through the cacheable
            // alias after this, so no dirty line can later land on a frame.
            cleanInvalidate(buffer->getBytesNoCopy(), kNativeBlockBytes);
            mapping = mapScanout(pa);
            if (mapping && (!mapping->getVirtualAddress() || mapping->getLength() < kNativeBlockBytes))
                OSSafeReleaseNULL(mapping);
        }
        if (!mapping) {
            if (required)
                panic("IOSwitch display: ioswitch-scanout-double=1: no mapped, physically contiguous %#llx-byte buffer "
                      "below 4 GiB for surface %u (got %#llx, %#llx contiguous)",
                      kNativeBlockBytes, i, pa, (uint64_t)run);
            IOLog("IOSwitch display: no physically contiguous %#llx-byte buffer below 4 GiB for surface %u (got %#llx, "
                  "%#llx contiguous): double-buffered, a late frame delays the ones after it\n",
                  kNativeBlockBytes, i, pa, (uint64_t)run);
            OSSafeReleaseNULL(buffer);
            return false;
        }
        scanBuffers[i] = buffer;
        scanMappings[i] = mapping;
        scanAddresses[i] = pa;
        return true;
    }
    void startDoubleBuffer()
    {
        startScanBuffer(1, true);
        const unsigned buffers = startScanBuffer(2, false) ? 3 : 2;
        flipTimer = IOTimerEventSource::timerEventSource(this, &NativeLegacyDisplay::flipDeadline);
        if (!flipTimer || getWorkLoop()->addEventSource(flipTimer) != kIOReturnSuccess)
            panic("IOSwitch display: ioswitch-scanout-double=1: cannot add the flip deadline timer");
        doubleBuffer = true;
        ring.reset(buffers);
        // The SMMU probe's page tables (directory + tables), physically contiguous below 4 GiB; none: no probe.
        probeTables = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous),
            (uint64_t)kProbeTables * 4096, 0x00000000fffff000ULL);
        IOByteCount run = 0;
        probeTablesPa = probeTables ? probeTables->getPhysicalSegment(0, &run, kIOMemoryMapperNone) : 0;
        if (!probeTablesPa || run < (uint64_t)kProbeTables * 4096 || probeTablesPa + run > (1ull << 32) ||
            !probeTables->getBytesNoCopy()) {
            IOLog("IOSwitch display: no contiguous %u KiB for the SMMU probe's tables: probe off\n", kProbeTables * 4);
            OSSafeReleaseNULL(probeTables);
        }
        kickDeferred = false;
    }
    // XNU's text console scrolls by window A's start address (video_console.c vc_tape) from here until the first client frame
    // scans. Its text lines are 16-pixel columns of the portrait scanout, 64 bytes of every memory row, so moving the start address
    // 64 bytes scrolls the whole screen one line, and DC A takes that at a frame boundary like a flip (frame-counter latching,
    // REG_ACT_CONTROL, as startRotation sets it): no frame shows half a scroll, and no pixel is copied. The tape is hekate's pitch
    // window with kTapeGap spare lines after every row: a scroll clears its new line there, outside the scanned window, before the
    // window moves onto it, and the console stays within kTapeGap lines of the latched window (it sleeps in waitTapeFrame until the
    // next frame end otherwise). The first frame leaves the tape through startRotation, hence the hardware rotation; checkRotation
    // lets the tape go once that latched. Without it (no -v, no contiguous memory, a kernel without vc_tape) the console scrolls as
    // before.
    void startConsoleTape()
    {
        if (!hwRotate) return;
        checkPitchWindow();
        const IOOptionBits options = IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous) |
                                     IOOptionBits(kIOMemoryMapperNone);
        tapeBuffer =
            IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, kTapeBytes, 0x00000000fffff000ULL);
        IOByteCount run = 0;
        const uint64_t pa = tapeBuffer ? tapeBuffer->getPhysicalSegment(0, &run, kIOMemoryMapperNone) : 0;
        if (pa && run >= kTapeBytes && pa + kTapeBytes <= (1ull << 32) && tapeBuffer->getBytesNoCopy()) {
            // As for surface 1: the zeroing went through the cacheable alias, which nothing writes after this.
            cleanInvalidate(tapeBuffer->getBytesNoCopy(), kTapeBytes);
            tapeMapping = mapScanout(pa, kTapeBytes);
        }
        if (!tapeMapping || !tapeMapping->getVirtualAddress() || tapeMapping->getLength() < kTapeBytes) {
            IOLog("IOSwitch display: no physically contiguous %#llx-byte console tape below 4 GiB (got %#llx, %#llx "
                  "contiguous): "
                  "the console scrolls in software\n",
                  kTapeBytes, pa, (uint64_t)run);
            OSSafeReleaseNULL(tapeMapping);
            OSSafeReleaseNULL(tapeBuffer);
            return;
        }
        tapeAddress = (uint32_t)pa;
        tapeStrideSet = false;
        tapeAttached = true;
        dc[kDcRegActControl] = pitchActControl & ~kWinAActHcntrSel;
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        // XNU redraws its screen on the tape, then calls tapeShow(0), under its console lock.
        if (vc_tape_attach(tapeMapping->getVirtualAddress(), kTapeStride, kTapeGap, kTapeLines, &tapeShow, &tapeLatched,
                           &tapeWait, this)) {
            // Not drawing rotated text (no -v), or no vc_tape in this kernel: nothing was shown.
            tapeAttached = false;
            dc[kDcRegActControl] = pitchActControl;
            OSSafeReleaseNULL(tapeMapping);
            OSSafeReleaseNULL(tapeBuffer);
            return;
        }
        chainPanicHook();
        gPanicRestore.tape = true;
        IOLog("IOSwitch display: the console scrolls by window A's start address (%llu KiB tape at %#x, up to %u lines "
              "ahead of the scan)\n",
              kTapeBytes >> 10, tapeAddress, kTapeGap);
    }
    // XNU's console, under its lock, from any context, a panic included (stores only): window A to tape position line from the next
    // frame boundary. The console wrote the new line first; the barrier puts it in memory before the window can fetch it. The first
    // call also sets the tape's line stride, latched with it.
    static void tapeShow(void *owner, uint32_t line)
    {
        NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
        if (self->hwSuspended) return;  // unclocked for SC7
        volatile uint32_t *dc = self->dc;
        __asm__ volatile("dsb sy" ::: "memory");
        dc[kDcWindowHeader] = kWindowASelect;
        if (!self->tapeStrideSet) {
            dc[kWinLineStride] = (self->pitchStride & ~0xffffu) | kTapeStride;
            self->tapeStrideSet = true;
        }
        dc[kWinbufStartAddr] = self->tapeAddress + line * kTapeLineBytes;
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
    }
    // XNU's console, under its lock: DC A took the last tapeShow (its ACT_REQ is clear).
    static int tapeLatched(void *owner)
    {
        return !(static_cast<NativeLegacyDisplay *>(owner)->dc[kDcStateControl] & kWinAActReq);
    }
    static int tapeWait(void *owner) { return static_cast<NativeLegacyDisplay *>(owner)->waitTapeFrame(); }
    // XNU's console before a character, without its lock, in a thread that may block (vc_tape_pace): until the next DC A frame end.
    // 0 when none will come (display power off, SC7, a panic, the tape gone): the console scrolls on. Six frames without one panic,
    // naming the GIC state, as every lost DC A interrupt does.
    int waitTapeFrame()
    {
        if (scanoutStopped || hwSuspended || gPanicRestore.panicking || !tapeAttached) return 0;
        // No frame end can arrive while the display's interrupt action is pending or running: IOKit keeps the level-triggered
        // DC A line disabled from the filter until vblankFired returns, so a console printing from that action (its stats line)
        // or behind it would wait on itself (the 100 ms panic below, seen on hardware). Scroll without waiting then.
        if (__atomic_load_n(&dcPending, __ATOMIC_ACQUIRE)) return 0;
        if (IOWorkLoop *loop = getWorkLoop(); loop && loop->onThread()) return 0;
        const uint64_t seen = __atomic_load_n(&tapeFrames, __ATOMIC_ACQUIRE);
        uint64_t deadline;
        clock_interval_to_deadline(kTapeWaitMS, kMillisecondScale, &deadline);
        assert_wait_deadline((event_t)&tapeFrames, THREAD_UNINT, deadline);
        // Asserted before the count is read again: a frame end in between is not missed.
        if (__atomic_load_n(&tapeFrames, __ATOMIC_ACQUIRE) != seen) {
            clear_wait(current_thread(), THREAD_AWAKENED);
            return 1;
        }
        if (thread_block(THREAD_CONTINUE_NULL) != THREAD_TIMED_OUT ||
            __atomic_load_n(&tapeFrames, __ATOMIC_ACQUIRE) != seen)
            return 1;
        if (scanoutStopped || hwSuspended || gPanicRestore.panicking || !tapeAttached) return 0;
        char gic[192];
        ioswitch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
        panic("IOSwitch display: the console waited %u ms for a DC A frame end to scroll: INT_STATUS %#x MASK %#x "
              "ENABLE %#x, %s",
              kTapeWaitMS, dc[kDcIntStatus], dc[kDcIntMask], dc[kDcIntEnable], gic);
    }
    // Under the gate, once window A no longer scans the tape: the console (released at the first frame) leaves it, its pages go.
    void stopConsoleTape()
    {
        if (!tapeAttached) return;
        gPanicRestore.tape = false;
        __asm__ volatile("dmb ish" ::: "memory");
        vc_tape_detach();
        tapeAttached = false;
        tapeSettle = false;
        thread_wakeup((event_t)&tapeFrames);  // a console still waiting to scroll: there is no tape now
        OSSafeReleaseNULL(tapeMapping);
        OSSafeReleaseNULL(tapeBuffer);
    }
    // Under the gate, in a kick, right after native_legacy_prepare_scanout released XNU's console: its last scroll may still wait for
    // the frame boundary, and startRotation's several stores to window A must not straddle that latch. The kick waits for the frame
    // end that shows it taken (checkTapeSettled).
    bool tapeSettling()
    {
        if (!tapeAttached || rotated || !(dc[kDcStateControl] & kWinAActReq)) return false;
        if (!tapeSettle) {
            tapeSettle = true;
            tapeSettleRequested = frameEnds;
        }
        kickDeferred = true;
        return true;
    }
    void checkTapeSettled()
    {
        if (dc[kDcStateControl] & kWinAActReq) {
            if (frameEnds - tapeSettleRequested < kLatchFrames) return;
            panic("IOSwitch display: window A never latched the console's last scroll in %u frames: STATE_CONTROL %#x "
                  "ACT_CONTROL %#x START %#x",
                  kLatchFrames, dc[kDcStateControl], dc[kDcRegActControl], windowActive(kWinbufStartAddr));
        }
        tapeSettle = false;
        if (kickDeferred) {
            kickDeferred = false;
            kickSwapQueue();
        }
    }
    static void flipDeadline(OSObject *owner, IOTimerEventSource *)
    {
        static_cast<NativeLegacyDisplay *>(owner)->checkFlipDeadline();
    }
    // kFlipDeadlineMS after a flip, under the gate. With the scanout running, six frame ends have come by now and four settle a
    // flip (latched, or checkFlip panics): one still out means DC A's interrupt is lost or window A never latches. A frame end
    // taken by the filter but not yet handled (the gate was busy, e.g. with a copy) only moves the deadline.
    void checkFlipDeadline()
    {
        if ((ring.pending == kNoBuffer && !probeRequest) || hwSuspended || scanoutStopped || gPanicRestore.panicking)
            return;
        if (__atomic_load_n(&dcPending, __ATOMIC_ACQUIRE) & kFrameEndInt) {
            flipTimer->setTimeoutMS(kFlipDeadlineMS);
            return;
        }
        char gic[192];
        ioswitch_interrupt_describe(kDisplayAInterrupt, gic, sizeof(gic));
        panic("IOSwitch display: window A flip to %#x not settled in %u ms (%llu frame ends since): STATE_CONTROL %#x "
              "INT_STATUS %#x MASK %#x ENABLE %#x, %s",
              ring.pending != kNoBuffer ? surfaceAddress(ring.pending)
              : probe.onIova            ? (uint32_t)kProbeSlot
                                        : surfaceAddress(probeSurface),
              kFlipDeadlineMS, frameEnds - flipRequested, dc[kDcStateControl], dc[kDcIntStatus], dc[kDcIntMask],
              dc[kDcIntEnable], gic);
    }
    // Surface i's physical address (window A's START_ADDR); 0 is the boot framebuffer, also the pitch console's.
    uint32_t surfaceAddress(int i) const
    {
        return (uint32_t)(i > 0 ? scanAddresses[i] : bootMapping->getPhysicalAddress());
    }
    // The surface window A scans when its START_ADDR is start; kNoBuffer when none is.
    int surfaceIndex(uint32_t start) const
    {
        for (unsigned i = 0; i < (doubleBuffer ? ring.buffers : 1u); ++i)
            if (start == surfaceAddress((int)i)) return (int)i;
        return kNoBuffer;
    }
    IOMemoryMap *surfaceMapping(int i) const { return i > 0 ? scanMappings[i] : blockMapping; }
    // Where the next block-linear frame goes: a surface window A neither scans nor may take (ring.target(), the one being drawn
    // while a copy runs), or the only one. A caller that writes it has made sure there is one (kickOnce defers otherwise).
    IOMemoryMap *drawSurface() const
    {
        const int target = doubleBuffer ? ring.target() : 0;
        return surfaceMapping(target == kNoBuffer ? 0 : target);
    }
    // The START_ADDR window A should have once everything requested has latched.
    uint32_t expectedStart() const
    {
        return doubleBuffer ? surfaceAddress(ring.pending != kNoBuffer ? ring.pending : ring.front) : surfaceAddress(0);
    }
    // Under the gate, a frame complete in drawSurface() (the caller's copy or fill): point window A at it from the next frame. A flip
    // still out (not seen taken at a frame end) is replaced: DC A takes whatever START_ADDR its assembly copy holds at the boundary,
    // so the newer frame is the one that scans out there (TegraScanoutRing.h; checkFlip sorts out a boundary that beat this store).
    // Double-buffered after a fallback (recoverFlip), a frame finished while a flip is out is held and flipped at that flip's latch.
    void flip()
    {
        if (gPanicRestore.panicking) return;
        const int target = ring.target();
        if (target == kNoBuffer)
            panic("IOSwitch display: a frame to flip, but no scanout surface was free for it (front %d pending %d "
                  "replaced %d)",
                  ring.front, ring.pending, ring.superseded);
        const TegraScanoutRing::Ready how = ring.ready(target);
        markFlip(target);
        if (how != TegraScanoutRing::kHold) writeFlip(target);
    }
    // Under the gate: window A to surface i from the next frame boundary (START_ADDR, then the window and general requests).
    void writeFlip(int i)
    {
        __asm__ volatile("dsb sy" ::: "memory");  // the frame is in memory before the window can fetch it
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcWindowHeader] = kWindowASelect;
        dc[kWinbufStartAddr] = surfaceAddress(i);
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
        flipRequested = frameEnds;  // a replaced flip's boundary is this one's too
        flipTimer->setTimeoutMS(kFlipDeadlineMS);
    }
    // Under the gate, a block-linear frame complete in drawSurface(): the first one switches window A to the rotated layout (and,
    // double-buffered, to surface 1), later ones flip when double-buffered; single-buffered they are already on screen.
    void presentBlock()
    {
        if (gPanicRestore.panicking) return;
        if (!rotated && &ioswitch_boot_io_report) ioswitch_boot_io_report();  // once; later calls return at once
        if (!rotated && &ioswitch_console_hold && ioswitch_console_hold)
            return;  // the console keeps the screen (migration)
        if (!rotated)
            startRotation();
        else if (doubleBuffer)
            flip();
    }
    // A frame end (atFrameEnd) or vblank while a flip is out, under the gate. Which surface window A scans (its active START_ADDR)
    // says what the boundary took: the flip asked for, a flip that was replaced but beat the replacing store (the newer one is
    // then taken at the next boundary), or nothing yet. Each latch frees the surfaces no longer scanned.
    void checkFlip(bool atFrameEnd)
    {
        const uint32_t control = dc[kDcStateControl], start = windowActive(kWinbufStartAddr);
        const int asked = ring.pending, replaced = ring.superseded, scanned = ring.front;
        const uint64_t stamp = atFrameEnd ? __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE) : vblankTime;
        switch (ring.latch(surfaceIndex(start), control & (kGeneralActReq | kWinAActReq))) {
        case TegraScanoutRing::kWaiting:
            if (!atFrameEnd || frameEnds - flipRequested < kLatchFrames) return;
            panic("IOSwitch display: window A never latched the flip to %#x in %u frames: STATE_CONTROL %#x "
                  "ACT_CONTROL %#x START %#x",
                  surfaceAddress(asked), kLatchFrames, control, dc[kDcRegActControl], start);
        case TegraScanoutRing::kInconsistent:
            // Double-buffered already, this is the check the path had before r14: window A does not show the flip asked for.
            if (!ring.replace)
                panic(
                    "IOSwitch display: window A latched START_ADDR %#x, not the flip to %#x (front %#x; STATE_CONTROL "
                    "%#x)",
                    start, surfaceAddress(asked), surfaceAddress(scanned), control);
            recoverFlip(start, control, asked, replaced, scanned);
            return;
        case TegraScanoutRing::kTaken:
            if (stats) {
                if (replaced != kNoBuffer) ++stReplaced;  // that frame was never shown
                notePresent(stamp, frameEnds - flipRequested, marks[asked]);
            }
            break;
        case TegraScanoutRing::kTakenReplaced:
            if (stats)
                notePresent(stamp, 1, marks[replaced]);  // shown for a frame after all; the newer one is still out
            break;
        }
        flipLatched();
    }
    // Under the gate, at the first frame end whose window A START_ADDR none of the asked-for flips explains while flips are being
    // replaced: DC A does not take a replaced START_ADDR as TegraScanoutRing.h models it. One line, then double buffering for the
    // rest of the boot, as before r14: the buffer DC A scans becomes the front, the newest complete frame is asked for again (no
    // request is out now, so it is a plain flip), a replaced buffer it does not scan stays untouched until a frame end shows a plain
    // latch, and a copy in flight finishes into its own buffer and waits for that latch. A mismatch after this panics as it did.
    void recoverFlip(uint32_t start, uint32_t control, int asked, int replaced, int scanned)
    {
        IOLog("IOSwitch display: window A scans %#x after flips to %#x (replaced %#x, front %#x; STATE_CONTROL %#x), "
              "not as "
              "modelled: double-buffered from here (the r7q path)\n",
              start, surfaceAddress(asked), replaced == kNoBuffer ? 0u : surfaceAddress(replaced),
              surfaceAddress(scanned), control);
        const int again = ring.recover(surfaceIndex(start));
        if (again != kNoBuffer) writeFlip(again);
        if (stats) ++stFallbacks;
    }
    // --- The SMMU probe (DisplaySmmuProbe.h), under the gate ---------------------------------------------------------
    // Its I/O, as DisplaySmmuProbe::perform drives it.
    struct ProbeIo {
        NativeLegacyDisplay *d;
        void tweakPixel() { d->probeWritePixel(d->probePixel ^ 0x00010101u); }
        void restorePixel() { d->probeWritePixel(d->probePixel); }
        bool attach() { return d->probeAttach(); }
        void scan(bool iova)
        {
            volatile uint32_t *regs = d->dc;
            regs[kDcStateAccess] = regs[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
            regs[kDcWindowHeader] = kWindowASelect;
            regs[kWinbufStartAddr] = iova ? (uint32_t)kProbeSlot : d->surfaceAddress(d->probeSurface);
            regs[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
            regs[kDcStateControl] = kGeneralActReq | kWinAActReq;
            // Out until a frame end shows it taken (probeFrameEnd); a lost DC A interrupt panics at the flip deadline.
            d->probeRequest = true;
            d->flipRequested = d->frameEnds;
            d->flipTimer->setTimeoutMS(kFlipDeadlineMS);
        }
        void detach() { tegra_smmu_detach(TEGRA_SMMU_ASID_DISPLAY, TEGRA_SMMU_DC_ASID); }
        void crcOff()
        {
            d->dc[kDcCrcControl] = 0;
            d->dc[kDcStateControl] = kGeneralUpdate;
            d->dc[kDcStateControl] = kGeneralActReq;
        }
    };
    // The scanned surface's first word (one pixel's low bits change; the write-combined view, then a barrier).
    void probeWritePixel(uint32_t value)
    {
        *reinterpret_cast<volatile uint32_t *>(surfaceMapping(probeSurface)->getVirtualAddress()) = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    // A window other than A that DC A would fetch for (windows B and C, WIN_ENABLE in their active options).
    bool otherWindowEnabled()
    {
        static constexpr uint32_t kWindowBCSelect[] = {1u << 5, 1u << 6};
        for (uint32_t select : kWindowBCSelect) {
            const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
            dc[kDcStateAccess] = access | kReadMuxActive;
            dc[kDcWindowHeader] = select;
            const uint32_t options = dc[kWinOptions];
            dc[kDcWindowHeader] = kWindowASelect;
            dc[kDcStateAccess] = access;
            if (options & kWinEnable) return true;
        }
        return false;
    }
    // The domain: every scan surface 1:1 (read-only: DC A only reads), the slot onto the scanned surface; cleaned to memory,
    // then the "dc" swgroup pointed at it. False (probeLine says why) leaves the swgroup as it was.
    bool probeAttach()
    {
        if (!probeDomain.init(probeTables->getBytesNoCopy(), probeTablesPa, kProbeTables)) {
            snprintf(probeLine, sizeof(probeLine), "the table buffer cannot hold a directory");
            return false;
        }
        bool ok = true;
        for (unsigned i = 0; i < ring.buffers; ++i)
            ok = ok && probeDomain.identity(surfaceAddress((int)i), kNativeBlockBytes, false);
        uint64_t pages[64];
        for (uint64_t off = 0; ok && off < kNativeBlockBytes; off += sizeof(pages) / sizeof(pages[0]) * 4096) {
            uint32_t n = 0;
            for (; n < 64 && off + n * 4096ull < kNativeBlockBytes; ++n)
                pages[n] = surfaceAddress(probeSurface) + off + n * 4096ull;
            ok = probeDomain.map(kProbeSlot + off, pages, n, false);
        }
        if (!ok) {
            snprintf(probeLine, sizeof(probeLine), "more than %u SMMU tables needed", kProbeTables);
            return false;
        }
        uintptr_t low, high;
        probeDomain.takeDirty(&low, &high);
        cleanInvalidate(probeTables->getBytesNoCopy(), (uint64_t)kProbeTables * 4096);
        probeSwgroupWas = tegra_smmu_read(TEGRA_SMMU_DC_ASID);
        return !tegra_smmu_attach(TEGRA_SMMU_ASID_DISPLAY, probeDomain.ptb(), TEGRA_SMMU_DC_ASID,
                                  TEGRA_SMMU_TRANSLATION_ENABLE_0, kDisplay0aTranslation, probeLine, sizeof(probeLine));
    }
    // Every DC A frame end: start the probe once things are quiet, or take its next step.
    void probeFrameEnd()
    {
        if (probeRequest && !(dc[kDcStateControl] & (kGeneralActReq | kWinAActReq))) {
            probeRequest = false;
            if (!probe.active() && kickDeferred) {  // the probe's last request (back to the physical address) is in
                kickDeferred = false;
                kickSwapQueue();
            }
        }
        if (!probe.active()) {
            if (probeRan || !probeTables || !doubleBuffer || !rotationLatched || hwSuspended || scanoutStopped ||
                tapeAttached || gPanicRestore.panicking)
                return;
            if (!probeAt) probeAt = frameEnds + kProbeAfterFrames;
            if (frameEnds < probeAt || transferred || ring.pending != kNoBuffer || ring.drawing != kNoBuffer ||
                ring.held != kNoBuffer || ring.superseded != kNoBuffer)
                return;
            probeRan = true;
            probeSurface = ring.front;
            if (otherWindowEnabled()) {
                IOLog("IOSwitch display: SMMU probe not run: window B or C is enabled (it would fetch through the SMMU "
                      "too)\n");
                return;
            }
            probePixel = *reinterpret_cast<volatile uint32_t *>(surfaceMapping(probeSurface)->getVirtualAddress());
            probeUnderflow = false;
            dc[kDcCrcControl] = kCrcOn;
            dc[kDcStateControl] = kGeneralUpdate;
            dc[kDcStateControl] = kGeneralActReq;
            probe.start();
            return;
        }
        const bool requestOut = dc[kDcStateControl] & (kGeneralActReq | kWinAActReq);
        const DisplaySmmuProbe::Action action = probe.frameEnd(dc[kDcCrcLatched], requestOut, probeUnderflow);
        probeUnderflow = false;
        ProbeIo io = {this};
        if (probe.perform(action, io)) finishProbe();
    }
    // The probe is over (passed, or failed and undone): its line, and the swaps it held go.
    void finishProbe()
    {
        if (probe.stage == DisplaySmmuProbe::kDone)
            IOLog("IOSwitch display: SMMU probe ok: DC A reads window A through the SMMU (ASID %u, swgroup dc, MC %#x "
                  "was "
                  "%#x, TRANSLATION_ENABLE_0 %#x): CRC %#x by physical address, 1:1 and at IOVA %#llx, %#x with one "
                  "pixel "
                  "changed; back in bypass\n",
                  (unsigned)TEGRA_SMMU_ASID_DISPLAY, (unsigned)TEGRA_SMMU_DC_ASID, probeSwgroupWas,
                  tegra_smmu_read(TEGRA_SMMU_TRANSLATION_ENABLE_0), probe.base, (unsigned long long)kProbeSlot,
                  probe.tweaked);
        else
            IOLog("IOSwitch display: SMMU probe failed at %s: %s%s%s (CRC base %#x again %#x pixel %#x last %#x); "
                  "undone, direct scanout stays off\n",
                  DisplaySmmuProbe::name(probe.failedAt), probe.why, probeLine[0] ? ": " : "", probeLine, probe.base,
                  probe.again, probe.tweaked, probe.seen);
        if (kickDeferred && !probeRequest) {  // else at the frame end that shows the last request taken
            kickDeferred = false;
            kickSwapQueue();
        }
    }
    // The display is going away or to sleep with the probe running: undo now (registers still clocked).
    void abortProbe(const char *reason)
    {
        if (!probe.active()) return;
        ProbeIo io = {this};
        probe.perform(probe.fail(reason), io);
        finishProbe();
    }
    // A surface left the scan: a frame held for the flip slot goes out (double-buffered after a fallback), and a swap waiting for a
    // surface may go.
    void flipLatched()
    {
        if (const int held = ring.flipHeld(); held != kNoBuffer) writeFlip(held);
        if (kickDeferred) {
            kickDeferred = false;
            kickSwapQueue();
        }
    }
    // Under the gate. A read of window A's active copy: the read mux follows STATE_ACCESS bit 0 and the window
    // WINDOW_HEADER; the write mux stays on the assembly copy.
    uint32_t windowActive(uint32_t index)
    {
        const uint32_t access = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcStateAccess] = access | kReadMuxActive;
        dc[kDcWindowHeader] = kWindowASelect;
        const uint32_t value = dc[index];
        dc[kDcStateAccess] = access;
        return value;
    }
    // Window A, read from its active copy, must be hekate's pitch window (_di_winA_pitch) or, with the console tape attached, that
    // window on the tape: the rotation and the tape reinterpret the same window and would only scramble anything else. The first
    // check keeps hekate's values for every way back (stopRotation, restorePitchWindow, the panic hook).
    void checkPitchWindow()
    {
        const uint32_t options = windowActive(kWinOptions), size = windowActive(kWinSize),
                       prescaled = windowActive(kWinPrescaled);
        const uint32_t dda = windowActive(kWinDdaIncrement), stride = windowActive(kWinLineStride),
                       kind = windowActive(kWinbufSurfaceKind);
        const uint32_t start = windowActive(kWinbufStartAddr), startHigh = windowActive(kWinbufStartAddrHi);
        const uint32_t hOffset = windowActive(kWinbufHOffset), vOffset = windowActive(kWinbufVOffset);
        const uint32_t depth = windowActive(kWinColorDepth), position = windowActive(kWinPosition);
        const bool boot = (stride & 0xffff) == 2880 && start == (uint32_t)bootMapping->getPhysicalAddress();
        const bool tape = tapeAttached && (stride & 0xffff) == kTapeStride && start >= tapeAddress &&
                          start - tapeAddress < kTapeLines * kTapeLineBytes &&
                          !((start - tapeAddress) % kTapeLineBytes);
        if ((options & (kWinEnable | kScanColumn | kHDirection | kVDirection)) != kWinEnable || size != kPitchSize ||
            prescaled != kPitchPrescaled || dda != kUnitDda || !(boot || tape) || (kind & 3) || startHigh || hOffset ||
            vOffset || position || (depth != 0xc && depth != 0xd))
            panic("IOSwitch display: window A is not hekate's pitch window: OPTIONS %#x SIZE %#x PRESCALED %#x DDA %#x "
                  "STRIDE %#x KIND %#x "
                  "START %#x:%#x H %#x V %#x POS %#x DEPTH %#x",
                  options, size, prescaled, dda, stride, kind, startHigh, start, hOffset, vOffset, position, depth);
        if (pitchSaved) return;
        pitchSaved = true;
        pitchOptions = options;
        pitchStride = stride;
        pitchKind = kind;
        pitchHOffset = hOffset;
        pitchActControl = dc[kDcRegActControl];
    }
    // From the first change to window A (the console tape or the rotation) on, a panic may have to put hekate's pitch window back
    // over the boot framebuffer (restorePitchOnPanic). The hook is chained once and stays; armed and tape say whether it acts.
    void chainPanicHook()
    {
        gPanicRestore.dc = dc;
        gPanicRestore.pixels = reinterpret_cast<volatile uint64_t *>(bootMapping->getVirtualAddress());
        gPanicRestore.bytes = bootMapping->getLength();
        gPanicRestore.options = pitchOptions;
        gPanicRestore.stride = pitchStride;
        gPanicRestore.kind = pitchKind;
        gPanicRestore.hOffset = pitchHOffset;
        gPanicRestore.start = surfaceAddress(0);
        static bool chained;
        if (!chained) {
            chained = true;
            gPanicRestore.previous = PE_arm_debug_panic_hook;
            PE_arm_debug_panic_hook = restorePitchOnPanic;
        }
        __asm__ volatile("dmb ish" ::: "memory");
    }
    // Under the gate, right after the first block-linear frame is in memory. Hekate's _di_winA_pitch and _di_winA_block differ in
    // exactly four window registers; everything else is checked to be hekate's pitch window first (on the console tape, its stride
    // and start address are the tape's), since the switch reinterprets the same memory.
    void startRotation()
    {
        checkPitchWindow();
        // Latch window A at the frame boundary: a layout change taken mid-frame would tear across the whole picture.
        dc[kDcRegActControl] = pitchActControl & ~kWinAActHcntrSel;
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcWindowHeader] = kWindowASelect;
        dc[kWinLineStride] = kBlockLineStride;
        dc[kWinbufSurfaceKind] = kBlockSurfaceKind;
        dc[kWinbufHOffset] = kBlockHOffset;
        dc[kWinOptions] = pitchOptions | kBlockScan;
        // Double-buffered, this frame is in surface 1 (ring.target() with surface 0 scanned and nothing asked for): no scrambled
        // frame through the pitch window at the handover. Single-buffered it is in the boot framebuffer, which window A may not scan
        // yet (the console tape).
        const int target = doubleBuffer ? ring.target() : 0;
        if (target == kNoBuffer)
            panic("IOSwitch display: the first frame has no scanout surface (front %d pending %d replaced %d)",
                  ring.front, ring.pending, ring.superseded);
        dc[kWinbufStartAddr] = surfaceAddress(target);
        // From here a panic must put the pitch window back (see restorePitchOnPanic); disarmed it only stops this driver's flips.
        chainPanicHook();
        gPanicRestore.armed = true;
        // Underflow latches without raising the line (INT_MASK untouched); checkRotation reads and clears it.
        dc[kDcIntStatus] = kWinAUnderflowInt;
        dc[kDcIntEnable] = dc[kDcIntEnable] | kWinAUnderflowInt;
        __asm__ volatile("dsb sy" ::: "memory");  // the swizzled frame is in memory before the window can fetch it
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
        rotated = true;
        rotationLatched = false;
        rotationRequested = frameEnds;
        underflowRun = 0;
        if (doubleBuffer) {
            ring.ready(target);
            flipRequested = frameEnds;
            flipTimer->setTimeoutMS(kFlipDeadlineMS);
            markFlip(target);
        }
        IOLog("IOSwitch display: window A to block-linear %s surface requested (ACT_CONTROL %#x -> %#x)\n",
              kBlockScanName, pitchActControl, pitchActControl & ~kWinAActHcntrSel);
    }
    // Every DC A frame end while rotated, under the gate: the ACT_REQ bits clear when the hardware takes the assembly copy.
    void checkRotation()
    {
        if (!rotationLatched) {
            const uint32_t control = dc[kDcStateControl];
            if (control & (kGeneralActReq | kWinAActReq)) {
                if (frameEnds - rotationRequested < kLatchFrames) return;
                panic("IOSwitch display: window A never latched the rotation in %u frames: STATE_CONTROL %#x "
                      "ACT_CONTROL %#x OPTIONS %#x "
                      "STRIDE %#x KIND %#x H %#x",
                      kLatchFrames, control, dc[kDcRegActControl], windowActive(kWinOptions),
                      windowActive(kWinLineStride), windowActive(kWinbufSurfaceKind), windowActive(kWinbufHOffset));
            }
            const uint32_t options = windowActive(kWinOptions), stride = windowActive(kWinLineStride);
            const uint32_t kind = windowActive(kWinbufSurfaceKind), hOffset = windowActive(kWinbufHOffset),
                           start = windowActive(kWinbufStartAddr);
            if (options != (pitchOptions | kBlockScan) || (stride & 0xffff) != kBlockLineStride ||
                kind != kBlockSurfaceKind || hOffset != kBlockHOffset || start != expectedStart())
                panic("IOSwitch display: DC A did not take the block-linear window A: OPTIONS %#x STRIDE %#x KIND %#x "
                      "H %#x START %#x, wrote %#x %#x %#x %#x %#x",
                      options, stride, kind, hOffset, start, pitchOptions | kBlockScan, kBlockLineStride,
                      kBlockSurfaceKind, kBlockHOffset, expectedStart());
            rotationLatched = true;
            underflowRun = 0;
            dc[kDcIntStatus] = kWinAUnderflowInt;
            stopConsoleTape();  // window A has left it
            if (!rotationAnnounced) {
                rotationAnnounced = true;
                IOLog("IOSwitch display: window A does a block-linear %s surface (latched after %llu frame ends)\n",
                      kBlockScanName, frameEnds - rotationRequested);
            }
            if (ring.pending != kNoBuffer) {
                // start is expectedStart(), the flip asked for last: taken (a replaced one never scanned for long).
                const int asked = ring.pending, replaced = ring.superseded;
                ring.latch(surfaceIndex(start), false);
                if (stats) {
                    if (replaced != kNoBuffer) ++stReplaced;
                    notePresent(__atomic_load_n(&feStamp, __ATOMIC_ACQUIRE), frameEnds - flipRequested, marks[asked]);
                }
                flipLatched();
            }
            return;
        }
        if (ring.pending != kNoBuffer) checkFlip(true);
        const uint32_t underflow = dc[kDcIntStatus] & kWinAUnderflowInt;
        if (!underflow) {
            underflowRun = 0;
            return;
        }
        dc[kDcIntStatus] = underflow;
        if (stats) stUnderflows++;
        if (probe.active()) probeUnderflow = true;
        if (++underflowRun < kUnderflowFrames) return;
        panic("IOSwitch display: window A underflowed %u frames in a row since the rotation: UFLOW_STATUS %#x OPTIONS "
              "%#x KIND %#x STRIDE %#x",
              underflowRun, windowActive(kWinbufUnderflow), windowActive(kWinOptions), windowActive(kWinbufSurfaceKind),
              windowActive(kWinLineStride));
    }
    // Under the gate: hekate's pitch window over the boot framebuffer again, from the next frame boundary.
    void restorePitchWindow()
    {
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcWindowHeader] = kWindowASelect;
        dc[kWinLineStride] = pitchStride;
        dc[kWinbufSurfaceKind] = pitchKind;
        dc[kWinbufHOffset] = pitchHOffset;
        dc[kWinbufStartAddr] = surfaceAddress(0);
        dc[kWinOptions] = pitchOptions;
        __asm__ volatile("dsb sy" ::: "memory");
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
        dc[kDcRegActControl] = pitchActControl;
    }
    // Under the gate, before the controller is unmapped: back to hekate's pitch window over a black frame, or, from the console
    // tape, over the console's screen (it redraws it in the boot framebuffer first; the tape may be read once more, never written).
    void stopRotation()
    {
        abortProbe("the display stopped");
        if (rotated) {
            gPanicRestore.armed = false;
            __asm__ volatile("dmb ish" ::: "memory");
            if (bootMapping)
                native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                                          bootMapping->getLength(), 2880, 720, 1280, 0xff000000U);
            restorePitchWindow();
            rotated = false;
            rotationLatched = false;
        } else if (tapeAttached) {
            gPanicRestore.tape = false;
            __asm__ volatile("dmb ish" ::: "memory");
            vc_tape_detach();
            restorePitchWindow();
        }
        stopConsoleTape();
        hwRotate = false;
        OSSafeReleaseNULL(blockMapping);
        // Surfaces 1 and 2 may be read until the pitch window latches: one frame of stale pixels at worst, never a write.
        doubleBuffer = false;
        kickDeferred = false;
        ring.reset(0);
        for (unsigned i = 1; i < kScanBuffers; ++i) {
            OSSafeReleaseNULL(scanMappings[i]);
            OSSafeReleaseNULL(scanBuffers[i]);
            scanAddresses[i] = 0;
        }
        if (flipTimer) {
            flipTimer->cancelTimeout();
            if (IOWorkLoop *loop = getWorkLoop()) loop->removeEventSource(flipTimer);
            OSSafeReleaseNULL(flipTimer);
        }
    }
    // Copy time of the last swap, with the running maximum and mean, at the 60th swap and every 600th after: the only hardware
    // measurement of the scanout copy (cntpct_el0, the architected counter at the ADT's timebase frequency).
    void noteCopy(uint64_t ticks)
    {
        copyFrames = copyFrames + 1;
        copyTicksSum = copyTicksSum + ticks;
        if (ticks > copyTicksMax) copyTicksMax = ticks;
        if (copyFrames != 60 && copyFrames % 600) return;
        const uint64_t hz = gPEClockFrequencyInfo.timebase_frequency_hz;
        if (!hz) return;
        // Double-buffered, the copy time is still only the copy: a swap that waits for the previous flip's latch waits before it.
        if (doubleBuffer)
            IOLog("IOSwitch display: block-linear copy %llu us, max %llu us, mean %llu us over %llu swaps "
                  "(double-buffered, %llu waited for a flip)\n",
                  ticks * 1000000 / hz, copyTicksMax * 1000000 / hz, copyTicksSum / copyFrames * 1000000 / hz,
                  copyFrames, flipWaits);
        else
            IOLog("IOSwitch display: %s copy %llu us, max %llu us, mean %llu us over %llu swaps\n",
                  hwRotate ? "block-linear" : "rotating", ticks * 1000000 / hz, copyTicksMax * 1000000 / hz,
                  copyTicksSum / copyFrames * 1000000 / hz, copyFrames);
    }

public:
    // Display power (SwitchBrightness; on unless switch-sleep=0), under the work-loop gate. Off, once the panel is asleep: FRAME_END and V_BLANK
    // masked (no display interrupt, so idle cores stay in WFI) and the controller stopped at the end of the frame (hekate
    // _di_dc_video_disable_config, without its power gating, so the restart needs no re-init). DSI stays in video mode. On: continuous
    // display again, then the same interrupts as before; TegraPanel's next command waits for the first frame end and panics without it.
    bool setScanout(bool on)
    {
        if (!dc) return false;
        // Powered down for SC7: the resume restarts the scanout (and wakes the panel) if the last request was on.
        if (hwSuspended) {
            resumeLit = on;
            return true;
        }
        if (on != scanoutStopped) return true;
        if (!on) dc[kDcIntMask] = dc[kDcIntMask] & ~(kVblankInt | kFrameEndInt);
        // Assembly copy for both the read and the write, as the rotation code leaves it: only the ACT_REQ below moves the mode.
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcDisplayCommand] = (dc[kDcDisplayCommand] & ~kDcModeMask) | (on ? kDcModeContinuous : 0);
        dc[kDcStateControl] = kGeneralUpdate;
        dc[kDcStateControl] = kGeneralActReq;
        dc[kDcStateControl] = kGeneralUpdate;
        dc[kDcStateControl] = kGeneralActReq;
        __atomic_fetch_and(&dcPending, ~(kVblankInt | kFrameEndInt), __ATOMIC_RELAXED);
        // A window underflow latched around the stop is not the rotated scanout failing (checkRotation counts runs of them).
        dc[kDcIntStatus] = kVblankInt | kFrameEndInt | kWinAUnderflowInt;
        underflowRun = 0;
        if (on) dc[kDcIntMask] = dc[kDcIntMask] | kFrameEndInt | (vsyncArmed ? kVblankInt : 0);
        scanoutStopped = !on;
        if (!on && tapeAttached)
            thread_wakeup((event_t)&tapeFrames);  // no frame end comes now: the console scrolls without one
        return true;
    }
    // SC7, step 1 (tegra_display_suspend), under the gate: whether the panel is lit, i.e. needs TegraPanel's power-off first.
    bool beginSuspend()
    {
        if (!dc || !vblankSource)
            panic("IOSwitch display: SC7 needs the DC A interrupt (sheng-vsync, ioswitch-vblank)");
        if (hwSuspended) panic("IOSwitch display: tegra_display_suspend twice without tegra_display_resume");
        return !scanoutStopped;
    }
    // SC7, step 2, under the gate, the panel asleep: the scanout stops as for display power off, window A's assembly registers (what
    // was last requested, a flip or the rotation not latched yet included) and the interrupt state are kept, and from here nothing
    // touches the controller until the resume. The panel comes back lit if the scanout still ran now (a display power request
    // between the steps is honoured, not undone).
    void quiesceForSuspend()
    {
        abortProbe("the display suspended (SC7)");
        resumeLit = !scanoutStopped;
        setScanout(false);
        dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
        dc[kDcWindowHeader] = kWindowASelect;
        for (size_t i = 0; i < sizeof(kWindowAState) / sizeof(kWindowAState[0]); ++i)
            savedWindow[i] = dc[kWindowAState[i]];
        savedDispWinOptions = dc[kDcDispWinOptions];
        savedActControl = dc[kDcRegActControl];
        savedScratchB = dc[kDcScratchB];
        savedIntEnable = dc[kDcIntEnable];
        savedIntMask = dc[kDcIntMask];
        // The requested surface is complete (flip() follows the copy); it is the one window A gets back.
        ring.settle();
        gPanicRestore.armed = false;
        __asm__ volatile("dmb ish" ::: "memory");
        vblankSource->disable();
        hwSuspended = true;
    }
    // SC7, step 3 (tegra_display_resume), under the gate, after tegra_display_power_up() left DC A configured and stopped: window A
    // as it was, its interrupts as at the suspend, the frame ends re-armed, the scanout restarted if it ran, and a swap that waited
    // kicked. checkRotation verifies the restored window at the first frame ends, as at the first rotation. True: wake the panel.
    bool finishResume()
    {
        if (!hwSuspended) panic("IOSwitch display: tegra_display_resume without tegra_display_suspend");
        dc[kDcStateAccess] = 0;
        dc[kDcWindowHeader] = kWindowASelect;
        dc[kWinOptions] = 0;
        for (size_t i = 0; i < sizeof(kWindowAState) / sizeof(kWindowAState[0]); ++i)
            dc[kWindowAState[i]] = savedWindow[i];
        dc[kDcDispWinOptions] = savedDispWinOptions;
        dc[kDcRegActControl] = savedActControl;
        dc[kDcScratchB] = savedScratchB;
        dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
        dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
        dc[kDcIntStatus] = kVblankInt | kFrameEndInt | kWinAUnderflowInt;
        dc[kDcIntEnable] = savedIntEnable;
        dc[kDcIntMask] = savedIntMask;
        __atomic_store_n(&dcPending, 0u, __ATOMIC_RELAXED);
        hwSuspended = false;
        scanoutStopped = true;
        vblankSource->enable();
        if (rotated) {
            rotationLatched = false;
            rotationRequested = frameEnds;
            underflowRun = 0;
            __asm__ volatile("dmb ish" ::: "memory");
            gPanicRestore.armed = true;
        }
        if (resumeLit) setScanout(true);
        if (presentDeferred) {
            presentDeferred = false;
            presentCopied();
        }  // an off-gate copy that finished during the suspend
        if (gpuPresentDeferred) {
            gpuPresentDeferred = false;
            presentGpuCopy();
        }  // likewise a GPU frame copy
        if (const int held = ring.flipHeld(); held != kNoBuffer) writeFlip(held);  // a frame held for the flip slot
        if (kickDeferred) {
            kickDeferred = false;
            kickSwapQueue();
        }
        return resumeLit;
    }
    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop. The GPU frame copy's
    // completion (any context, even the GPU's interrupt filter) only records its status and signals the work loop.
    void gpuComplete(int status)
    {
        if (gpuDoneSource) {
            __atomic_store_n(&gpuStatus, status, __ATOMIC_RELAXED);
            __atomic_store_n(&gpuDonePending, true, __ATOMIC_RELEASE);
            gpuDoneSource->interruptOccurred(nullptr, nullptr, 0);
            return;
        }
        struct Args {
            NativeLegacyDisplay *display;
            int status;
        } args = {this, status};
        IOWorkLoop *loop = getWorkLoop();
        auto action = [](OSObject *, void *a0, void *, void *, void *) -> IOReturn {
            Args *a = static_cast<Args *>(a0);
            a->display->finishGpuSwap(a->status);
            return kIOReturnSuccess;
        };
        if (loop)
            loop->runAction(action, this, &args);
        else
            finishGpuSwap(status);
    }
    void free() override
    {
        forgetPrimary();
        stopGpuCopy();
        stopCopyLoop();
        stopVsync();
        forgetMappings();
        if (logicalPM) {
            PMstop();
            logicalPM = false;
        }
        primary = false;
        OSSafeReleaseNULL(bootMapping);
        IOMobileFramebufferLegacy::free();
    }
    void stop(IOService *provider) override
    {
        forgetPrimary();
        stopGpuCopy();
        stopCopyLoop();
        stopVsync();
        if (IOWorkLoop *loop = getWorkLoop())
            loop->runAction(
                [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                    static_cast<NativeLegacyDisplay *>(owner)->forgetMappings();
                    return kIOReturnSuccess;
                },
                this);
        transferred = false;
        for (auto &entry : validated) entry = nullptr;
        gpuRequest = nullptr;
        gpuRetired = false;
        if (logicalPM) {
            PMstop();
            logicalPM = false;
        }
        primary = false;
        IOMobileFramebufferLegacy::stop(provider);
        OSSafeReleaseNULL(bootMapping);
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840,
    // the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part of this
    // port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is the
    // original state setter (virtual 354, the one the original code calls with this same request shape) with only the powered word named.
    void announcePoweredByFirmware()
    {
        struct StateRequest {
            uint32_t skipRequested, requested, skipPowered, powered;
            uint8_t changed;
            uint8_t pad[11];
        } request = {1, 0, 0, 1, 1, {}};
        typedef int (*Setter)(void *, StateRequest *, bool);
        Setter setter = reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this, &request, false);
    }
    static bool bootFlag(const char *name)
    {
        unsigned value = 0;
        return PE_parse_boot_argn(name, &value, sizeof(value)) && value == 1;
    }
    bool start(IOService *provider) override
    {
        unsigned early = 1;
        PE_parse_boot_argn("sheng-early-retire", &early, sizeof(early));
        bootEarlyRetire = early == 1;
        bootTransfer = bootFlag("offline-legacy-transfer");
        bootRetire = bootFlag("offline-legacy-retire");
        bootCpuPresent = bootFlag("offline-legacy-cpu-present");
        bootValidate = bootFlag("offline-legacy-validate");
        if (!startDisplay(provider)) return false;
        stats = bootFlag("ioswitch-display-stats");
        fastSwizzle = bootFlag("ioswitch-fast-swizzle");
        keepMappings = bootFlag("ioswitch-surface-maps");
        unsigned waitMs = 100;
        if (PE_parse_boot_argn("ioswitch-gpu-wait-ms", &waitMs, sizeof(waitMs)) && waitMs >= 1 && waitMs <= 1000)
            gpuWaitNs = (uint64_t)waitMs * 1000000ULL;
        if (bootFlag("ioswitch-frame-scan")) scanBudget = kScanFrames;
        startVsync();
        startHardwareRotation();
        if (IOWorkLoop *loop = hwRotate ? getWorkLoop() : nullptr)
            loop->runAction(
                [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                    static_cast<NativeLegacyDisplay *>(owner)->startConsoleTape();
                    return kIOReturnSuccess;
                },
                this);
        startCopyLoop();
        startGpuCopy();
        if (primary) __atomic_store_n(&primaryDisplay, this, __ATOMIC_RELEASE);
        return true;
    }
    static NativeLegacyDisplay *primaryDisplay;
    void forgetPrimary()
    {
        NativeLegacyDisplay *self = this;
        __atomic_compare_exchange_n(&primaryDisplay, &self, nullptr, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    bool startDisplay(IOService *provider)
    {
        if (!IOMobileFramebufferLegacy::start(provider)) return false;
        if (primary && bootMapping) announcePoweredByFirmware();
        unsigned enabled = 0;
        PE_parse_boot_argn("offline-legacy-pm", &enabled, sizeof(enabled));
        if (!enabled) return true;
        unsigned phase = 0;
        PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
        if (enabled != 1 || phase != 4 || !provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1, kIOReturnNotReady, 0);
            IOMobileFramebufferLegacy::stop(provider);
            return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2] = {};
        states[0].version = states[1].version = 1;
        states[1].capabilityFlags = kIOPMDeviceUsable;
        PMinit();
        logicalPM = initialized;
        if (!logicalPM) {
            native_legacy_pm_observed(1, kIOReturnNotReady, 0);
            IOMobileFramebufferLegacy::stop(provider);
            return false;
        }
        provider->joinPMtree(this);
        IOReturn status = registerPowerDriver(this, states, 2);
        native_legacy_pm_observed(1, status, 0);
        if (status == kIOReturnSuccess) {
            status = changePowerStateToPriv(1);
            native_legacy_pm_observed(2, status, 1);
        }
        if (status != kIOReturnSuccess) {
            PMstop();
            logicalPM = false;
            IOMobileFramebufferLegacy::stop(provider);
            return false;
        }
        return true;
    }
    IOReturn setPowerState(unsigned long ordinal, IOService *device) override
    {
        if (!logicalPM) return IOMobileFramebufferLegacy::setPowerState(ordinal, device);
        if (ordinal > 1 || !bootMapping) {
            native_legacy_pm_observed(3, kIOReturnUnsupported, ordinal);
            return kIOReturnUnsupported;
        }
        native_legacy_pm_observed(3, kIOReturnSuccess, ordinal);
        IOReturn status = IOMobileFramebufferLegacy::setPowerState(ordinal, device);
        native_legacy_pm_observed(4, status, ordinal);
        return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options = 0) override
    {
        unsigned enabled = 0;
        PE_parse_boot_argn("offline-legacy-register", &enabled, sizeof(enabled));
        if (enabled != 1) native_legacy_backend_guard(84, this, __builtin_return_address(0));
        // Nothing in the family publishes IOMFBDisplayRefresh (its platform subclass does), and without it the render server keeps its own
        // 1/60 s heartbeat (int-210: refresh=0). Publish it before any client can find the display, only when sheng-vsync asks for more.
        unsigned hz = 0;
        PE_parse_boot_argn("sheng-vsync", &hz, sizeof(hz));
        if (primary && hz > 60 && hz <= 240) native_legacy_set_refresh_properties(this);
        IOService::registerService(options);
        native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override { return primary; }
    // getRefreshAttributes (slot 301) is not overridden: at 60 Hz the generic 1/60 s refresh words are already right.
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking = false, rekick = false;
    void kickSwapQueue() override
    {
        armVsync();
        if (kicking) {
            rekick = true;
            return;
        }
        kicking = true;
        do {
            rekick = false;
            kickOnce();
            gpuBypass = false;
        } while (rekick && !transferred);
        kicking = false;
    }
    void kickOnce()
    {
        if (!bootTransfer || transferred || !logicalPM || !bootMapping || !primary) return;
        const OriginalLegacySwapRequest *request = pendingSwapRequest();
        if (!request || !isValidated(request)) return;
        if (probe.active() || probeRequest) {  // window A is the SMMU probe's for a few frames
            kickDeferred = true;
            return;
        }
        // Every surface is held (one scanned, one asked for, and with three a replaced one not yet known to be untaken, or a flip
        // out with two), or the controller is unclocked during SC7: the frame end that settles a latch, or the resume, kicks again.
        const bool noSurface = doubleBuffer && ring.target() == kNoBuffer;
        if (noSurface || hwSuspended) {
            if (noSurface && !kickDeferred) {
                flipWaits = flipWaits + 1;
                if (stats) deferredAt = mach_absolute_time();
            }
            kickDeferred = true;
            return;
        }
        // r14-pacing: a newer accepted swap queued behind this one makes it stale: shown, it would hold the newer one back a frame
        // (or be replaced at once). Every accepted swap is a whole frame, so it retires unshown, as an early retire does at a
        // copy's start, and the newest one goes. Only with early retirement (the default): with sheng-early-retire=0 every swap
        // retires after its own copy.
        if (bootEarlyRetire) {
            for (const OriginalLegacySwapRequest *next = queuedAfter(request); next && isValidated(next);
                 next = queuedAfter(request)) {
                if (!retireSwap(request, true)) break;
                if (stats) ++stCollapsed;
                request = pendingSwapRequest();
                if (!request || !isValidated(request)) return;
            }
        }
        const uint64_t kickAt = stats ? mach_absolute_time() : 0;
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
        OSObject *surface = nullptr;
        memcpy(&surface, reinterpret_cast<const unsigned char *>(request) + 2624, sizeof(surface));
        if (!surface) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank = {};
            uint32_t bw = 0, bh = 0;
            getDisplayDimensions(&bw, &bh);
            uint64_t blankLength = (uint64_t)bw * bh * 4;
            if (PE_current_console(&blank) || blank.v_baseAddr != bootMapping->getPhysicalAddress() ||
                !native_legacy_video_mode(blank) || native_legacy_video_width(blank) != bw ||
                native_legacy_video_height(blank) != bh ||
                !native_legacy_surface_bounds((uint32_t)blank.v_width, (uint32_t)blank.v_height, blank.v_rowBytes,
                                              bootMapping->getLength(), bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
                return;
            }
            if (!native_legacy_prepare_scanout()) {
                native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
                return;
            }
            if (tapeSettling()) return;
            // Once window A is rotated it reads the whole block-linear extent, which reaches past the pitch frame (block row 5). While
            // it scans the console tape, a blank frame leaves it as a first client frame does: block-linear, through startRotation.
            const bool block = rotated || tapeAttached;
            if (block ? !native_legacy_block_fill(reinterpret_cast<volatile void *>(drawSurface()->getVirtualAddress()),
                                                  drawSurface()->getLength(), 0xff000000U)
                      : !native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                                                   bootMapping->getLength(), blank.v_rowBytes, (uint32_t)blank.v_width,
                                                   (uint32_t)blank.v_height, 0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument, request, 0);
                return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            noteTaken(request, kickAt);  // before the flip, which carries it to the present
            if (block) presentBlock();
            shadowValid = false;
            transferred = true;
            native_legacy_frame_observed(1, 0, 0, 0, 0, 0);
            {
                const uint32_t colors[4] = {0xff000000U, 0, 0, 0};
                const uint64_t counts[4] = {(uint64_t)bw * bh, 0, 0, 0};
                native_legacy_frame_colors_observed(colors, counts, 0);
            }
            native_legacy_transfer_observed(kIOReturnSuccess, request, blankLength);
            finishTransfer(request);
            return;
        }
        if (!surface->metaCast("IOSurface")) return;
        IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
        PE_Video video = {};
        uint32_t width = 0, height = 0;
        getDisplayDimensions(&width, &height);
        uint64_t length = (uint64_t)width * height * 4;
        uint64_t sourceStride = native_legacy_surface_stride(surface);
        uint64_t allocation = native_legacy_surface_allocation(surface);
        if (!memory || !memory->metaCast("IOMemoryDescriptor") ||
            !native_legacy_surface_bounds(width, height, sourceStride, allocation, memory->getLength()) ||
            native_legacy_surface_width(surface) < width || native_legacy_surface_height(surface) < height ||
            !nativeBgra(native_legacy_surface_format(surface), native_legacy_surface_planes(surface)) ||
            PE_current_console(&video) || video.v_baseAddr != bootMapping->getPhysicalAddress() ||
            !native_legacy_video_mode(video) || native_legacy_video_width(video) != width ||
            native_legacy_video_height(video) != height ||
            !native_legacy_surface_bounds((uint32_t)video.v_width, (uint32_t)video.v_height, video.v_rowBytes,
                                          bootMapping->getLength(), bootMapping->getLength())) {
            native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
            return;
        }
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if (!gpuBypass) {
            if (!native_legacy_prepare_scanout()) {
                native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
                return;
            }
            if (tapeSettling()) return;
        }
        noteSurface(surface, memory);
        if (gpuCopy && !gpuBypass && offerGpuCopy(request, memory, sourceStride, allocation, width, height, kickAt))
            return;
        if (!gpuBypass && native_legacy_gpu_offer(this, memory, sourceStride, width, height)) {
            // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            shadowValid = false;
            gpuRequest = request;
            gpuLength = length;
            transferred = true;
            // The compositor renders the next frame only after this swap retires, so retiring at the end of the copy serialized render and
            // copy (int-243: about 4.4 ms render then about 3 ms copy per frame). Retire now: the presenter holds its own reference to this
            // frame's memory, the previous frame's copy has finished (transferred held this one back), and the next swap still waits for
            // the copy because transferred stays set until finishGpuSwap.
            noteTaken(request, kickAt);
            if (bootEarlyRetire && retireSwap(request, true)) {
                gpuRetired = true;
                native_legacy_early_retires = native_legacy_early_retires + 1;
            }
            return;
        }
        uint64_t mapAt = stats ? mach_absolute_time() : 0;
        bool ownedSource = true;
        IOMemoryMap *source = sourceMapping(memory, ownedSource);
        auto releaseSource = [&] {
            if (ownedSource) OSSafeReleaseNULL(source);
        };
        if (!source || !source->getVirtualAddress() || source->getLength() < allocation) {
            releaseSource();
            native_legacy_transfer_observed(kIOReturnNoMemory, request, 0);
            return;
        }
        if (stats) stMap.add(mach_absolute_time() - mapAt);
        const void *sourceAddress = reinterpret_cast<const void *>(source->getVirtualAddress());
        if (asyncCopy && hwRotate) {
            if (!native_legacy_block_layout(reinterpret_cast<volatile void *>(drawSurface()->getVirtualAddress()),
                                            drawSurface()->getLength(), sourceAddress, source->getLength(),
                                            sourceStride, width, height)) {
                releaseSource();
                native_legacy_transfer_observed(kIOReturnBadArgument, request, 0);
                return;
            }
            startCopy(request, source, ownedSource, sourceStride, allocation, width, height, kickAt);
            return;
        }
        {
            uint64_t gpu, cache;
            bool timedOut;
            syncForCpu(sourceAddress, allocation, gpu, cache, timedOut);
            if (stats && tegra_gm20b_sync_for_cpu) {
                stGpu.add(gpu);
                stCache.add(cache);
                stGpuTimeouts += timedOut;
            }
        }
        if (hwRotate
                ? !native_legacy_block_layout(reinterpret_cast<volatile void *>(drawSurface()->getVirtualAddress()),
                                              drawSurface()->getLength(), sourceAddress, source->getLength(),
                                              sourceStride, width, height)
                : !native_legacy_scanout_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                                                bootMapping->getLength(), video, sourceAddress, source->getLength(),
                                                sourceStride, width, height)) {
            releaseSource();
            native_legacy_transfer_observed(kIOReturnBadArgument, request, 0);
            return;
        }
        if (scanBudget) {
            const uint64_t scanAt = stats ? mach_absolute_time() : 0;
            --scanBudget;
            scanFrame(sourceAddress, source->getLength(), sourceStride, width, height,
                      kScanFrames - scanBudget <= kColorFrames);
            if (stats) stScan.add(mach_absolute_time() - scanAt);
        }
        if (!native_legacy_prepare_scanout()) {
            releaseSource();
            native_legacy_transfer_observed(kIOReturnNotReady, request, 0);
            return;
        }
        if (stats && stReadDue && hwRotate) {
            stReadDue = false;
            stRead = readPass(sourceAddress, sourceStride, width, height);
        }
        noteTaken(request, kickAt);  // before the flip, which carries it to the present
        uint64_t copyStart;
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(copyStart));
        uint64_t stored = UINT64_MAX;
        {
            // Store only what changed since the previous frame (the scanout memory is slow device memory).
            uint64_t need = (uint64_t)width * height * 4;
            // Differential copy is opt-in (sheng-fb-diff=1): on hardware it reads the whole source every frame and was not faster
            // than the plain copy (143-285 ms per swap either way), and the first boots with it were followed by an unresponsive
            // lock screen that disappeared with the plain copy (int-005). Those times are UniXNU-sheng's, another device; the
            // Switch's own copy time is the "IOSwitch display: ... copy" line noteCopy logs.
            static int diffOff = -1;
            if (diffOff < 0) {
                unsigned v = 0;
                PE_parse_boot_argn("sheng-fb-diff", &v, sizeof v);
                diffOff = (v != 1);
            }
            if (!video.v_rotate && !diffOff && !shadow && need <= (64ull << 20)) {
                shadow = IOMallocAligned((vm_size_t)need, 16384);
                if (shadow) {
                    shadowBytes = need;
                    shadowValid = false;
                }
            }
            if (!video.v_rotate && !diffOff && shadow && shadowBytes >= need)
                stored = native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                                                 bootMapping->getLength(), video.v_rowBytes, sourceAddress,
                                                 source->getLength(), sourceStride, width, height, shadow, shadowValid);
            if (stored != UINT64_MAX) shadowValid = true;
        }
        if (stored == UINT64_MAX && hwRotate) {
            // One unrotated swizzling pass; the first such frame switches window A under it (latched at a frame boundary;
            // single-buffered, the frame scanned while this copy runs shows the new bytes through the old pitch window once).
            // Double-buffered, the pass fills the surface window A does not scan and window A flips to it.
            const Swizzled how =
                swizzle(drawSurface(), sourceAddress, source->getLength(), sourceStride, width, height);
            if (how == kSwizzleFailed) {
                releaseSource();
                native_legacy_transfer_observed(kIOReturnBadArgument, request, 0);
                return;
            }
            if (stats) noteSwizzled(how);
            stored = length;
            presentBlock();
        }
        if (stored == UINT64_MAX) {
            shadowValid = false;
            if (!native_legacy_scanout_copy(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                                            bootMapping->getLength(), video, sourceAddress, source->getLength(),
                                            sourceStride, width, height)) {
                releaseSource();
                native_legacy_transfer_observed(kIOReturnBadArgument, request, 0);
                return;
            }
            stored = length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {
            uint64_t copyEnd;
            __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(copyEnd));
            native_legacy_copy_timing_observed(copyEnd - copyStart, stored);
            noteCopy(copyEnd - copyStart);
            if (stats) {
                stCopy.add(copyEnd - copyStart);  // the architected counter: mach time's unit on arm64
                if (stSourceAttr == 0x100)
                    stSourceAttr = mairAttribute(sourceAddress);  // the first page is mapped now: the copy read it
            }
        }
        releaseSource();
        transferred = true;
        native_legacy_transfer_observed(kIOReturnSuccess, request, length);
        finishTransfer(request);
    }
    // A swap the kick has finished with (copied, filled or handed to the GPU), before it retires: its queue time and flip wait.
    void noteTaken(const OriginalLegacySwapRequest *request, uint64_t kickAt)
    {
        if (!stats) return;
        // r6-lows/r14-pacing: carried to the flip of this frame (flip, markFlip), then to its present (notePresent).
        taken = {submittedAt(request), kickAt, 0, 0};
        if (taken.submit) stQueue.add(kickAt > taken.submit ? kickAt - taken.submit : 0);
        if (deferredAt) {
            taken.waited = kickAt > deferredAt ? kickAt - deferredAt : 0;
            stFlipWait.add(taken.waited);
            deferredAt = 0;
        }
    }
    // The kernel mapping of a surface's memory for its copy. Kept (ioswitch-surface-maps=1), it stays in surfaceMappings and owned is
    // false; otherwise the caller releases it, as before.
    IOMemoryMap *sourceMapping(IOMemoryDescriptor *memory, bool &owned)
    {
        owned = !keepMappings;
        if (owned) return memory->map(kIOMapAnywhere | kIOMapReadOnly);
        SurfaceMapping *oldest = &surfaceMappings[0];
        for (auto &entry : surfaceMappings) {
            if (entry.memory == memory) {
                entry.used = ++surfaceUses;
                return entry.map;
            }
            if (entry.used < oldest->used) oldest = &entry;
        }
        IOMemoryMap *map = memory->map(kIOMapAnywhere | kIOMapReadOnly);
        if (!map || !map->getVirtualAddress()) {
            OSSafeReleaseNULL(map);
            return nullptr;
        }
        forgetMapping(*oldest);
        memory->retain();
        *oldest = {memory, map, ++surfaceUses};
        if (stats) ++stNewMaps;
        return map;
    }
    void forgetMapping(SurfaceMapping &entry)
    {
        OSSafeReleaseNULL(entry.map);
        OSSafeReleaseNULL(entry.memory);
        entry = {};
    }
    void forgetMappings()
    {
        for (auto &entry : surfaceMappings) forgetMapping(entry);
    }
    // With CA on Metal (tegra-gpu=4) the GPU renders the surface, and CA may swap it before the GPU is done. The GPU is not IO-coherent:
    // wait for its submitted work (at most ioswitch-gpu-wait-ms, 100 by default; later, the frame shows torn), then drop the mapping's
    // cache lines (the frame copied from the same surface a few swaps ago is still in them) so every read after sees DRAM. No GPU client:
    // returns at once, no cache operation. With the stats the two are timed apart: the wait alone (a zero-length range), then the same
    // clean+invalidate done here (not a second zero-timeout sync: gpu.c panics on any sync timeout, and a submission made in between
    // would be one). Ticks, 0 without stats.
    void syncForCpu(const void *address, uint64_t bytes, uint64_t &gpu, uint64_t &cache, bool &timedOut) const
    {
        gpu = cache = 0;
        timedOut = false;
        if (!tegra_gm20b_sync_for_cpu) return;
        if (!stats) {
            tegra_gm20b_sync_for_cpu(address, bytes, gpuWaitNs);
            return;
        }
        const uint64_t waitAt = mach_absolute_time();
        timedOut = tegra_gm20b_sync_for_cpu(address, 0, gpuWaitNs) != 0;
        const uint64_t cacheAt = mach_absolute_time();
        cleanInvalidate(address, bytes);
        gpu = cacheAt - waitAt;
        cache = mach_absolute_time() - cacheAt;
    }
    // The two content scans read every pixel again, byte by byte; they only describe the first frames (the probe keeps kScanFrames,
    // and the colours of the first kColorFrames only, so the colour pass stops there).
    static void scanFrame(const void *source, uint64_t bytes, uint64_t stride, uint32_t width, uint32_t height,
                          bool colors)
    {
        uint64_t scan[5];
        if (native_legacy_frame_scan(source, bytes, stride, width, height, scan))
            native_legacy_frame_observed(0, scan[0], scan[1], scan[2], scan[3], scan[4]);
        uint32_t found[4];
        uint64_t counts[4], other = 0;
        if (colors && native_legacy_frame_colors(source, bytes, stride, width, height, found, counts, &other))
            native_legacy_frame_colors_observed(found, counts, other);
    }
    // The block-linear pass into target: kSwizzleFast, kSwizzleByte or kSwizzleFailed. Both loops write the same bytes;
    // native_legacy_block_layout (the caller's check) bounds them, and the fast one (ioswitch-fast-swizzle=1) declines a source whose
    // address or row stride is not 16-byte aligned, or a target not 512-byte aligned (said once).
    enum Swizzled { kSwizzleFailed, kSwizzleFast, kSwizzleByte };
    Swizzled swizzle(IOMemoryMap *target, const void *source, uint64_t bytes, uint64_t stride, uint32_t width,
                     uint32_t height) const
    {
        void *out = reinterpret_cast<void *>(target->getVirtualAddress());
        if (fastSwizzle) {
            if (tegra_block_swizzle_aligned(out, source, stride, height / 8, width / 16, kNativeBlockRow))
                return kSwizzleFast;
            static bool told;
            if (!__atomic_exchange_n(&told, true, __ATOMIC_RELAXED))
                IOLog("IOSwitch display: ioswitch-fast-swizzle declined: source offset %#llx, stride %llu (both need "
                      "16-byte alignment), "
                      "target offset %#llx (needs 512); the byte loop copies instead\n",
                      (unsigned long long)((uintptr_t)source & 0xfff), (unsigned long long)stride,
                      (unsigned long long)((uintptr_t)out & 0xfff));
        }
        return native_legacy_block_copy(out, target->getLength(), source, bytes, stride, width, height)
                   ? kSwizzleByte
                   : kSwizzleFailed;
    }
    void noteSwizzled(Swizzled how)
    {
        if (how == kSwizzleFast)
            ++stFast;
        else if (how == kSwizzleByte)
            ++stByte;
    }
    // The stats' read probe: every visible row of the source loaded once, 16 bytes at a time as the fast swizzle does. Ticks; 0 when
    // the source is not 16-byte aligned.
    static uint64_t readPass(const void *source, uint64_t stride, uint32_t width, uint32_t height)
    {
        if (((uintptr_t)source | stride) & 15) return 0;
        uint64_t start, end, sum = 0;
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
        for (uint32_t y = 0; y < height; ++y) {
            const uint64_t *row =
                reinterpret_cast<const uint64_t *>(reinterpret_cast<uintptr_t>(source) + (uint64_t)y * stride);
            for (uint32_t x = 0; x < width / 2; x += 2) sum ^= row[x] + row[x + 1];
        }
        __asm__ volatile("" ::"r"(sum));  // keeps the loads
        __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(end));
        return end - start;
    }
    // ioswitch-display-async: after startHardwareRotation, which it needs.
    void startCopyLoop()
    {
        unsigned mode = 0;
        PE_parse_boot_argn("ioswitch-display-async", &mode, sizeof(mode));
        if (!mode || !primary) return;
        if ((mode != 1 && mode != 2) || !hwRotate) {
            IOLog("IOSwitch display: ioswitch-display-async=%u ignored: it is 1 or 2 and needs the hardware rotation "
                  "(ioswitch-hw-rotate)\n",
                  mode);
            return;
        }
        copyLoop = IOWorkLoop::workLoop();
        copySource =
            copyLoop ? IOInterruptEventSource::interruptEventSource(this, &NativeLegacyDisplay::copyFired) : nullptr;
        if (!copySource || copyLoop->addEventSource(copySource) != kIOReturnSuccess)
            panic("IOSwitch display: ioswitch-display-async=%u: cannot start the copy thread", mode);
        asyncCopy = mode;
    }
    // Stop and free, outside the gate: no copy may run once the scanout and kept mappings go. Removing the source closes the copy
    // loop's gate, so it waits for a running copyWork, and with it for its copyFinished (which takes the family's gate, free here).
    void stopCopyLoop()
    {
        if (!copyLoop) return;
        IOWorkLoop *loop = getWorkLoop();
        if (!loop)
            panic("IOSwitch display: ioswitch-display-async: the display's work loop is gone before its copy thread "
                  "stopped");
        loop->runAction(
            [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                __atomic_store_n(&static_cast<NativeLegacyDisplay *>(owner)->copyStopping, true, __ATOMIC_RELEASE);
                return kIOReturnSuccess;
            },
            this);
        copyLoop->removeEventSource(copySource);
        OSSafeReleaseNULL(copySource);
        OSSafeReleaseNULL(copyLoop);
        // A hand-off the copy thread never took: drop its mapping (its request goes with the family's own stop).
        loop->runAction(
            [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
                if (self->copyQueued && self->job.ownedSource) OSSafeReleaseNULL(self->job.source);
                self->copyQueued = false;
                self->asyncCopy = 0;
                return kIOReturnSuccess;
            },
            this);
    }
    // Under the gate, a validated surface swap mapped and bounded: hand it to the copy thread. The probe's scans are decided here, so
    // scanBudget stays under the gate.
    void startCopy(const OriginalLegacySwapRequest *request, IOMemoryMap *source, bool ownedSource, uint64_t stride,
                   uint64_t allocation, uint32_t width, uint32_t height, uint64_t kickAt)
    {
        job = {};
        job.request = request;
        job.source = source;
        job.target = drawSurface();
        if (doubleBuffer) ring.claim(ring.target());  // the copy thread writes it until copyFinished
        job.ownedSource = ownedSource;
        job.stride = stride;
        job.allocation = allocation;
        job.width = width;
        job.height = height;
        if (scanBudget) {
            --scanBudget;
            job.scan = true;
            job.colors = kScanFrames - scanBudget <= kColorFrames;
        }
        if (stats && stReadDue) {
            stReadDue = false;
            job.read = true;
        }
        transferred = true;
        copyQueued = true;
        noteTaken(request, kickAt);
        if (asyncCopy == 2 && retireSwap(request, true)) job.retired = true;
        copySource->interruptOccurred(nullptr, nullptr, 0);
    }
    static void copyFired(OSObject *owner, IOInterruptEventSource *, int)
    {
        static_cast<NativeLegacyDisplay *>(owner)->copyWork();
    }
    // The copy thread, outside the family's gate: the part of a swap that reads the surface. job is not touched after copyFinished.
    void copyWork()
    {
        const void *source = reinterpret_cast<const void *>(job.source->getVirtualAddress());
        job.status = kIOReturnSuccess;
        if (__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE))
            job.status = kIOReturnAborted;
        else {
            syncForCpu(source, job.allocation, job.gpu, job.cache, job.timedOut);
            if (job.scan) {
                const uint64_t scanAt = mach_absolute_time();
                scanFrame(source, job.source->getLength(), job.stride, job.width, job.height, job.colors);
                job.scanned = mach_absolute_time() - scanAt;
            }
            if (!native_legacy_prepare_scanout())
                job.status = kIOReturnNotReady;
            else {
                if (job.read) job.readTicks = readPass(source, job.stride, job.width, job.height);
                uint64_t start, end;
                __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
                job.swizzled = swizzle(job.target, source, job.source->getLength(), job.stride, job.width, job.height);
                if (job.swizzled == kSwizzleFailed) job.status = kIOReturnBadArgument;
                __asm__ volatile("dsb sy" ::: "memory");
                __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(end));
                job.copy = end - start;
                if (stats) job.attribute = mairAttribute(source);
            }
        }
        getWorkLoop()->runAction(
            [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
                static_cast<NativeLegacyDisplay *>(owner)->copyFinished();
                return kIOReturnSuccess;
            },
            this);
    }
    // Under the gate, the copy thread done with job.
    void copyFinished()
    {
        copyQueued = false;
        if (job.ownedSource) OSSafeReleaseNULL(job.source);
        if (job.status == kIOReturnSuccess) {
            native_legacy_copy_timing_observed(job.copy, (uint64_t)job.width * job.height * 4);
            noteCopy(job.copy);
        }
        if (stats) {
            if (tegra_gm20b_sync_for_cpu && job.status != kIOReturnAborted) {
                stGpu.add(job.gpu);
                stCache.add(job.cache);
                stGpuTimeouts += job.timedOut;
            }
            if (job.scan) stScan.add(job.scanned);
            if (job.read) stRead = job.readTicks;
            if (job.status == kIOReturnSuccess) {
                stCopy.add(job.copy);
                noteSwizzled(static_cast<Swizzled>(job.swizzled));
            }
            if (stSourceAttr == 0x100 && job.attribute) stSourceAttr = job.attribute;
        }
        if (__atomic_load_n(&copyStopping, __ATOMIC_ACQUIRE)) return;
        if (job.status != kIOReturnSuccess) {
            // As a failed copy under the gate: the swap stays queued for the next kick. One retired at the start is gone, so the
            // next may go at once (a refused one is not tried again here, which could loop). Its surface holds nothing to show.
            if (doubleBuffer) ring.release();
            native_legacy_transfer_observed(job.status, job.request, 0);
            transferred = false;
            if (job.retired) kickSwapQueue();
            return;
        }
        if (hwSuspended) {
            presentDeferred = true;
            return;
        }  // the controller is unclocked: finishResume presents it
        presentCopied();
    }
    // Under the gate: the copied frame to the screen, then its swap retired (or, retired at the start, the next one released).
    void presentCopied()
    {
        const OriginalLegacySwapRequest *request = job.request;
        presentBlock();
        native_legacy_transfer_observed(kIOReturnSuccess, request, (uint64_t)job.width * job.height * 4);
        // Retired at the start, or gone from the queue while the gate was open (the family discarded it): nothing left to retire.
        if (job.retired || pendingSwapRequest() != request) {
            transferred = false;
            kickSwapQueue();
            return;
        }
        finishTransfer(request);
    }
    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy; anything else uses the CPU copy.
    void finishGpuSwap(int status)
    {
        if (gpuScanout) {
            finishGpuCopy(status);
            return;
        }
        const OriginalLegacySwapRequest *request = gpuRequest;
        if (!request) return;
        gpuRequest = nullptr;
        if (gpuRetired) {
            // The swap retired when the client took it; its copy ending only releases the next frame. A failed copy loses this frame
            // (the next one takes the CPU copy).
            gpuRetired = false;
            if (status == 0)
                native_legacy_transfer_observed(kIOReturnSuccess, request, gpuLength);
            else {
                native_legacy_early_failures = native_legacy_early_failures + 1;
                native_legacy_gpu_fallback_observed(status);
                gpuBypass = true;
            }
            transferred = false;
            kickSwapQueue();
            return;
        }
        if (status == 0) {
            native_legacy_transfer_observed(kIOReturnSuccess, request, gpuLength);
            finishTransfer(request, true);
            return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred = false;
        gpuBypass = true;
        kickSwapQueue();
    }
    // ioswitch-gpu-copy=1: after startHardwareRotation. The copy goes to the buffer window A does not scan, so it needs the second
    // buffer; the GPU driver may still decline any frame (the CPU copies that one).
    void startGpuCopy()
    {
        if (!bootFlag("ioswitch-gpu-copy") || !primary) return;
        if (!hwRotate || !doubleBuffer || !tegra_gm20b_scanout_copy) {
            IOLog("IOSwitch display: ioswitch-gpu-copy=1 ignored: it needs ioswitch-scanout-double=1, the hardware "
                  "rotation and TegraGM20B's "
                  "tegra_gm20b_scanout_copy%s\n",
                  tegra_gm20b_scanout_copy ? "" : " (not linked)");
            return;
        }
        IOWorkLoop *loop = getWorkLoop();
        gpuDoneSource = IOInterruptEventSource::interruptEventSource(this, &NativeLegacyDisplay::gpuDoneFired);
        gpuDeadline = IOTimerEventSource::timerEventSource(this, &NativeLegacyDisplay::gpuDeadlineFired);
        if (!loop || !gpuDoneSource || !gpuDeadline || loop->addEventSource(gpuDoneSource) != kIOReturnSuccess ||
            loop->addEventSource(gpuDeadline) != kIOReturnSuccess)
            panic("IOSwitch display: ioswitch-gpu-copy=1: cannot add the GPU copy's completion sources");
        gpuCopy = true;
    }
    // Outside the gate. The GPU would still write a buffer that goes away with the rotation, and call back: no stop with a copy out.
    void stopGpuCopy()
    {
        if (__atomic_load_n(&gpuScanout, __ATOMIC_ACQUIRE))
            panic("IOSwitch display: stopped with GPU frame copy %llu outstanding", gpuSequence);
        IOWorkLoop *loop = getWorkLoop();
        IOEventSource *sources[] = {gpuDoneSource, gpuDeadline};
        for (IOEventSource *source : sources)
            if (source && loop) loop->removeEventSource(source);
        if (gpuDeadline) gpuDeadline->cancelTimeout();
        OSSafeReleaseNULL(gpuDoneSource);
        OSSafeReleaseNULL(gpuDeadline);
        gpuCopy = false;
    }
    // Under the gate, in the kick, with a surface free: offer the swap's surface to the GPU for the buffer window A neither scans nor
    // may take (ring.target(), the one drawSurface() names; a flip may be out for another). Declined, the caller copies it on the CPU.
    // Accepted, the swap retires now unless sheng-early-retire=0 (the compositor renders its next frame, into its other surface,
    // while the GPU copies), and transferred holds the next swap until the completion has flipped window A.
    bool offerGpuCopy(const OriginalLegacySwapRequest *request, IOMemoryDescriptor *memory, uint64_t stride,
                      uint64_t allocation, uint32_t width, uint32_t height, uint64_t kickAt)
    {
        if (width != kNativeBlockWidth || height != kNativeBlockHeight || !doubleBuffer) return false;
        const int target = ring.target();
        if (target == kNoBuffer) return false;
        TegraScanoutCopy copy = {};
        copy.version = TEGRA_SCANOUT_COPY_VERSION;
        copy.width = width;
        copy.height = height;
        copy.sequence = gpuSequence + 1;
        copy.display = this;
        copy.source = memory;
        copy.sourceStride = stride;
        copy.sourceBytes = allocation;
        copy.target = surfaceAddress(target);
        copy.targetBytes = kNativeBlockBytes;
        copy.targetPitch = (uint32_t)kNativeBlockStride;
        copy.blockHeightLog2 = 4;  // kNativeBlockGobs, the DC's BLOCK_HEIGHT(4)
        memory->retain();
        if (tegra_gm20b_scanout_copy(&copy)) {
            memory->release();
            if (stats) ++stGpuDeclined;
            return false;
        }
        gpuSequence = copy.sequence;
        gpuSource = memory;
        gpuOfferedAt = mach_absolute_time();
        ring.claim(target);  // the GPU writes it until finishGpuCopy
        __atomic_store_n(&gpuScanout, true, __ATOMIC_RELEASE);
        gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
        native_legacy_swap_stage(1);
        shadowValid = false;
        gpuRequest = request;
        gpuLength = (uint64_t)width * height * 4;
        transferred = true;
        noteTaken(request, kickAt);
        if (bootEarlyRetire && retireSwap(request, true)) {
            gpuRetired = true;
            native_legacy_early_retires = native_legacy_early_retires + 1;
        }
        return true;
    }
    static void gpuDoneFired(OSObject *owner, IOInterruptEventSource *, int)
    {
        NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
        if (!__atomic_exchange_n(&self->gpuDonePending, false, __ATOMIC_ACQUIRE)) return;
        self->finishGpuSwap(__atomic_load_n(&self->gpuStatus, __ATOMIC_RELAXED));
    }
    static void gpuDeadlineFired(OSObject *owner, IOTimerEventSource *)
    {
        NativeLegacyDisplay *self = static_cast<NativeLegacyDisplay *>(owner);
        if (!self->gpuScanout) return;
        // A completion signalled but not handled yet (the gate was busy) only moves the deadline.
        if (__atomic_load_n(&self->gpuDonePending, __ATOMIC_ACQUIRE)) {
            self->gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
            return;
        }
        panic("IOSwitch display: GPU frame copy %llu into %#x not completed in %u ms (tegra_gm20b_scanout_copy "
              "accepted it)",
              self->gpuSequence, self->surfaceAddress(self->ring.drawing), kGpuCopyDeadlineMS);
    }
    // Under the gate: the GPU frame copy is done (status 0) or failed. Done, window A flips to it (after SC7: at the resume) and the swap
    // retires, or, retired at the offer, the next one may go. Failed, nothing is flipped: a swap still queued is copied on the CPU at
    // once, one retired at the offer is lost (the next frame replaces it), and the next kick uses the CPU copy either way.
    void finishGpuCopy(int status)
    {
        const OriginalLegacySwapRequest *request = gpuRequest;
        gpuDeadline->cancelTimeout();
        __atomic_store_n(&gpuScanout, false, __ATOMIC_RELEASE);
        gpuRequest = nullptr;
        OSSafeReleaseNULL(gpuSource);
        if (stats) {
            stGpuCopy.add(mach_absolute_time() - gpuOfferedAt);
            if (!status) ++stGpuCopies;
        }
        if (status) {
            native_legacy_gpu_fallback_observed(status);
            ring.release();  // the target may hold a partial frame: never flipped to
            gpuBypass = true;
            if (gpuRetired) {
                gpuRetired = false;
                native_legacy_early_failures = native_legacy_early_failures + 1;
            }
            transferred = false;
            kickSwapQueue();
            return;
        }
        gpuPresented = request;
        if (hwSuspended) {
            gpuPresentDeferred = true;
            return;
        }  // the controller is unclocked: finishResume presents it
        presentGpuCopy();
    }
    void presentGpuCopy()
    {
        const OriginalLegacySwapRequest *request = gpuPresented;
        gpuPresented = nullptr;
        presentBlock();  // the GPU's completion already put the frame in DRAM
        native_legacy_transfer_observed(kIOReturnSuccess, request, gpuLength);
        // Retired at the offer, or gone from the queue meanwhile (the family discarded it): nothing left to retire.
        if (gpuRetired || !request || pendingSwapRequest() != request) {
            gpuRetired = false;
            transferred = false;
            kickSwapQueue();
            return;
        }
        finishTransfer(request, true);
    }
    // Hands the pending head to the unchanged family's completion. Leaves transferred alone: the callers decide when the next frame may go.
    bool retireSwap(const OriginalLegacySwapRequest *request, bool force)
    {
        if (!(bootRetire || bootCpuPresent || force)) return false;
        if (!activateOnlyPendingSwap(request)) return false;
        const uint64_t submitted = stats ? submittedAt(request) : 0;
        removeValidated(request);
        IOMobileFramebufferLegacy::completeCurrentSwap();
        native_legacy_retirement_observed(this);
        if (stats) {
            const uint64_t now = mach_absolute_time();
            if (submitted && now > submitted) stRetire.add(now - submitted);
            lastRetire = now;
            ++stSwaps;
        }
        return true;
    }
    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    void finishTransfer(const OriginalLegacySwapRequest *request, bool force = false)
    {
        // Only after the real copy and store barrier: select the actual pending head and let the unchanged family release it and wake
        // waiters. This reports memory scanout completion, not a panel vblank or hardware fence.
        if (!retireSwap(request, force)) return;
        // The unchanged family has retired this generation and released its current reference. Permit the next independently validated
        // single pending frame. Keep the latch set while completion can reenter this method, and whenever retirement remains disabled.
        transferred = false;
        kickSwapQueue();
    }
    IOReturn collectCompletionReports(uint32_t, void *, uint32_t *count, uint32_t) override
    {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if (count) *count = 0;
        return kIOReturnUnsupported;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override
    {
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes = reinterpret_cast<const unsigned char *>(request);
        auto word = [&](unsigned offset) {
            uint32_t value = 0;
            memcpy(&value, bytes + offset, 4);
            return value;
        };
        OSObject *surface = nullptr;
        unsigned reason = 1;
        bool accepted = false;
        if (bootValidate && logicalPM && primary && bootMapping && request) {
            reason = 2;
            OSObject *surfaces[4] = {};
            memcpy(surfaces, bytes + 2624, sizeof(surfaces));
            surface = surfaces[0];
            uint32_t width = 0, height = 0;
            getDisplayDimensions(&width, &height);
            if (surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
                native_legacy_single_layer_state(word(792), word(796), word(800))) {
                reason = 3;
                bool rectangles = true;
                const unsigned rectOffsets[] = {17, 81};
                for (unsigned offset : rectOffsets) {
                    uint32_t rect[4] = {};
                    memcpy(rect, bytes + offset, sizeof(rect));
                    rectangles = rectangles && rect[0] == 0 && rect[1] == 0 && rect[2] == width && rect[3] == height;
                }
                bool options = true;
                for (unsigned offset = 804; offset <= 812; offset += 4) options = options && word(offset) == 0;
                if (width && height && rectangles && options && word(816) == width && word(820) == height) {
                    reason = 4;
                    IOMemoryDescriptor *memory = native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied.
                    if (native_legacy_surface_width(surface) >= width &&
                        native_legacy_surface_height(surface) >= height &&
                        nativeBgra(native_legacy_surface_format(surface), native_legacy_surface_planes(surface)) &&
                        memory && memory->metaCast("IOMemoryDescriptor") &&
                        native_legacy_surface_bounds(width, height, native_legacy_surface_stride(surface),
                                                     native_legacy_surface_allocation(surface), memory->getLength())) {
                        reason = 0;
                        accepted = true;
                    } else {
                        native_legacy_surface_rejected(
                            native_legacy_surface_width(surface), native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface), native_legacy_surface_planes(surface),
                            native_legacy_surface_stride(surface), native_legacy_surface_allocation(surface),
                            memory ? memory->getLength() : 0);
                    }
                }
            }
        }
        if (!accepted && reason == 2 && !surface && bootValidate) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4] = {};
            memcpy(none, bytes + 2624, sizeof(none));
            uint32_t width = 0, height = 0;
            getDisplayDimensions(&width, &height);
            bool options = true;
            for (unsigned offset = 804; offset <= 812; offset += 4) options = options && word(offset) == 0;
            if (!none[0] && !none[1] && !none[2] && !none[3] && width && height && options && word(816) == width &&
                word(820) == height && native_legacy_blank_state(word(792), word(796), word(800))) {
                reason = 5;
                accepted = true;
            }
        }
        if (accepted)
            noteSubmitted(request);
        else
            removeValidated(request);
        if (!accepted && stats) {
            __atomic_fetch_add(&stRejected, 1u, __ATOMIC_RELAXED);
            __atomic_store_n(&stRejectReason, reason, __ATOMIC_RELAXED);
        }
        native_legacy_swap_validation_observed(accepted, reason, request, surface);
        return accepted;
    }
    // An accepted swap: remembered for the kick, and for the stats its submission time, the interval since the previous one and the
    // compositor's own time since the last retire (0 when the previous swap was still on its way).
    void noteSubmitted(const OriginalLegacySwapRequest *request)
    {
        const uint64_t now = mach_absolute_time();
        if (!addValidated(request, now) || !stats) return;
        if (lastSubmit) {
            stInterval.add(now - lastSubmit);
            stClient.add(lastRetire > lastSubmit && now > lastRetire ? now - lastRetire : 0);
        }
        lastSubmit = now;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override
    {
        if (!pitch) return kIOReturnBadArgument;
        *pitch = 0;
        IOService *provider = getProvider();
        if (!provider) return kIOReturnNotReady;
        OSObject *property = provider->getProperty("dot-pitch");
        if (!property) return kIOReturnNotFound;
        OSData *data = OSDynamicCast(OSData, property);
        if (!data || data->getLength() != sizeof(*pitch)) return kIOReturnBadArgument;
        memcpy(pitch, data->getBytesNoCopy(), sizeof(*pitch));
        return kIOReturnSuccess;
    }
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    IOReturn supportedFrameInfo(uint32_t, uint32_t *count, char *key, uint32_t *mask) override
    {
        if (!count || !key || !mask) return kIOReturnBadArgument;
        *count = 0;
        *mask = 0;
        bzero(key, 64);
        return kIOReturnUnsupported;
    }
    IOReturn newUserClient(task_t task, void *security, UInt32 type, IOUserClient **client) override
    {
        native_legacy_client_enter(task, type);
        IOReturn status = IOMobileFramebufferLegacy::newUserClient(task, security, type, client);
        native_legacy_client_observed(status, status == kIOReturnSuccess && client ? *client : nullptr);
        if (status == kIOReturnSuccess && client && *client && primary && displaySleepEnabled())
            answerPowerRequests(*client);
        return status;
    }
    // Display power requests. The render server blanks the panel with IOMobileFramebufferRequestPowerChange (QuartzCore; user-client
    // selector 12, one scalar: 0 off, else on). The original client passes it to the family only when the framebuffer's power-capable
    // byte (+0x8c) is set, as a platform subclass would, and that byte also makes every swap fail while the platform power word
    // (+0x16d4) is 0, which nothing here writes; so that path stays closed (it answers kIOReturnUnsupported) and the family's powered
    // state stays as announcePoweredByFirmware left it. Instead each client's externalMethod (slot 168, as LegacyDisplayProbe's
    // interposer, which this wraps when it ran first) is given a wrapper that hands selector 12 to SwitchBrightness; the compositor stops
    // presenting while it believes the display is off. The vtable copy keeps the wrapped entry one past its end; like the probe's copies
    // it lives as long as the kernel (one per client of the main display).
    static constexpr unsigned kClientSlots = 256, kExternalMethodSlot = 168, kRequestPowerChange = 12;
    typedef IOReturn (*ExternalMethod)(IOUserClient *, uint32_t, IOExternalMethodArguments *,
                                       IOExternalMethodDispatch *, OSObject *, void *);
    static IOReturn powerExternal(IOUserClient *client, uint32_t selector, IOExternalMethodArguments *args,
                                  IOExternalMethodDispatch *dispatch, OSObject *target, void *ref)
    {
        if (selector == kRequestPowerChange && args && args->scalarInput && args->scalarInputCount == 1 &&
            switch_display_power_request(args->scalarInput[0] != 0))
            return kIOReturnSuccess;
        const uintptr_t original = (*reinterpret_cast<uintptr_t *const *>(client))[kClientSlots];
        return reinterpret_cast<ExternalMethod>(original)(client, selector, args, dispatch, target, ref);
    }
    // Unless switch-sleep=0 (SwitchBrightness.h switch_display_power_enabled, the same rule): with it the clients keep their original
    // vtables, exactly as before display power existed.
    static bool displaySleepEnabled()
    {
        unsigned enabled = 1;
        PE_parse_boot_argn("switch-sleep", &enabled, sizeof(enabled));
        return enabled != 0;
    }
    static void answerPowerRequests(IOUserClient *client)
    {
        const uintptr_t *table = *reinterpret_cast<uintptr_t *const *>(client);
        uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc((kClientSlots + 1) * sizeof(uintptr_t)));
        if (!copy) return;
        memcpy(copy, table, kClientSlots * sizeof(uintptr_t));
        copy[kClientSlots] = copy[kExternalMethodSlot];
        copy[kExternalMethodSlot] = reinterpret_cast<uintptr_t>(&powerExternal);
        __asm__ volatile("dmb ish" ::: "memory");
        *reinterpret_cast<uintptr_t **>(client) = copy;
    }
    bool startHardware() override
    {
        unsigned enabled = 0;
        PE_parse_boot_argn("offline-legacy-gop", &enabled, sizeof(enabled));
        if (enabled != 1) return false;
        PE_Video video = {};
        IOService *provider = getProvider();
        if (!provider || PE_current_console(&video) || !native_legacy_video_mode(video)) return false;
        IOMemoryMap *mapping = provider->mapDeviceMemoryWithIndex(0, kIOMapWriteCombineCache);
        if (!mapping) return false;
        bool valid = mapping->getVirtualAddress() && mapping->getPhysicalAddress() == video.v_baseAddr &&
                     mapping->getLength() == video.v_rowBytes * video.v_height;
        if (valid) {
            installBootDimensions(native_legacy_video_width(video), native_legacy_video_height(video));
            uint32_t width = 0, height = 0;
            getDisplayDimensions(&width, &height);
            valid = width == native_legacy_video_width(video) && height == native_legacy_video_height(video);
            if (valid) {
                OSSafeReleaseNULL(bootMapping);
                bootMapping = mapping;
                mapping = nullptr;
                // The Switch boots its panel in text mode (v_display=0); it is still the one built-in display.
                primary = true;
                valid = native_legacy_gop_mapping_observed(this, bootMapping, width, height);
                if (valid && IOSWITCH_PORTRAIT_NATIVE) publishStrideRequirements();
            }
        }
        OSSafeReleaseNULL(mapping);
        return valid;
    }
    // The compositor's framebuffer rows (QuartzCore CA::WindowServer::AppleDisplay::update_display_limits reads this IOMFB
    // property; iosurface_alignment() then rounds every framebuffer's bytesPerRow up to it, 64 by default). The GPU renders
    // into a linear surface in place only when its rows are 128-byte multiples (Mesa nil image.rs; below that NVK renders
    // through a tiled shadow and copies it back every pass). 1280 x 4 = 5120 was one; 720 x 4 = 2880 is not, so ask
    // 128: 2944-byte rows. The scanout copies take any source stride.
    void publishStrideRequirements()
    {
        OSDictionary *requirements = OSDictionary::withCapacity(1);
        OSNumber *horizontal = OSNumber::withNumber(128ull, 32);
        if (requirements && horizontal && requirements->setObject("StrideLinearHorizontal", horizontal))
            setProperty("DisplayPipeStrideRequirements", requirements);
        OSSafeReleaseNULL(horizontal);
        OSSafeReleaseNULL(requirements);
    }
#define LEAF(n)                                                            \
    __attribute__((noreturn, noinline)) void opaque##n() override          \
    {                                                                      \
        native_legacy_backend_guard(n, this, __builtin_return_address(0)); \
    }
    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
    LEAF(345)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,
                              IOMobileFramebufferLegacy) NativeLegacyDisplay *NativeLegacyDisplay::primaryDisplay;
// SwitchBrightness's display power sequence (SwitchBrightness.h): from its work loop, after the panel is asleep and before it wakes.
extern "C" bool native_legacy_display_set_scanout(bool on)
{
    NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    IOWorkLoop *loop = display ? display->getWorkLoop() : nullptr;
    if (!loop) return false;
    return loop->runAction(
               [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
                   return static_cast<NativeLegacyDisplay *>(owner)->setScanout(arg != nullptr) ? kIOReturnSuccess
                                                                                                : kIOReturnNotReady;
               },
               display, on ? display : nullptr) == kIOReturnSuccess;
}
// SC7 hooks for the sleep code; contract in TegraDisplayPower.h. The panel sleeps and wakes with TegraPanel's own power sequence
// (display power's), the controller stops and restarts with setScanout, and the pipeline itself goes down and comes up cold in
// TegraDisplayPower.cpp; the steps that touch window A and the interrupt run on the display's work loop.
template <typename Body> static bool onDisplayLoop(NativeLegacyDisplay *display, Body body)
{
    bool result = false;
    struct Call {
        Body *body;
        bool *result;
    } call = {&body, &result};
    display->getWorkLoop()->runAction(
        [](OSObject *owner, void *a0, void *, void *, void *) -> IOReturn {
            Call *c = static_cast<Call *>(a0);
            *c->result = (*c->body)(static_cast<NativeLegacyDisplay *>(owner));
            return kIOReturnSuccess;
        },
        display, &call);
    return result;
}
static NativeLegacyDisplay *sc7Display(const char *step)
{
    NativeLegacyDisplay *display = __atomic_load_n(&NativeLegacyDisplay::primaryDisplay, __ATOMIC_ACQUIRE);
    if (!display || !display->getWorkLoop()) panic("IOSwitch display: %s before the display started", step);
    const uint32_t panel = tegra_display_panel_id();
    if (panel != TEGRA_DISPLAY_PANEL_AMS699VC01)
        panic("IOSwitch display: %s: panel %#x has no cold init here (only the OLED's AMS699VC01, %#x)", step, panel,
              TEGRA_DISPLAY_PANEL_AMS699VC01);
    return display;
}
extern "C" void tegra_display_suspend(void)
{
    NativeLegacyDisplay *display = sc7Display("tegra_display_suspend");
    const bool lit = onDisplayLoop(display, [](NativeLegacyDisplay *d) { return d->beginSuspend(); });
    if (lit && tegra_panel_power_off())
        IOLog("IOSwitch display: SC7 panel power-off failed: the display is not running\n");
    onDisplayLoop(display, [](NativeLegacyDisplay *d) {
        d->quiesceForSuspend();
        return true;
    });
    IOSleep(20);  // DC A stops at the end of the frame it was scanning
    tegra_display_power_down();
}
extern "C" void tegra_display_resume(void)
{
    NativeLegacyDisplay *display = sc7Display("tegra_display_resume");
    tegra_display_power_up();
    const bool lit = onDisplayLoop(display, [](NativeLegacyDisplay *d) { return d->finishResume(); });
    // Its first step waits for a frame end of the restarted DC A (a lost DC A interrupt panics there, naming the GIC state).
    if (lit && tegra_panel_power_on(-1))
        IOLog("IOSwitch display: SC7 panel power-on failed: the display is not running\n");
}
IOService *native_legacy_display_allocate(void)
{
    return new NativeLegacyDisplay;
}
extern "C" void native_legacy_gpu_complete(void *display, int status)
{
    if (display) static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);
}
