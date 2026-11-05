// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio: the kernel half of the Switch's iPadOS audio device. iPadOS 17's audio server (audiomxd) loads our
// AudioServerPlugIn (hal/SwitchAudioPlugIn.c); the plug-in opens this service, maps its control page and two sample
// rings (SwitchAudioAbi.h) and asks it to start and stop each direction. Design after UniXNU-sheng's
// package/ShengAudioService.cpp, the iPadOS 17.7.11 audio path that is known to play on real hardware.
//
// Backends, chosen by boot-arg switch-audio (default 0):
//   0  null: a 10 ms timer advances the play position on the host clock and clears what it passed; capture is
//      silence; no Tegra hardware is touched. The timer runs only while a stream does.
//   1  Tegra I2S playback (speaker and headphone jack); capture stays silence on the hardware's clock.
//   3  also Tegra capture (headset microphone).
//   +4 (5, 7) the speaker only, whatever the jack says (to tell a wrong jack reading from a silent speaker path).
// switch-audio-stats=1 (Tegra) adds one line every 5 s: streams started, frames played, underruns, the loudest sample, the
// DMA, I2S and jack state and the codec's power and volume registers. The register fields are read at the moment of the
// line: between streams the I2S is off (en 0) and, 5 s after the last one, the codec's power registers are 0.
// The Tegra backend is interrupt driven throughout: the ADMA period interrupt moves the positions, the codec's IRQ
// line reports the jack, and an interrupt that does not arrive, a DMA that stalls or runs fast, or hardware that does
// not answer, panics with one line.
//
// The OLED's audio path. Sources: the shipped DTB (switchroot/ioswitch/oled-sku3.dtb = CTCaer
// switch-l4t-platform-t210-nx tegra210b01-fric.dts over tegra210b01-nx-common.dtsi:864-950), switchroot Android
// android_device_nintendo_nx media/nvaudio_conf.xml (the mixer paths), CTCaer switch-l4t-kernel-nvidia
// sound/soc/codecs/rt5640.c and sound/soc/tegra-alt (tegra_machine_driver_mobile.c, tegra_asoc_utils_alt.c,
// tegra210_i2s_alt.c, tegra210-adma.c), CTCaer switch-l4t-kernel-4.9 drivers/clk/tegra, mainline sound/soc/tegra
// (tegra210_ahub.c, tegra210_admaif.c, tegra210_i2s.c) and pinctrl-tegra210.c.
//  - Codec: Realtek RT5639 (rt5640 family, VENDOR_ID2 0x6231) on I2C1 at 0x1c. Its own class-D amplifier drives the
//    speakers (SPOLP/N, SPORP/N; spkvdd = vdd-ac-bat) and its HP amplifier the jack (HPOL/R); there is no external
//    amplifier. The only microphone is a headset's, on IN1P (single ended). LDO1_EN = PZ4 (high = on, then 400 ms);
//    the codec's GPIO1 is its IRQ output, on PBB4; jack detect is the codec's own (realtek,sel-jd-source 4 = its
//    GPIO2, jack-detect-is-jd1, inverted: JD_STATUS 0 = plugged). The same headphone-detect net reaches Tegra PV6
//    (the DT's jack-detect-gpios, active low; tegra210b01-fric-pinmux.dtsi touch_rst_pv6 "Audio Codec (ALC5639)
//    Headphone Detect": rsvd0, no pull, tristate, input). rt5640.c never reads PV6, but its pad is on the net, so it is
//    muxed as L4T leaves it before the codec is read; its level only goes into the diagnostics here.
//    Microphone vs plain headphones: MICBIAS1 over-current at 600 uA.
//  - Link: Tegra I2S1 (AHUB 0x702d1000; CAR clock "i2s0") is bit and frame clock master, the codec slave
//    (dai-link-1: format i2s, no bitclock-master), stereo, 48 kHz. Samples are S32_LE from memory to the I2S
//    (SwitchAudioFormat.h): 32-bit slots, BCLK 64 fs, and the codec reads 24 bits of each (I2S1 data length 24, BCLK
//    64 fs), as rt5640_hw_params sets it for 24-bit samples in a 64-bit frame. (The DTB's dai-link says s16_le, BCLK
//    32 fs: what L4T and this driver ran until r17-audio.) MCLK 256 fs = 12.288 MHz (mclk-fs 256) on AUD_MCLK = PMC
//    clk_out_1 <- extern1 <- PLLA_OUT0 (clk-tegra210.c init table); the codec runs from MCLK directly (sysclk source
//    MCLK, pre-divider 1). Pins DAP1_FS/DIN/DOUT/SCLK (function i2s1), AUD_MCLK.
//  - Clocks for 48 kHz (tegra_asoc_utils_alt.c): PLLA 368.64 MHz (T210B01 table, clk-tegra210b01.c:1789-1793:
//    38.4 MHz ref, m 2, n 38, p 2, SDM 0xfccc, really 368639062.5 Hz), PLLA_OUT0 49.152 MHz, AHUB (d_audio)
//    = PLLA_OUT0, I2S 3.072 MHz (64 fs), extern1 12.288 MHz, APE from PLLP at 25.5 MHz. The real frame rate is
//    therefore 47999.878 Hz, and ticks_per_frame_q32 says so.
//  - Routing: XBAR "I2S1 Mux" = ADMAIF1 (playback), "ADMAIF1 Mux" = I2S1 (capture), as nvaudio_conf.xml sets them;
//    ADMA request line 1 = ADMAIF1 (DTB admaif dmas). ADSP and the AHUB effect blocks are not used.
//  - Interrupts: ADMA channel n raises SPI 24+n on the AGIC, the APE's own GICv2 (distributor 0x702f9000, CPU
//    interface 0x702fa000; DTB adma@702e2000 interrupts), whose host output is main-GIC SPI 102 = INTID 134 (DTB
//    agic@702f9000 interrupts <0 0x66 ..>). This driver claims ioswitch_interrupt_nub(134, false) and runs the
//    cascade: the channel's AGIC SPI targeted at the interface the CPU reads in its banked ITARGETSR0, IAR/EOIR on the
//    AGIC, ADMA_CH_INT_STATUS/CLEAR. Channels 0 (playback) and 1 (capture) are host channels.
//
// The ADMA plays the playback ring and fills the capture ring in place: one cycle of 8 periods (the most an ADMA
// cycle holds) is exactly one ring, so a period is 2048 frames (42.7 ms, 16 KiB of S32 stereo). Each period interrupt
// clears the playback period just consumed (so the next turn mixes onto silence) and invalidates the capture period
// just written, then moves out_frames/in_frames on. The ADMA does not snoop the caches: the plug-in cleans what it
// mixes, this driver cleans what it clears and invalidates what the ADMA wrote. Sample time 0 is the host time the
// playback DMA started.
//
// Started by ioswitch_audio_publish() from the board thread; the classes register through the normal static
// initializer (they derive only from XNU's own IOService and IOUserClient).
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "SwitchAudioAbi.h"
#include "SwitchAudioFormat.h"
#include "../ArmGIC/IOSwitchInterrupts.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraI2C/TegraI2C.h"

extern "C" {
extern task_t kernel_task;
vm_offset_t phystokv(uint64_t pa);
uint64_t pmap_find_pa(pmap_t pmap, uint64_t va);
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
// Defined in TegraGPIO.cpp (output level, then GPIO mode; masked writes); TegraGPIO.h does not declare it yet.
int tegra_gpio_output(unsigned gpio, int value);
}

namespace {
constexpr uint32_t kOutBytes = SWITCH_AUDIO_RING_FRAMES * SWITCH_AUDIO_OUT_CHANNELS * sizeof(switch_audio_sample_t);
constexpr uint32_t kInBytes = SWITCH_AUDIO_RING_FRAMES * SWITCH_AUDIO_IN_CHANNELS * sizeof(switch_audio_sample_t);
constexpr uint32_t kTickMS = SWITCH_AUDIO_PERIOD_FRAMES * 1000 / SWITCH_AUDIO_RATE;
// Below 4 GiB and physically contiguous, so the ADMA plays the rings in place (32-bit addresses).
constexpr mach_vm_address_t kDmaMask = 0x00000000fffff000ULL;

uint64_t timerHz()
{
    mach_timebase_info_data_t tb;
    clock_timebase_info(&tb);
    return 1000000000ULL * tb.denom / tb.numer;
}

// CoreAudio assumes a 24 MHz host clock (Apple's timer): AudioGetHostClockFrequency and the audio server's IO thread
// read one double constant in the dyld shared cache. This board's timer runs at 19.2 MHz (ADT /cpus/cpu0
// timebase-frequency), so without this the audio server's IO cycles run 25 % long (UniXNU-sheng, sheng.md). The
// constant sits on a read-only shared page, so the caller cannot write it; this rewrites that physical page once for
// every process. Only a value of exactly 24 MHz (or ours already) at an address inside the shared region is touched.
IOReturn setHostClock(task_t task, uint64_t address)
{
    constexpr uint64_t kApple = 0x4176e36000000000ULL;  // 24000000.0
    if ((address & 7) || address < 0x180000000ULL || address >= 0x400000000ULL) return kIOReturnBadArgument;
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, 8, kIODirectionOut, task);
    if (!memory) return kIOReturnNoMemory;
    IOReturn result = memory->prepare();  // wires the page while we write it
    if (result != kIOReturnSuccess) {
        memory->release();
        return result;
    }
    uint64_t seen = 0;
    uint64_t pa = copyin(address, &seen, sizeof(seen)) ? 0 : pmap_find_pa(get_task_pmap(task), address);
    // The timer frequency as an IEEE double, from the integer (no floating point in the kernel).
    const uint64_t hz = timerHz();
    const unsigned top = 63 - (unsigned)__builtin_clzll(hz);
    const uint64_t ours = (uint64_t)(1023 + top) << 52 | ((hz << (52 - top)) & ((1ULL << 52) - 1));
    result = pa ? kIOReturnUnsupported : kIOReturnNotReadable;
    if (pa) {
        volatile uint64_t *value = reinterpret_cast<volatile uint64_t *>(phystokv(pa));
        if (*value == seen && (seen == kApple || seen == ours)) {
            *value = ours;
            __asm__ volatile("dsb sy" ::: "memory");
            result = kIOReturnSuccess;
        }
    }
    memory->complete();
    memory->release();
    return result;
}

// ---- Tegra backend constants -----------------------------------------------------------------------------------
constexpr unsigned kPeriods = 8;                                         // ADMA_CH_CONFIG_MAX_BUFS
constexpr uint32_t kPeriodFrames = SWITCH_AUDIO_RING_FRAMES / kPeriods;  // 2048
constexpr uint32_t kOutPeriodBytes = kOutBytes / kPeriods, kInPeriodBytes = kInBytes / kPeriods;
static_assert(kPeriodFrames * kPeriods == SWITCH_AUDIO_RING_FRAMES, "a ring is whole ADMA periods");
constexpr uint32_t kWatchdogMS = 4 * kPeriodFrames * 1000 / SWITCH_AUDIO_RATE;  // 170 ms: 4 periods
constexpr uint32_t kGraceMS = 50;        // after a missed deadline, for an interrupt action already queued
constexpr uint32_t kIdleMS = 5000;       // codec power-down after the last stream stops (ASoC pmdown_time)
constexpr uint32_t kJackSettleMS = 250;  // jack contact bounce
constexpr uint32_t kStatsMS = 5000;      // switch-audio-stats line
constexpr uint64_t kSettlePeriods = 2;   // switch-audio-stats: periods after a start left out of lead and lag
constexpr unsigned kChOut = 0, kChIn = 1;
constexpr uint32_t kAgicAdma = 32 + 24;  // AGIC INTID of ADMA channel 0
constexpr uint32_t kCascadeIntid = 32 + 102;

// Register blocks (T210 TRM map; hekate bdk/soc/t210.h).
constexpr vm_offset_t kCarPA = 0x60006000, kPmcPagePA = 0x7000e000, kPinmuxPA = 0x70003000, kAhubPA = 0x702d0000,
                      kAdmaPA = 0x702e2000, kAgicPA = 0x702f9000;
constexpr uint32_t kPmcOffset = 0x400;  // APBDEV_PMC 0x7000e400

inline uint32_t r32(volatile uint8_t *base, uint32_t o)
{
    return *(volatile uint32_t *)(base + o);
}
inline void w32(volatile uint8_t *base, uint32_t o, uint32_t v)
{
    *(volatile uint32_t *)(base + o) = v;
}

// A register handshake without an interrupt of its own (reset done, channel idle, PLL lock): microseconds.
bool waitBits(volatile uint8_t *base, uint32_t reg, uint32_t mask, uint32_t want, uint32_t us)
{
    uint64_t deadline;
    clock_interval_to_deadline(us, kMicrosecondScale, &deadline);
    while ((r32(base, reg) & mask) != want) {
        if (mach_absolute_time() > deadline) return (r32(base, reg) & mask) == want;
    }
    return true;
}

// The ADMA does not snoop: clean (CPU -> DRAM) before it reads, clean+invalidate after it wrote. 64-byte lines (A57).
void cacheClean(const void *start, size_t bytes)
{
    for (uintptr_t at = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + bytes; at < end; at += 64)
        __asm__ volatile("dc cvac, %0" ::"r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
void cacheFlush(const void *start, size_t bytes)
{
    for (uintptr_t at = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + bytes; at < end; at += 64)
        __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// CLK_RST_CONTROLLER (hekate bdk/soc/clock.h; Linux clk-tegra-periph.c, clk-tegra210b01.c).
enum : uint32_t {
    RST_DEVICES_L = 0x4,
    CLK_OUT_ENB_L = 0x10,
    OSC_CTRL = 0x50,
    PLLA_BASE = 0xb0,
    PLLA_OUT = 0xb4,
    PLLA_MISC1 = 0xb8,
    PLLA_MISC0 = 0xbc,
    PLLA_MISC2 = 0x5d8,
    CLK_SOURCE_I2S0 = 0x1d8,
    CLK_SOURCE_D_AUDIO = 0x3d0,
    CLK_SOURCE_EXTERN1 = 0x3ec,
    CLK_SOURCE_APE = 0x6c0,
    CLK_ENB_L_SET = 0x320,
    RST_DEV_L_CLR = 0x304,
    CLK_ENB_V_SET = 0x440,
    RST_DEV_V_CLR = 0x434,
    CLK_ENB_Y_SET = 0x29c,
    RST_DEV_Y_CLR = 0x2ac,
};
// Clock numbers: i2s0 30 (L), d_audio 106, apb2ape 107, extern1 120 (V), ape 198 (Y).
constexpr uint32_t kClkL = 1u << 30, kClkV = (1u << 10) | (1u << 11) | (1u << 24), kClkY = 1u << 6;
enum : uint32_t {
    PLL_BYPASS = 1u << 31,
    PLL_ENABLE = 1u << 30,
    PLL_LOCK = 1u << 27,
    PLLA_IDDQ = 1u << 25,
    PLLA_MNP_MASK = 0x01f0ffff,     // plla_nmp: m 7:0, n 15:8, p 24:20
    PLLA_LOCK_ENABLE = 0x10000000,  // PLLA_MISC0_DEFAULT_VALUE
    PLLA_EN_SDM = 1u << 26,         // MISC2
};
// PLLA = 38.4 MHz / m * (n + 1/2 + sdm / 8192) / p (clk-pll.c:653); p 2 is hardware 1 (pll_qlin_pdiv_to_hw).
constexpr uint32_t kPllaM = 2, kPllaN = 38, kPllaP = 2, kPllaPHw = 1, kPllaSdm = 0xfccc;
constexpr uint32_t kPllaMnp = (kPllaPHw << 20) | (kPllaN << 8) | kPllaM;
constexpr int64_t kPllaNEff = 8192 * (int64_t)kPllaN + 4096 + (int64_t)(int16_t)kPllaSdm;  // n_eff * 8192 = 314572
// 7.1 dividers (value = 2 * ratio - 2): PLLA_OUT0 = PLLA / 7.5, I2S = PLLA_OUT0 / 16 (the 64 fs bit clock),
// extern1 = PLLA_OUT0 / 4, APE = PLLP 408 MHz / 16. PLLA_OUT0 is 1024 fs.
constexpr uint32_t kPllaOut0X2 = 15, kPllaOut0PerFs = 1024;
constexpr uint32_t kPllaOut0Div = kPllaOut0X2 - 2, kI2sDiv = switch_audio_i2s_divider(kPllaOut0PerFs), kExtern1Div = 6,
                   kApeSource = (4u << 29) | 30;
static_assert(kI2sDiv == 30, "I2S0 = PLLA_OUT0 / 16");

// APBDEV_PMC (hekate bdk/soc/pmc_t210.h; mainline pmc.c tegra210_io_pads/io_pad_vctrls).
enum : uint32_t {
    PWRGATE_STATUS = 0x38,
    NO_IOPOWER = 0x44,
    PWR_DET = 0x48,
    PWR_DET_VAL = 0xe4,
    CLK_OUT_CNTRL = 0x1a8,
    IO_DPD_STATUS = 0x1bc,
    IO_DPD2_STATUS = 0x1c4,
};
constexpr uint32_t kPartitionAud = 1u << 27;          // POWER_RAIL_AUD
constexpr uint32_t kPad33V = (1u << 5) | (1u << 18);  // audio, audio-hv: 1 = 3.3 V
constexpr uint32_t kNoIoPowerAudioHv = 1u << 18;
constexpr uint32_t kClkOut1Mask = (3u << 6) | (1u << 2), kClkOut1Extern1 = (3u << 6) | (1u << 2);  // src 3, force on

// Pinmux (PINMUX_AUX at 0x70003000, offsets pinctrl-tegra210.c minus 0x3000): function 1:0, pull 3:2 (1 down),
// tristate 4, park 5, input 6. Values from the DTB pinmux node; the park bit is cleared as Linux does on T210.
struct Pin {
    uint16_t offset;
    uint8_t value;
};
constexpr Pin kPins[] = {
    {0x124, 0x00},  // dap1_fs_pb0: i2s1, driven
    {0x128, 0x54},  // dap1_din_pb1: i2s1, pull-down, tristate, input
    {0x12c, 0x00},  // dap1_dout_pb2: i2s1, driven
    {0x130, 0x00},  // dap1_sclk_pb3: i2s1, driven
    {0x180, 0x00},  // aud_mclk_pbb0: aud (clk_out_1), driven
    {0x190, 0x54},  // gpio_x3_aud_pbb4: codec IRQ, pull-down, input
    {0x214, 0x50},  // touch_rst_pv6: the headphone-detect net, no pull, tristate, input
    {0x28c, 0x05},  // pz4: rsvd1, pull-down, driven (codec LDO1_EN)
};
// 10-31 (every boot through r3v): with nothing in the jack INT_IRQ_ST read 0 at start and at every stats line, JD_STATUS
// and GPIO2_STATUS both low, so every stream went to the headphone amplifier. Nothing in the boot chain (hekate,
// U-Boot's pinmux table, TegraGPIO) muxed PV6, which sits on that net; L4T sets it as above. The value found is logged
// with the first jack reading.
constexpr uint32_t kPinJackDetect = 0x214;
constexpr uint32_t kPinFields = 0x7f;

// AHUB block 0x702d0000: ADMAIF +0 (mainline tegra210_admaif.h), XBAR +0x800 (one mux per destination, a bit per
// source, three parts 0x200 apart: MUX_REG/MUX_VALUE in tegra210_ahub.h), I2S1 +0x1000 (tegra210_i2s.h).
enum : uint32_t {
    ADMAIF_RX_ENABLE = 0x000,
    ADMAIF_RX_SOFT_RESET = 0x004,
    ADMAIF_RX_STATUS = 0x00c,
    ADMAIF_RX_CIF = 0x020,
    ADMAIF_TX_ENABLE = 0x300,
    ADMAIF_TX_SOFT_RESET = 0x304,
    ADMAIF_TX_STATUS = 0x30c,
    ADMAIF_TX_CIF = 0x320,
    ADMAIF_GLOBAL_ENABLE = 0x700,
    XBAR_ADMAIF1 = 0x800,
    XBAR_I2S1 = 0x840,
    XBAR_PART = 0x200,
    I2S_RX_ENABLE = 0x1000,
    I2S_RX_SOFT_RESET = 0x1004,
    I2S_RX_STATUS = 0x100c,
    I2S_RX_CIF = 0x1020,
    I2S_RX_CTRL = 0x1024,
    I2S_TX_ENABLE = 0x1040,
    I2S_TX_SOFT_RESET = 0x1044,
    I2S_TX_STATUS = 0x104c,
    I2S_TX_CIF = 0x1060,
    I2S_TX_CTRL = 0x1064,
    I2S_ENABLE = 0x1080,
    I2S_CTRL = 0x10a0,
    I2S_TIMING = 0x10a4,
};
constexpr uint32_t kXbarFromAdmaif1 = 1u << 0, kXbarFromI2s1 = 1u << 16;  // MUX_VALUE(0, 0), MUX_VALUE(0, 16)
// The S32 link's words (SwitchAudioFormat.h, as L4T's hw_params build them for S32_LE): audio CIFs 32/32 bits, the
// ADMAIF unpacked (one sample per word), I2S1 master in LRCK frames with 32-bit slots (TIMING 31: half a 64 fs frame),
// data offset 1 (I2S).
constexpr uint32_t kI2sCtrl = switch_audio_i2s_ctrl(), kI2sTiming = switch_audio_i2s_timing(), kI2sDataOffset = 1u << 8;
static_assert(kI2sCtrl == 0x1f000407 && kI2sTiming == 31, "I2S1: 32-bit slots, 64 fs");

// ADMA (tegra210-adma.c, L4T): channel registers at 0x80 per channel, globals at 0xc00.
enum : uint32_t {
    CH_CMD = 0x00,
    CH_STATUS = 0x0c,
    CH_INT_STATUS = 0x10,
    CH_INT_CLEAR = 0x1c,
    CH_CTRL = 0x24,
    CH_CONFIG = 0x28,
    CH_FIFO_CTRL = 0x2c,
    CH_TC_STATUS = 0x30,
    CH_SRC = 0x34,
    CH_TRG = 0x3c,
    CH_TC = 0x44,
    CH_XFER_STATUS = 0x54,
    CH_STRIDE = 0x80,
    GLOBAL_CMD = 0xc00,
    GLOBAL_SOFT_RESET = 0xc04,
    GLOBAL_CG = 0xc08,
    GLOBAL_INT_CLEAR = 0xc20,
};
// CTRL: request 1 (ADMAIF1) at 28 (tx) / 24 (rx), direction MEM2AHUB 4 / AHUB2MEM 2 at 12, continuous mode, flow
// control. CONFIG: 8 buffers at 28 (src) / 24 (trg), burst fls(16) = 5 at 20, WRR weight 1. FIFO_CTRL: size 3
// (request <= 2) at 8 (tx) / 0 (rx).
constexpr uint32_t kChCtrl[2] = {(1u << 28) | (4u << 12) | (2u << 8) | (1u << 1),
                                 (1u << 24) | (2u << 12) | (2u << 8) | (1u << 1)};
constexpr uint32_t kChConfig[2] = {(7u << 28) | (5u << 20) | 1u, (7u << 24) | (5u << 20) | 1u};
constexpr uint32_t kChFifo[2] = {3u << 8, 3u};

// AGIC (GICv2: distributor at +0, CPU interface at +0x1000).
enum : uint32_t {
    GICD_CTLR = 0x000,
    GICD_ISENABLER = 0x100,
    GICD_ICENABLER = 0x180,
    GICD_ISPENDR = 0x200,
    GICD_ICPENDR = 0x280,
    GICD_ISACTIVER = 0x300,
    GICD_IPRIORITYR = 0x400,
    GICD_ITARGETSR = 0x800,
    GICD_ICFGR = 0xc00,
    GICC_CTLR = 0x1000,
    GICC_PMR = 0x1004,
    GICC_IAR = 0x100c,
    GICC_EOIR = 0x1010,
};

// ---- RT5639 -----------------------------------------------------------------------------------------------------
constexpr unsigned kCodecBus = 1;
constexpr uint8_t kCodecAddr = 0x1c;
constexpr unsigned kCodecPower = TEGRA_GPIO(Z, 4), kCodecIrq = TEGRA_GPIO(BB, 4);
constexpr unsigned kJackDetect = TEGRA_GPIO(V, 6);  // the headphone-detect net, 0 = plugged; diagnostics only
// Registers (rt5640.h). Table entries at kPrivate + n are private registers (PRIV_INDEX n, then PRIV_DATA).
enum : uint8_t {
    RESET = 0x00,
    SPK_VOL = 0x01,
    HP_VOL = 0x02,
    IN1_IN2 = 0x0d,
    DAC1_DIG_VOL = 0x19,
    STO_ADC_MIXER = 0x27,
    STO_DAC_MIXER = 0x2a,
    REC_L2_MIXER = 0x3c,
    REC_R2_MIXER = 0x3e,
    HPO_MIXER = 0x45,
    SPK_L_MIXER = 0x46,
    SPK_R_MIXER = 0x47,
    SPO_L_MIXER = 0x48,
    SPO_R_MIXER = 0x49,
    SPO_CLSD_RATIO = 0x4a,
    OUT_L3_MIXER = 0x4f,
    OUT_R3_MIXER = 0x52,
    PWR_DIG1 = 0x61,
    PWR_DIG2 = 0x62,
    PWR_ANLG1 = 0x63,
    PWR_ANLG2 = 0x64,
    PWR_MIXER = 0x65,
    PWR_VOL = 0x66,
    PRIV_INDEX = 0x6a,
    PRIV_DATA = 0x6c,
    I2S1_SDP = 0x70,
    ADDA_CLK1 = 0x73,
    GLB_CLK = 0x80,
    CLS_D_OVCD = 0x8c,
    CLS_D_OUT = 0x8d,
    DEPOP_M1 = 0x8e,
    DEPOP_M2 = 0x8f,
    DEPOP_M3 = 0x90,
    CHARGE_PUMP = 0x91,
    MICBIAS = 0x93,
    JD_CTRL = 0xbb,
    IRQ_CTRL1 = 0xbd,
    IRQ_CTRL2 = 0xbe,
    INT_IRQ_ST = 0xbf,
    GPIO_CTRL1 = 0xc0,
    GPIO_CTRL3 = 0xc2,
    GEN_CTRL1 = 0xfa,
    GEN_CTRL2 = 0xfb,
    VENDOR_ID2 = 0xff,
};
constexpr uint16_t kPrivate = 0x100;
enum : uint16_t {
    // PWR_DIG1, PWR_DIG2
    P_I2S1 = 1u << 15,
    P_DAC_L1 = 1u << 12,
    P_DAC_R1 = 1u << 11,
    P_ADC_L = 1u << 2,
    P_ADC_R = 1u << 1,
    P_CLS_D = 1u,
    P_ADC_SF = 1u << 15,
    // PWR_ANLG1, PWR_ANLG2
    P_VREF1 = 1u << 15,
    P_FV1 = 1u << 14,
    P_MB = 1u << 13,
    P_BG = 1u << 11,
    P_HP_L = 1u << 7,
    P_HP_R = 1u << 6,
    P_HA = 1u << 5,
    P_VREF2 = 1u << 4,
    P_FV2 = 1u << 3,
    P_LDO2 = 1u << 2,
    P_BST1 = 1u << 15,
    P_MB1 = 1u << 11,
    // PWR_MIXER, PWR_VOL
    P_OM_L = 1u << 15,
    P_OM_R = 1u << 14,
    P_SM_L = 1u << 13,
    P_SM_R = 1u << 12,
    P_RM_L = 1u << 11,
    P_RM_R = 1u << 10,
    P_SV_L = 1u << 15,
    P_SV_R = 1u << 14,
    P_HV_L = 1u << 11,
    P_HV_R = 1u << 10,
    // DEPOP_M1, DEPOP_M2, CHARGE_PUMP
    D_SMT_TRIG = 1u << 15,
    D_HP_L_SMT = 1u << 9,
    D_HP_R_SMT = 1u << 8,
    D_HP_CD_PD = 1u << 7,
    D_RSTN = 1u << 6,
    D_RSTP = 1u << 5,
    D_HP_CO = 1u << 4,
    D_HP_CP = 1u << 3,
    D_HP_SG = 1u << 2,
    D_HP_CB = 1u,
    D_DEPOP_MAN = 1u << 13,
    CP_OSW_L = 1u << 11,
    CP_OSW_R = 1u << 10,
    CP_PM_HP_HV = 2u << 8,
    // SPK_VOL / HP_VOL: left mute 15, left channel off 14, left volume 13:8 (0 = +12 dB, 1.5 dB steps), right 7/6/5:0
    V_MUTE = (1u << 15) | (1u << 7),
};
// nvaudio_conf.xml: "x Speaker Playback Volume" 30 (register 39 - 30 = 9, -1.5 dB), "x HP Playback Volume" 38
// (1, +10.5 dB), both channel switches on. The DAC volume tops out at rt5640.c's own control limits:
// CUSTOM_DAC_SPK_MAX_VAL_FRIG 150 (-9.4 dB, the OLED speakers, eq-config-type 2) and CUSTOM_DAC_HP_MAX_VAL 170 (-1.9 dB).
constexpr uint16_t kSpkVol = (9u << 8) | 9u, kHpVol = (1u << 8) | 1u;
constexpr uint16_t kDacSpeaker = 0x9696, kDacHeadphone = 0xaaaa;
constexpr uint16_t kMicBoost = 1u << 12;  // IN1 boost +20 dB (no switchroot setting exists; headset microphones)

struct RegValue {
    uint16_t reg, value;
};
// rt5640.c init_list (applied at probe).
constexpr RegValue kInitList[] = {
    {0x11d, 0x0347}, {0x13d, 0x3600}, {0x112, 0x0aa8}, {0x114, 0x8aaa},
    {0x120, 0x6110}, {0x121, 0xe0e0}, {0x123, 0x1804},
};
// eq_speakers_list_frig: the OLED speakers' EQ and limiter (rt5640_spk_event POST_PMU, eq-config-type 2).
constexpr RegValue kSpeakerEq[] = {
    {0x1a0, 0xe3f0}, {0x1a1, 0x0000}, {0x1a2, 0xc5e9}, {0x1a3, 0x1a98}, {0x1a4, 0x1d2c}, {0x1a5, 0xc882},
    {0x1a6, 0x1c10}, {0x1a7, 0x01f4}, {0x1a8, 0xe904}, {0x1a9, 0x1c10}, {0x1aa, 0x01f4}, {0x1ab, 0xe904},
    {0x1ac, 0x1c10}, {0x1ad, 0x01f4}, {0x1ae, 0x1e88}, {0x1af, 0x03ea}, {0x1b0, 0x1fe2}, {0x1b1, 0x001e},
    {0x1b2, 0x1fe2}, {0x1b3, 0x0800}, {0x1b4, 0x066e}, {0x0b1, 0x00e1}, {0x0b0, 0x6061}, {0x0b5, 0x1f00},
    {0x0b6, 0x0480}, {0x0b4, 0x2b30},
};
// eq_microphone_list_nx (rt5640_hp_event POST_PMU: with headphones the EQ block filters the ADC instead).
constexpr RegValue kMicEq[] = {
    {0x0d3, 0x2a20}, {0x1b0, 0x1f68}, {0x1b1, 0x0094}, {0x1b2, 0x1f69},
    {0x1b3, 0x0800}, {0x1b4, 0x0800}, {0x0b1, 0x0040}, {0x0b0, 0xe040},
};
// eq_normal_list_nx (rt5640_spk_event PRE_PMD).
constexpr RegValue kEqOff[] = {{0x0b1, 0x0000}, {0x0b0, 0x6000}, {0x0b4, 0x2206}};

enum Route : uint8_t { kRouteNone, kRouteSpeaker, kRouteHeadphones };
}  // namespace

class SwitchAudio : public IOService {
    OSDeclareDefaultStructors(SwitchAudio) IOWorkLoop *loop = nullptr;
    IOCommandGate *gate = nullptr;
    IOTimerEventSource *timer = nullptr;  // null: the play clock; Tegra: the period interrupt's deadline
    IOBufferMemoryDescriptor *memory[3] = {};
    SwitchAudioControl *control = nullptr;

    uint64_t hz = 0;

    // Tegra backend: hardware on `loop`, the codec (I2C transfers sleep) on its own work loop.
    bool tegra = false, tegraCapture = false;
    volatile uint8_t *car = nullptr, *pmc = nullptr, *pinmux = nullptr, *ahub = nullptr, *adma = nullptr,
                     *agic = nullptr;
    uint32_t ringPA[3] = {};
    uint32_t jackPinFound = 0;  // PV6's pinmux as the boot chain left it (logged with the first jack reading)
    IOInterruptEventSource *cascade = nullptr;
    uint64_t periods[2] = {};   // periods done since the DMA started
    uint16_t xferSeen[2] = {};  // ADMA_CH_XFER_STATUS (completed-period counter) at the last interrupt
    bool graced = false;
    IOWorkLoop *codecLoop = nullptr;
    IOCommandGate *codecGate = nullptr;
    IOInterruptEventSource *jackSource = nullptr;
    IOTimerEventSource *jackTimer = nullptr, *idleTimer = nullptr;
    bool codecOn = false, jackKnown = false, jackIn = false, headset = false, speakerOnly = false;
    Route route = kRouteNone;
    // switch-audio-stats: counted on the audio work loop, printed (and the peak reset) on the codec work loop.
    IOTimerEventSource *statsTimer = nullptr;
    uint64_t statFrames = 0, statFramesShown = 0, statIoCyclesShown = 0, statIoFramesShown = 0;
    uint32_t statStarts = 0, statStartsShown = 0, statUnderruns = 0;
    bool statsArmed = false, statsShown = false;  // statsTimer: armed / a line written (statsFired)
    int statPeak = 0;
    int64_t statLagMin = INT64_MAX, statLagMax = INT64_MIN, statLeadMin = INT64_MAX;
    uint64_t periodTicksQ16 = 0;  // host ticks per ADMA period in 48.16 (kPeriodFrames * ticks_per_frame_q32 >> 16)

    uint64_t nowFrame() const
    {
        const uint64_t now = mach_absolute_time();
        if (now <= control->anchor_host) return 0;
        const uint64_t ticks = now - control->anchor_host;  // split so nothing overflows (no 128-bit helpers here)
        return ticks / hz * SWITCH_AUDIO_RATE + ticks % hz * SWITCH_AUDIO_RATE / hz;
    }
    switch_audio_sample_t *ring(uint32_t type) const
    {
        return static_cast<switch_audio_sample_t *>(memory[type]->getBytesNoCopy());
    }

    // ---- Null backend -------------------------------------------------------------------------------------------
    // Playback up to now is "played"; clear it so the next turn of the ring mixes onto silence.
    void advance()
    {
        const uint64_t now = nowFrame();
        if (control->state & SwitchAudioOutputRunning) {
            switch_audio_sample_t *out = ring(SwitchAudioMemoryOutput);
            uint64_t from = control->out_frames;
            if (now > from + SWITCH_AUDIO_RING_FRAMES) from = now - SWITCH_AUDIO_RING_FRAMES;
            while (from < now) {
                const uint64_t at = from % SWITCH_AUDIO_RING_FRAMES;
                const uint64_t run =
                    now - from < SWITCH_AUDIO_RING_FRAMES - at ? now - from : SWITCH_AUDIO_RING_FRAMES - at;
                bzero(out + at * SWITCH_AUDIO_OUT_CHANNELS, run * SWITCH_AUDIO_OUT_CHANNELS * sizeof(*out));
                from += run;
            }
            control->out_frames = now;
        }
        if (control->state & SwitchAudioInputRunning) control->in_frames = now;  // the capture ring stays silent
    }
    static void tick(OSObject *owner, IOTimerEventSource *source)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (!self->control->state) return;
        self->advance();
        source->setTimeoutMS(kTickMS);
    }

    // ---- Tegra: clocks, pads, AHUB, ADMA, AGIC (once, at start) -----------------------------------------------------
    static volatile uint8_t *map(vm_offset_t pa, vm_size_t size, const char *what)
    {
        auto *base = reinterpret_cast<volatile uint8_t *>(ml_io_map(pa, size));
        if (!base) panic("SwitchAudio: cannot map %s at %#lx", what, (unsigned long)pa);
        return base;
    }

    // The rate the I2S really runs at, as host ticks per frame in 32.32: fs = ref * n_eff / (8192 m p) * 2/15 / 1024
    // (PLLA_OUT0 / 16 for the bit clock, / 64 bit clocks a frame).
    uint64_t tegraTicksPerFrame(uint64_t refHz) const
    {
        return switch_audio_ticks_per_frame_q32(hz, refHz, kPllaM, kPllaP, (uint64_t)kPllaNEff, kPllaOut0X2,
                                                kPllaOut0PerFs);
    }

    // PLLA 368.64 MHz and PLLA_OUT0 49.152 MHz, in the order clk-pll.c programs a disabled SDM PLL (set_defaults,
    // _update_pll_mnp, _clk_pll_enable, wait for lock). Left as it is if it already runs this configuration.
    void startPlla()
    {
        const uint32_t base = r32(car, PLLA_BASE);
        const bool running = (base & (PLL_ENABLE | PLL_BYPASS | PLLA_IDDQ)) == PLL_ENABLE &&
                             (base & PLLA_MNP_MASK) == kPllaMnp && (r32(car, PLLA_MISC2) & PLLA_EN_SDM) &&
                             (r32(car, PLLA_MISC1) & 0xffff) == kPllaSdm;
        if (!running) {
            w32(car, PLLA_OUT, r32(car, PLLA_OUT) & ~3u);  // PLLA_OUT0 off while the PLL changes
            w32(car, PLLA_BASE, (base & ~PLL_ENABLE) | PLLA_IDDQ);
            w32(car, PLLA_MISC0, PLLA_LOCK_ENABLE);
            w32(car, PLLA_MISC2, 0);
            w32(car, PLLA_BASE, (r32(car, PLLA_BASE) & ~(PLL_BYPASS | PLLA_MNP_MASK)) | kPllaMnp);
            w32(car, PLLA_MISC1, (r32(car, PLLA_MISC1) & ~0xffffu) | kPllaSdm);
            w32(car, PLLA_MISC2, PLLA_EN_SDM);
            IODelay(1);
            w32(car, PLLA_BASE, r32(car, PLLA_BASE) & ~PLLA_IDDQ);
            IODelay(5);
            w32(car, PLLA_BASE, r32(car, PLLA_BASE) | PLL_ENABLE);
            if (!waitBits(car, PLLA_BASE, PLL_LOCK, PLL_LOCK, 600)) {
                panic("SwitchAudio: PLLA did not lock: BASE %#x MISC0 %#x MISC1 %#x MISC2 %#x", r32(car, PLLA_BASE),
                      r32(car, PLLA_MISC0), r32(car, PLLA_MISC1), r32(car, PLLA_MISC2));
            }
            IODelay(50);
        }
        // PLLA_OUT0: ratio at 15:8, out of reset (bit 0) and enabled (bit 1); the vreg field 31:28 stays 0.
        w32(car, PLLA_OUT, (r32(car, PLLA_OUT) & ~0xf000ff03u) | (kPllaOut0Div << 8));
        w32(car, PLLA_OUT, r32(car, PLLA_OUT) | 3u);
    }

    void tegraClocksAndPads()
    {
        // Every rate below assumes the 38.4 MHz oscillator as PLL reference (the T210B01 PLLA table has only that).
        const uint32_t osc = r32(car, OSC_CTRL);
        if ((osc >> 28) != 5 || ((osc >> 26) & 3))
            panic("SwitchAudio: unexpected OSC_CTRL %#x (want 38.4 MHz, ref /1)", osc);
        // The APE partition (AHUB, ADMA, AGIC) is powered at boot and nothing here gates it; un-gating it would need the
        // MC flush and SLCG sequence of powergate-t21x.c, so a gated partition is reported, not handled.
        if (!(r32(pmc, PWRGATE_STATUS) & kPartitionAud)) {
            panic("SwitchAudio: APE power partition is off (PMC PWRGATE_STATUS %#x)", r32(pmc, PWRGATE_STATUS));
        }
        if ((r32(pmc, IO_DPD_STATUS) & (1u << 17)) || (r32(pmc, IO_DPD2_STATUS) & (1u << 29))) {
            panic("SwitchAudio: audio pads in deep power-down (IO_DPD_STATUS %#x IO_DPD2_STATUS %#x)",
                  r32(pmc, IO_DPD_STATUS), r32(pmc, IO_DPD2_STATUS));
        }
        // DAP1 is on the audio-hv rail, AUD_MCLK on audio; both are vdd-1v8 (MAX77620 SD3), DTB iopad-defaults 1.8 V.
        // mainline tegra_io_pad_set_voltage: write-enable the bit in PWR_DET, then clear it in PWR_DET_VALUE.
        if (r32(pmc, PWR_DET_VAL) & kPad33V) {
            w32(pmc, PWR_DET, r32(pmc, PWR_DET) | kPad33V);
            w32(pmc, PWR_DET_VAL, r32(pmc, PWR_DET_VAL) & ~kPad33V);
            IODelay(250);
        }
        if (r32(pmc, NO_IOPOWER) & kNoIoPowerAudioHv) w32(pmc, NO_IOPOWER, r32(pmc, NO_IOPOWER) & ~kNoIoPowerAudioHv);

        startPlla();
        w32(car, CLK_SOURCE_APE, kApeSource);
        w32(car, CLK_SOURCE_D_AUDIO, 0);            // PLLA_OUT0 (mux_d_audio_clk index 0), 1:1
        w32(car, CLK_SOURCE_I2S0, kI2sDiv);         // PLLA_OUT0 (mux 0)
        w32(car, CLK_SOURCE_EXTERN1, kExtern1Div);  // PLLA_OUT0 (mux 0)
        w32(car, CLK_ENB_Y_SET, kClkY);
        w32(car, CLK_ENB_V_SET, kClkV);
        w32(car, CLK_ENB_L_SET, kClkL);
        IODelay(2);
        w32(car, RST_DEV_Y_CLR, kClkY);  // hekate hw_init leaves APE in reset
        w32(car, RST_DEV_V_CLR, kClkV);
        w32(car, RST_DEV_L_CLR, kClkL);
        (void)r32(car, RST_DEV_L_CLR);
        IODelay(2);
        // MCLK: clk_out_1 <- extern1, forced on (clk-tegra-pmc.c PMC_CLK(1, 6, 2)).
        w32(pmc, CLK_OUT_CNTRL, (r32(pmc, CLK_OUT_CNTRL) & ~kClkOut1Mask) | kClkOut1Extern1);
        jackPinFound = r32(pinmux, kPinJackDetect);
        for (const Pin &pin : kPins) {
            w32(pinmux, pin.offset, (r32(pinmux, pin.offset) & ~kPinFields) | pin.value);
        }
    }

    void tegraAhubAndDma()
    {
        w32(ahub, ADMAIF_GLOBAL_ENABLE, 1);
        for (uint32_t part = 0; part < 3; ++part) {
            w32(ahub, XBAR_I2S1 + part * XBAR_PART, part ? 0 : kXbarFromAdmaif1);
            w32(ahub, XBAR_ADMAIF1 + part * XBAR_PART, part ? 0 : kXbarFromI2s1);
        }
        // tegra_adma_init: clear interrupts, soft reset, enable.
        w32(adma, GLOBAL_INT_CLEAR, 1);
        w32(adma, GLOBAL_SOFT_RESET, 1);
        if (!waitBits(adma, GLOBAL_SOFT_RESET, 1, 0, 10000)) panic("SwitchAudio: ADMA soft reset did not finish");
        w32(adma, GLOBAL_CMD, 1);
    }

    // The two ADMA channel SPIs on the AGIC, as Linux irq-gic.c sets up a distributor and CPU interface (priority
    // 0xa0, level, the reading CPU interface as target, PMR 0xf0, bypass bits kept). A refused enable panics.
    void tegraAgic()
    {
        const uint32_t mask = r32(agic, GICD_ITARGETSR) & 0xff;  // banked: the interface our accesses come from
        if (!mask) panic("SwitchAudio: AGIC CPU interface mask reads 0 (GICD_CTLR %#x)", r32(agic, GICD_CTLR));
        const unsigned channels = tegraCapture ? 2 : 1;
        for (unsigned ch = 0; ch < channels; ++ch) {
            const uint32_t id = kAgicAdma + ch, bit = 1u << (id % 32), word = 4 * (id / 32), shift = 8 * (id % 4);
            w32(agic, GICD_ICENABLER + word, bit);
            w32(agic, GICD_ICPENDR + word, bit);
            const uint32_t byteReg = id & ~3u;
            w32(agic, GICD_IPRIORITYR + byteReg,
                (r32(agic, GICD_IPRIORITYR + byteReg) & ~(0xffu << shift)) | 0xa0u << shift);
            w32(agic, GICD_ITARGETSR + byteReg,
                (r32(agic, GICD_ITARGETSR + byteReg) & ~(0xffu << shift)) | mask << shift);
            const uint32_t cfg = GICD_ICFGR + 4 * (id / 16);
            w32(agic, cfg, r32(agic, cfg) & ~(2u << (2 * (id % 16))));  // level
        }
        w32(agic, GICD_CTLR, r32(agic, GICD_CTLR) | 1);
        w32(agic, GICC_PMR, 0xf0);
        w32(agic, GICC_CTLR, (r32(agic, GICC_CTLR) & 0x1e0) | 1);
        for (unsigned ch = 0; ch < channels; ++ch) {
            const uint32_t id = kAgicAdma + ch, bit = 1u << (id % 32), word = 4 * (id / 32);
            w32(agic, GICD_ISENABLER + word, bit);
            if (!(r32(agic, GICD_ISENABLER + word) & bit)) {
                panic("SwitchAudio: AGIC refused SPI %u (GICD_CTLR %#x GICC_CTLR %#x)", id - 32, r32(agic, GICD_CTLR),
                      r32(agic, GICC_CTLR));
            }
        }
    }

    void tegraInit()
    {
        car = map(kCarPA, 0x1000, "CAR");
        pmc = map(kPmcPagePA, 0x1000, "PMC") + kPmcOffset;
        pinmux = map(kPinmuxPA, 0x1000, "pinmux");
        ahub = map(kAhubPA, 0x2000, "AHUB");
        adma = map(kAdmaPA, 0x1000, "ADMA");
        agic = map(kAgicPA, 0x2000, "AGIC");
        for (uint32_t type = SwitchAudioMemoryOutput; type <= SwitchAudioMemoryInput; ++type) {
            IOByteCount length = 0;
            const addr64_t pa = memory[type]->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
            const uint32_t size = type == SwitchAudioMemoryOutput ? kOutBytes : kInBytes;
            if (!pa || length < size || pa + size > 0x100000000ULL)
                panic("SwitchAudio: ring %u at %#llx is not DMA-able", type, pa);
            ringPA[type] = (uint32_t)pa;
        }
        tegraClocksAndPads();
        tegraAhubAndDma();
        tegraAgic();
        IOService *nub = ioswitch_interrupt_nub(kCascadeIntid, false);  // sleeps until the GIC is up
        cascade = IOInterruptEventSource::interruptEventSource(this, &SwitchAudio::cascadeFired, nub, 0);
        if (!cascade || loop->addEventSource(cascade) != kIOReturnSuccess) {
            panic("SwitchAudio: cannot attach the APE interrupt (INTID %u)", kCascadeIntid);
        }
        cascade->enable();
    }

    // ---- Tegra: streams (audio work loop) --------------------------------------------------------------------------
    void softReset(uint32_t reg, const char *what)
    {
        w32(ahub, reg, 1);
        if (!waitBits(ahub, reg, 1, 0, 10000)) panic("SwitchAudio: %s soft reset did not finish", what);
    }
    // tegra210_i2s_alt.c tegra210_i2s_sw_reset: the reset clears the CIF and stream controls and I2S_CTRL, so they are
    // saved around it. It completes on the I2S's interface clock, so I2S_CTRL must already say master (the bit clock then
    // comes from the CAR i2s0 clock; as a slave the I2S would wait for one from the codec, which is a slave too).
    void i2sSoftReset(uint32_t reg, const char *what)
    {
        const uint32_t saved[] = {I2S_TX_CIF, I2S_RX_CIF, I2S_TX_CTRL, I2S_RX_CTRL, I2S_CTRL};
        uint32_t values[5];
        for (unsigned i = 0; i < 5; ++i) values[i] = r32(ahub, saved[i]);
        w32(ahub, reg, 1);
        if (!waitBits(ahub, reg, 1, 0, 10000)) {
            panic("SwitchAudio: %s soft reset did not finish (I2S CTRL %#x EN %#x TIMING %#x RX %#x TX %#x; CAR I2S0 "
                  "src %#x "
                  "enb %u rst %u PLLA %#x OUT %#x)",
                  what, r32(ahub, I2S_CTRL), r32(ahub, I2S_ENABLE), r32(ahub, I2S_TIMING), r32(ahub, I2S_RX_STATUS),
                  r32(ahub, I2S_TX_STATUS), r32(car, CLK_SOURCE_I2S0), !!(r32(car, CLK_OUT_ENB_L) & kClkL),
                  !!(r32(car, RST_DEVICES_L) & kClkL), r32(car, PLLA_BASE), r32(car, PLLA_OUT));
        }
        for (unsigned i = 0; i < 5; ++i) w32(ahub, saved[i], values[i]);
    }
    void stopUnit(uint32_t enable, uint32_t status, const char *what)
    {
        w32(ahub, enable, 0);
        if (!waitBits(ahub, status, 1, 0, 10000))
            panic("SwitchAudio: %s did not stop (status %#x)", what, r32(ahub, status));
    }

    void tegraStart()
    {
        switch_audio_sample_t *out = ring(SwitchAudioMemoryOutput), *in = ring(SwitchAudioMemoryInput);
        bzero(out, kOutBytes);
        cacheClean(out, kOutBytes);
        bzero(in, kInBytes);
        cacheFlush(in, kInBytes);
        control->out_frames = 0;
        control->in_frames = 0;
        control->out_written = 0;
        // I2S1 and ADMAIF1, each session in L4T's order (tegra210_i2s_alt.c): set_fmt (CTRL with master mode, I2S_EN),
        // hw_params (timing, data offset, CIF), then the DAPM PRE_PMU sw reset of each direction it powers up
        // (tegra210_i2s_rx_stop/tx_stop), which keeps that configuration. 10-30 15:55: with the reset written first, at
        // CTRL's reset default (slave), "I2S1 RX soft reset did not finish". Then the ADMAIF resets and
        // tegra_admaif_hw_params.
        w32(ahub, I2S_CTRL, kI2sCtrl);
        w32(ahub, I2S_ENABLE, 1);
        w32(ahub, I2S_TIMING, kI2sTiming);
        w32(ahub, I2S_RX_CTRL, kI2sDataOffset);
        w32(ahub, I2S_TX_CTRL, kI2sDataOffset);
        w32(ahub, I2S_RX_CIF, switch_audio_i2s_rx_cif());  // playback: XBAR stereo -> DAP stereo
        w32(ahub, I2S_TX_CIF, switch_audio_i2s_tx_cif());  // capture: DAP stereo -> XBAR mono (left: IN1 is on both)
        i2sSoftReset(I2S_RX_SOFT_RESET, "I2S1 RX");
        if (tegraCapture) i2sSoftReset(I2S_TX_SOFT_RESET, "I2S1 TX");
        softReset(ADMAIF_TX_SOFT_RESET, "ADMAIF1 TX");
        softReset(ADMAIF_RX_SOFT_RESET, "ADMAIF1 RX");
        w32(ahub, ADMAIF_TX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_OUT_CHANNELS));
        w32(ahub, ADMAIF_RX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_IN_CHANNELS));
        const unsigned channels = tegraCapture ? 2 : 1;
        for (unsigned ch = 0; ch < channels; ++ch) {
            const uint32_t base = ch * CH_STRIDE;
            w32(adma, base + CH_CMD, 0);
            w32(adma, base + CH_INT_CLEAR, 1);
            w32(adma, base + CH_TC, ch == kChOut ? kOutPeriodBytes : kInPeriodBytes);
            w32(adma, base + CH_CTRL, kChCtrl[ch]);
            w32(adma, base + CH_SRC, ch == kChOut ? ringPA[SwitchAudioMemoryOutput] : 0);
            w32(adma, base + CH_TRG, ch == kChOut ? 0 : ringPA[SwitchAudioMemoryInput]);
            w32(adma, base + CH_FIFO_CTRL, kChFifo[ch]);
            w32(adma, base + CH_CONFIG, kChConfig[ch]);
        }
        w32(ahub, I2S_RX_ENABLE, 1);
        if (tegraCapture) w32(ahub, I2S_TX_ENABLE, 1);
        w32(ahub, ADMAIF_TX_ENABLE, 1);
        if (tegraCapture) w32(ahub, ADMAIF_RX_ENABLE, 1);
        // Sample time 0 is ring frame 0 leaving memory. tegra_adma_start brackets the start with the global clock gate.
        control->anchor_host = mach_absolute_time();
        w32(adma, GLOBAL_CG, 0);
        for (unsigned ch = 0; ch < channels; ++ch) w32(adma, ch * CH_STRIDE + CH_CMD, 1);
        w32(adma, GLOBAL_CG, 7);
        for (unsigned ch = 0; ch < channels; ++ch) {
            periods[ch] = 0;
            xferSeen[ch] = (uint16_t)r32(adma, ch * CH_STRIDE + CH_XFER_STATUS);
        }
        graced = false;
        timer->setTimeoutMS(kWatchdogMS);
        ++statStarts;
    }

    // tegra_adma_stop, tegra_admaif_stop, the I2S widgets' power-down: DMA, then ADMAIF, then I2S.
    void tegraStop()
    {
        timer->cancelTimeout();
        const unsigned channels = tegraCapture ? 2 : 1;
        for (unsigned ch = 0; ch < channels; ++ch) {
            const uint32_t base = ch * CH_STRIDE;
            w32(adma, base + CH_CMD, 0);
            if (!waitBits(adma, base + CH_STATUS, 1, 0, 10000)) {
                panic("SwitchAudio: ADMA ch%u did not stop (STATUS %#x)", ch, r32(adma, base + CH_STATUS));
            }
            w32(adma, base + CH_INT_CLEAR, 1);
        }
        stopUnit(ADMAIF_TX_ENABLE, ADMAIF_TX_STATUS, "ADMAIF1 TX");
        softReset(ADMAIF_TX_SOFT_RESET, "ADMAIF1 TX");
        if (tegraCapture) {
            stopUnit(ADMAIF_RX_ENABLE, ADMAIF_RX_STATUS, "ADMAIF1 RX");
            softReset(ADMAIF_RX_SOFT_RESET, "ADMAIF1 RX");
        }
        stopUnit(I2S_RX_ENABLE, I2S_RX_STATUS, "I2S1 RX");
        if (tegraCapture) stopUnit(I2S_TX_ENABLE, I2S_TX_STATUS, "I2S1 TX");
        w32(ahub, I2S_ENABLE, 0);  // the bit and frame clocks stop until the next session (runtime suspend in Linux)
    }

    // Period n of the playback ring has left memory: clear it for the next turn and move out_frames past it. A DMA
    // that finishes periods sooner than the 48 kHz clock allows is not paced by the I2S: panic rather than play noise.
    void playedPeriod()
    {
        const uint64_t n = periods[kChOut]++;
        switch_audio_sample_t *slot =
            ring(SwitchAudioMemoryOutput) + (n % kPeriods) * kPeriodFrames * SWITCH_AUDIO_OUT_CHANNELS;
        const uint64_t done = (n + 1) * kPeriodFrames, now = nowFrame();
        if (statsTimer) notePlayed(slot, done);
        bzero(slot, kOutPeriodBytes);
        cacheClean(slot, kOutPeriodBytes);
        control->out_frames = done;
        if (!tegraCapture) control->in_frames = done;  // the capture ring stays silent, on the hardware's clock
        if (done > now + kPeriodFrames) {
            panic(
                "SwitchAudio: ADMA ch0 runs fast: %llu frames played at clock frame %llu (I2S %#x CTRL %#x TIMING %#x)",
                done, now, r32(ahub, I2S_RX_STATUS), r32(ahub, I2S_CTRL), r32(ahub, I2S_TIMING));
        }
    }
    // switch-audio-stats: the period that just played, before it is cleared: its loudest sample (in 16-bit units, as
    // the line has always shown it: 32767 is full scale), an underrun when the plug-in had not written up to its end,
    // how far past it the plug-in had written (the write-ahead margin: the safety offset in use, less the IO thread's
    // lateness), and how late this interrupt runs after the period's end on the I2S clock model the plug-in's zero
    // time stamps use (anchor_host + frames * ticks_per_frame_q32). The model and the host timer both run from the
    // 38.4 MHz oscillator, so the lag only holds the DMA's start offset and the interrupt latency: one that drifts
    // over a session means the time stamps do. The first kSettlePeriods of a start stay out of lead and lag (the audio
    // server's first IO cycle comes after them: "under" has always counted one per start). (The plug-in's writes are
    // already in this CPU-coherent mapping.)
    void notePlayed(const switch_audio_sample_t *slot, uint64_t done)
    {
        statFrames += kPeriodFrames;
        const bool settled = done > kSettlePeriods * kPeriodFrames;
        if (settled) {
            const int64_t lagTicks = (int64_t)(mach_absolute_time() - control->anchor_host) -
                                     (int64_t)((done / kPeriodFrames) * periodTicksQ16 >> 16);
            const int64_t lagUs = lagTicks * 1000000 / (int64_t)hz;
            if (lagUs < statLagMin) statLagMin = lagUs;
            if (lagUs > statLagMax) statLagMax = lagUs;
        }
        if (!(control->state & SwitchAudioOutputRunning)) return;
        const int64_t lead = (int64_t)control->out_written - (int64_t)done;
        if (settled && lead < statLeadMin) statLeadMin = lead;
        if (lead < 0) ++statUnderruns;
        int peak = statPeak;
        for (uint32_t i = 0; i < kPeriodFrames * SWITCH_AUDIO_OUT_CHANNELS; ++i) {
            const int sample = slot[i] >> 16;  // -32768..32767
            const int magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak) peak = magnitude;
        }
        statPeak = peak;
    }

    // Period n of the capture ring is in memory: drop what the CPUs cached of it, then publish it.
    void capturedPeriod()
    {
        const uint64_t n = periods[kChIn]++;
        cacheFlush(ring(SwitchAudioMemoryInput) + (n % kPeriods) * kPeriodFrames * SWITCH_AUDIO_IN_CHANNELS,
                   kInPeriodBytes);
        control->in_frames = (n + 1) * kPeriodFrames;
    }

    // One ADMA channel's interrupt: acknowledge it and account every period its completed-period counter
    // (ADMA_CH_XFER_STATUS, as tegra_adma_get_residue reads it) has moved past, so a late interrupt loses nothing.
    void channelInterrupt(unsigned ch)
    {
        const uint32_t base = ch * CH_STRIDE;
        const uint32_t status = r32(adma, base + CH_INT_STATUS);
        if (status) w32(adma, base + CH_INT_CLEAR, status);
        if (!control->state) return;  // the period that ended as the stream stopped
        const uint16_t xfer = (uint16_t)r32(adma, base + CH_XFER_STATUS);
        uint32_t done = (uint16_t)(xfer - xferSeen[ch]);
        xferSeen[ch] = xfer;
        if (done > kPeriods) {
            panic("SwitchAudio: ADMA ch%u completed %u periods in one interrupt (XFER_STATUS %#x): lost track", ch,
                  done, xfer);
        }
        for (; done; --done) ch == kChOut ? playedPeriod() : capturedPeriod();
        if (ch != kChOut) return;
        graced = false;
        timer->setTimeoutMS(kWatchdogMS);
        // Capture runs on the same clock from the same start: two periods behind playback, its interrupt is lost or its
        // DMA stalled.
        if (tegraCapture && periods[kChOut] > periods[kChIn] + 2) {
            const uint32_t in = kChIn * CH_STRIDE, id = kAgicAdma + kChIn, bit = 1u << (id % 32), word = 4 * (id / 32);
            panic("SwitchAudio: ADMA ch1 behind: %llu periods vs %llu, XFER %u seen %u INT_STATUS %#x STATUS %#x, AGIC "
                  "pend %u, "
                  "ADMAIF RX %#x I2S TX %#x",
                  periods[kChIn], periods[kChOut], r32(adma, in + CH_XFER_STATUS) & 0xffff, xferSeen[kChIn],
                  r32(adma, in + CH_INT_STATUS), r32(adma, in + CH_STATUS), !!(r32(agic, GICD_ISPENDR + word) & bit),
                  r32(ahub, ADMAIF_RX_STATUS), r32(ahub, I2S_TX_STATUS));
        }
    }

    // Work loop action for INTID 134 (level; the GIC masks it while this runs): drain the AGIC.
    static void cascadeFired(OSObject *owner, IOInterruptEventSource *, int)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        for (;;) {
            const uint32_t iar = r32(self->agic, GICC_IAR), id = iar & 0x3ff;
            if (id >= 1020) break;
            if (id == kAgicAdma + kChOut || (self->tegraCapture && id == kAgicAdma + kChIn)) {
                self->channelInterrupt(id - kAgicAdma);
            } else {
                w32(self->agic, GICD_ICENABLER + 4 * (id / 32), 1u << (id % 32));  // not ours: keep it off
                IOLog("SwitchAudio: AGIC interrupt %u is not ours, disabled\n", id);
            }
            w32(self->agic, GICC_EOIR, iar);
        }
    }

    // No playback period interrupt for 4 periods. If the ADMA moved on, its interrupt is not reaching us (after a grace
    // period for an action already queued); if it did not, the DMA is stalled (no I2S clock pulling data). Either way,
    // one line with the state.
    static void watchdogFired(OSObject *owner, IOTimerEventSource *sender)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (!self->control->state) return;
        volatile uint8_t *dma = self->adma, *gic = self->agic;
        const uint32_t xfer = r32(dma, CH_XFER_STATUS) & 0xffff;
        if (xfer != self->xferSeen[kChOut] && !self->graced) {
            self->graced = true;
            sender->setTimeoutMS(kGraceMS);
            return;
        }
        if (xfer != self->xferSeen[kChOut]) {
            char state[160];
            ioswitch_interrupt_describe(kCascadeIntid, state, sizeof(state));
            const uint32_t id = kAgicAdma + kChOut, bit = 1u << (id % 32), word = 4 * (id / 32);
            panic("SwitchAudio: ADMA ch0 period interrupt lost: XFER %u seen %u INT_STATUS %#x, AGIC en %u pend %u act "
                  "%u, %s",
                  xfer, self->xferSeen[kChOut], r32(dma, CH_INT_STATUS), !!(r32(gic, GICD_ISENABLER + word) & bit),
                  !!(r32(gic, GICD_ISPENDR + word) & bit), !!(r32(gic, GICD_ISACTIVER + word) & bit), state);
        }
        panic(
            "SwitchAudio: ADMA ch0 stalled at period %llu: STATUS %#x TC_STATUS %#x, ADMAIF TX %#x, I2S RX %#x EN %#x",
            self->periods[kChOut], r32(dma, CH_STATUS), r32(dma, CH_TC_STATUS), r32(self->ahub, ADMAIF_TX_STATUS),
            r32(self->ahub, I2S_RX_STATUS), r32(self->ahub, I2S_ENABLE));
    }

    // ---- RT5639 (codec work loop) ----------------------------------------------------------------------------------
    uint16_t codecRead(uint8_t reg)
    {
        uint8_t bytes[2] = {};
        int error = tegra_i2c_read(kCodecBus, kCodecAddr, reg, bytes, sizeof(bytes));
        if (error) error = tegra_i2c_read(kCodecBus, kCodecAddr, reg, bytes, sizeof(bytes));
        if (error) panic("SwitchAudio: RT5639 read %#x failed: %d", reg, error);
        return (uint16_t)(bytes[0] << 8 | bytes[1]);  // 16-bit registers, big endian
    }
    void codecWrite(uint16_t reg, uint16_t value)
    {
        if (reg >= kPrivate) {
            codecWrite(PRIV_INDEX, reg - kPrivate);
            reg = PRIV_DATA;
        }
        const uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
        int error = tegra_i2c_write(kCodecBus, kCodecAddr, (uint8_t)reg, bytes, sizeof(bytes));
        if (error) error = tegra_i2c_write(kCodecBus, kCodecAddr, (uint8_t)reg, bytes, sizeof(bytes));
        if (error) panic("SwitchAudio: RT5639 write %#x = %#x failed: %d", reg, value, error);
    }
    void codecUpdate(uint8_t reg, uint16_t mask, uint16_t value)
    {
        const uint16_t old = codecRead(reg), now = (uint16_t)((old & ~mask) | (value & mask));
        if (now != old) codecWrite(reg, now);
    }
    template <size_t N> void codecList(const RegValue (&list)[N])
    {
        for (const RegValue &entry : list) codecWrite(entry.reg, entry.value);
    }
    static bool jackPlugged(uint16_t irqStatus)
    {
        return !(irqStatus & (1u << 4));
    }  // JD_STATUS, inverted (jd_inverted)
    Route wantedRoute() const { return jackIn && !speakerOnly ? kRouteHeadphones : kRouteSpeaker; }

    // rt5640_i2c_probe, rt5640_probe, rt5640_enable_jack_detect, then the static link setup the machine driver and
    // hw_params give it (CBS_CFS, I2S, NB_NF; sysclk MCLK 12.288 MHz, 48 kHz, 24-bit, BCLK 64 fs) and the mixer
    // switches of nvaudio_conf.xml's rt5640-speaker and rt5640-headphone paths. Power follows the route later.
    void codecProbe()
    {
        if (tegra_i2c_init(kCodecBus) != TEGRA_I2C_OK) panic("SwitchAudio: I2C1 did not start");
        if (tegra_gpio_output(kCodecPower, 1)) panic("SwitchAudio: cannot drive PZ4 (RT5639 LDO1_EN)");
        IOSleep(400);
        uint8_t id[2] = {};
        const int error = tegra_i2c_read(kCodecBus, kCodecAddr, VENDOR_ID2, id, sizeof(id));
        if (error || id[0] != 0x62 || id[1] != 0x31) {
            panic("SwitchAudio: no RT5639 on I2C1 0x1c (VENDOR_ID2 %02x%02x, error %d)", id[0], id[1], error);
        }
        codecWrite(RESET, 0);
        codecUpdate(GEN_CTRL1, (1u << 11) | 1u, (1u << 11) | 1u);  // MCLK detect, I2S clock
        codecList(kInitList);
        codecWrite(kPrivate + 0x77, 0x9f00);         // HP DCC
        codecUpdate(CLS_D_OUT, 1u << 11, 1u << 11);  // class-D stereo
        codecUpdate(CLS_D_OVCD, 1u << 8, 1u << 8);   // power down on over-current
        codecUpdate(SPO_CLSD_RATIO, 7, 4);
        codecWrite(kPrivate + 0x90, 0x2000);  // HP amp detection bounds
        codecWrite(kPrivate + 0x91, 0x1000);
        codecBiasOff();
        codecUpdate(GEN_CTRL1, 0x0301, 0x0301);
        // Jack detect: source the codec's GPIO2, GPIO1 as the IRQ output, JD1 only, over-current 600 uA x 0.75 scale.
        codecUpdate(JD_CTRL, 7u << 13, 4u << 13);
        codecUpdate(GPIO_CTRL1, (1u << 15) | (1u << 10), (1u << 15) | (1u << 10));
        codecUpdate(GPIO_CTRL3, 1u << 2, 1u << 2);
        codecWrite(GEN_CTRL1, 0x3e01);
        codecWrite(GEN_CTRL2, 0x0000);
        codecWrite(kPrivate + 0x15, 0xa800 | (2u << 8));
        codecUpdate(MICBIAS, (3u << 9) | (1u << 11), 1u << 11);
        codecUpdate(IRQ_CTRL2, 1u << 11, 0);  // external microphone: no sticky over-current
        codecWrite(IRQ_CTRL1, 1u << 15);      // IRQ_JD_NOR
        // Link and clocks.
        // Slave, normal BCLK, 24-bit, I2S; sysclk = MCLK; BCLK 64 fs, pre-divider 1 (256 fs) (SwitchAudioFormat.h).
        codecUpdate(I2S1_SDP, SWITCH_AUDIO_RT5640_SDP_MASK, SWITCH_AUDIO_RT5640_SDP);
        codecUpdate(GLB_CLK, 3u << 14, 0);
        codecUpdate(ADDA_CLK1, SWITCH_AUDIO_RT5640_ADDA_MASK, SWITCH_AUDIO_RT5640_ADDA);
        // Mixer switches (a cleared bit routes): stereo DAC mixers <- DAC1, speaker mixers <- DAC1, speaker outputs
        // <- SPKVOL, output mixers <- DAC1, HPO mixer <- HPVOL. Outputs muted and at nvaudio_conf.xml's volumes.
        codecUpdate(STO_DAC_MIXER, (1u << 14) | (1u << 6), 0);
        codecUpdate(SPK_L_MIXER, 1u << 3, 0);
        codecUpdate(SPK_R_MIXER, 1u << 3, 0);
        codecUpdate(SPO_L_MIXER, 1u << 12, 0);
        codecUpdate(SPO_R_MIXER, 1u << 12, 0);
        codecUpdate(OUT_L3_MIXER, 1u, 0);
        codecUpdate(OUT_R3_MIXER, 1u, 0);
        codecUpdate(HPO_MIXER, 1u << 13, 0);
        codecWrite(SPK_VOL, V_MUTE | kSpkVol);
        codecWrite(HP_VOL, V_MUTE | kHpVol);
        if (tegraCapture) {
            // IN1P -> BST1 -> REC mixers -> ADC -> stereo ADC mixer (ADC1) -> IF1.
            codecUpdate(REC_L2_MIXER, 1u << 1, 0);
            codecUpdate(REC_R2_MIXER, 1u << 1, 0);
            codecUpdate(STO_ADC_MIXER, (1u << 14) | (1u << 6), 0);
            codecUpdate(IN1_IN2, 0xfu << 12, kMicBoost);
        }
    }

    // rt5640_set_bias_level STANDBY (from OFF) and OFF.
    void codecBiasStandby()
    {
        codecUpdate(PWR_ANLG1, P_VREF1 | P_MB | P_BG | P_VREF2, P_VREF1 | P_MB | P_BG | P_VREF2);
        IOSleep(15);
        codecUpdate(PWR_ANLG1, P_FV1 | P_FV2, P_FV1 | P_FV2);
        codecUpdate(GEN_CTRL1, 0x0301, 0x0201);
    }
    void codecBiasOff()
    {
        codecUpdate(DEPOP_M1, D_HP_CB | D_HP_CP | D_HP_CO | D_HP_CD_PD, D_HP_CD_PD);
        codecWrite(DEPOP_M2, 0x1100);
        for (uint8_t reg = PWR_DIG1; reg <= PWR_VOL; ++reg) codecWrite(reg, 0);
        codecUpdate(GEN_CTRL1, 1u, 1u);  // the I2S clock stays: jack detect with an external microphone needs it
    }

    // rt5640_pud_depop.
    void codecDepop(bool up)
    {
        if (up) {
            codecWrite(CHARGE_PUMP, CP_PM_HP_HV | CP_OSW_R | CP_OSW_L);
            codecUpdate(DEPOP_M3, 0x777, (7u << 8) | (3u << 4) | 7u);  // 192 kHz, 12 kHz, 192 kHz
            codecWrite(kPrivate + 0x37, 0xfc00);
            codecUpdate(DEPOP_M1, D_SMT_TRIG, D_SMT_TRIG);
            codecUpdate(DEPOP_M1, D_RSTN, D_RSTN);
            codecUpdate(DEPOP_M1, D_RSTN | D_HP_R_SMT | D_HP_L_SMT, D_HP_R_SMT | D_HP_L_SMT);
        } else {
            codecUpdate(DEPOP_M3, 0x777, (6u << 8) | (4u << 4) | 6u);  // 96 kHz, 24 kHz, 96 kHz
            codecWrite(kPrivate + 0x37, 0xfc00);
            codecUpdate(DEPOP_M1, D_HP_SG, D_HP_SG);
            codecUpdate(DEPOP_M1, D_RSTP, D_RSTP);
            codecUpdate(DEPOP_M1, D_RSTP | D_HP_R_SMT | D_HP_L_SMT, D_HP_R_SMT | D_HP_L_SMT);
        }
    }

    // The DAPM power-up and power-down of each route, in DAPM order (supplies, mixers, PGAs, then the HP post and
    // bias-on steps); the output mute is separate (codecMute) so a route can be up while no stream plays.
    void codecRouteUp(Route to)
    {
        if (to == kRouteSpeaker) {
            codecUpdate(PWR_DIG1, P_CLS_D, P_CLS_D);  // "Improve SPK Amp Drv", then rt5640_spk_event POST_PMU
            codecList(kSpeakerEq);
            codecUpdate(PWR_MIXER, P_SM_L | P_SM_R, P_SM_L | P_SM_R);
            codecUpdate(PWR_VOL, P_SV_L | P_SV_R, P_SV_L | P_SV_R);
            codecWrite(DAC1_DIG_VOL, kDacSpeaker);
        } else if (to == kRouteHeadphones) {
            codecUpdate(PWR_ANLG1, P_HP_L | P_HP_R, P_HP_L | P_HP_R);
            // "Improve HP Amp Drv": hp_amp_power_on.
            codecUpdate(DEPOP_M2, D_DEPOP_MAN, D_DEPOP_MAN);
            codecWrite(DEPOP_M1, D_HP_CP | D_HP_CB);
            codecUpdate(PWR_ANLG1, P_FV1 | P_FV2, 0);
            codecUpdate(PWR_ANLG1, P_HA, P_HA);
            IOSleep(15);
            codecUpdate(PWR_ANLG1, P_FV1 | P_FV2, P_FV1 | P_FV2);
            codecUpdate(DEPOP_M1, D_HP_SG | D_HP_CO, D_HP_SG | D_HP_CO);
            codecUpdate(PWR_MIXER, P_OM_L | P_OM_R, P_OM_L | P_OM_R);
            codecUpdate(PWR_VOL, P_HV_L | P_HV_R, P_HV_L | P_HV_R);
            codecDepop(true);  // "HP Amp" POST_PMU
            codecList(kMicEq);
            codecWrite(DAC1_DIG_VOL, kDacHeadphone);
            IOSleep(80);   // "HP Post"
            IOSleep(100);  // bias ON with the HP amp up: then release its soft mute
            codecUpdate(DEPOP_M1, D_HP_SG | D_HP_R_SMT | D_HP_L_SMT, 0);
        }
        route = to;
    }
    void codecRouteDown()
    {
        codecMute(true);
        if (route == kRouteSpeaker) {
            codecUpdate(PWR_VOL, P_SV_L | P_SV_R, 0);
            codecUpdate(PWR_MIXER, P_SM_L | P_SM_R, 0);
            codecList(kEqOff);  // rt5640_spk_event PRE_PMD
            codecUpdate(PWR_DIG1, P_CLS_D, 0);
        } else if (route == kRouteHeadphones) {
            codecDepop(false);  // "HP Amp" PRE_PMD
            IOSleep(30);
            codecUpdate(DEPOP_M1, D_SMT_TRIG | D_HP_R_SMT | D_HP_L_SMT, 0);
            codecWrite(CHARGE_PUMP, CP_OSW_R | CP_OSW_L);
            codecUpdate(PWR_VOL, P_HV_L | P_HV_R, 0);
            codecUpdate(PWR_MIXER, P_OM_L | P_OM_R, 0);
            codecUpdate(PWR_ANLG1, P_HP_L | P_HP_R, 0);
        }
        route = kRouteNone;
    }
    void codecMute(bool mute)
    {
        if (route == kRouteSpeaker)
            codecUpdate(SPK_VOL, V_MUTE, mute ? V_MUTE : 0);
        else if (route == kRouteHeadphones)
            codecUpdate(HP_VOL, V_MUTE, mute ? V_MUTE : 0);
    }

    // rt5640_detect_headset, once the jack has settled: MICBIAS1 on, and an over-current means a plain TRS plug (the
    // microphone contact grounded). Keeps the bias only for a headset.
    void codecDetectHeadset()
    {
        headset = false;
        if (jackIn) {
            codecUpdate(PWR_ANLG1, P_LDO2, P_LDO2);
            codecUpdate(PWR_ANLG2, P_MB1, P_MB1);
            codecUpdate(IRQ_CTRL2, 1u << 3, 0);
            IOSleep(100);
            headset = jackIn && !(codecRead(IRQ_CTRL2) & (1u << 3));
        }
        if (!headset) {
            codecUpdate(PWR_ANLG2, P_MB1, 0);
            codecUpdate(PWR_ANLG1, P_LDO2, 0);
        }
    }

    // A stream starts: the codec powered (bias, DAC, capture path), routed to where the jack says, unmuted.
    void codecRun()
    {
        idleTimer->cancelTimeout();
        statsResume();
        tegra_gpio_check_delivery(kCodecIrq, "SwitchAudio");  // before trusting the jack state
        if (!codecOn) {
            codecBiasStandby();
            codecUpdate(PWR_DIG1, P_I2S1 | P_DAC_L1 | P_DAC_R1, P_I2S1 | P_DAC_L1 | P_DAC_R1);
            if (tegraCapture) {
                codecDetectHeadset();
                codecUpdate(PWR_ANLG2, P_BST1, P_BST1);
                codecUpdate(PWR_MIXER, P_RM_L | P_RM_R, P_RM_L | P_RM_R);
                codecUpdate(PWR_DIG1, P_ADC_L | P_ADC_R, P_ADC_L | P_ADC_R);
                codecUpdate(PWR_DIG2, P_ADC_SF, P_ADC_SF);
            }
            codecOn = true;
        }
        const Route want = wantedRoute();
        if (route != want) {
            codecRouteDown();
            codecRouteUp(want);
        }
        codecMute(false);
    }
    // The last stream stopped: mute now, power down after kIdleMS unless one starts again.
    void codecPause()
    {
        codecMute(true);
        idleTimer->setTimeoutMS(kIdleMS);
    }

    static IOReturn codecGated(OSObject *owner, void *run, void *, void *, void *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        run ? self->codecRun() : self->codecPause();
        return kIOReturnSuccess;
    }
    static void codecIdle(OSObject *owner, IOTimerEventSource *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (self->control->state || !self->codecOn) return;
        tegra_gpio_check_delivery(kCodecIrq, "SwitchAudio");
        self->codecRouteDown();
        self->codecBiasOff();
        self->headset = false;
        self->codecOn = false;
    }
    // PBB4, both edges (rt5640_irq: IRQF_TRIGGER_RISING | FALLING): wait for the contacts to settle.
    static void jackEdge(OSObject *owner, IOInterruptEventSource *, int)
    {
        static_cast<SwitchAudio *>(owner)->jackTimer->setTimeoutMS(kJackSettleMS);
    }
    // Also the first reading, kJackSettleMS after codecStart; until then the jack counts as empty (the speaker).
    static void jackSettled(OSObject *owner, IOTimerEventSource *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        const uint16_t status = self->codecRead(INT_IRQ_ST);
        const bool in = jackPlugged(status);
        if (!self->jackKnown) {
            self->jackKnown = true;
            IOLog("SwitchAudio: headphones %s at start\n", in ? "in" : "out");
        } else if (in != self->jackIn) {
            IOLog("SwitchAudio: headphones %s\n", in ? "plugged in" : "unplugged");
        } else {
            return;
        }
        self->jackIn = in;
        if (!self->codecOn) return;  // the next stream routes
        if (self->tegraCapture) self->codecDetectHeadset();
        if (self->route == self->wantedRoute()) return;
        self->codecRouteDown();
        self->codecRouteUp(self->wantedRoute());
        if (self->control->state) self->codecMute(false);
    }

    // switch-audio-stats=1, every kStatsMS on the codec work loop (it reads the codec over I2C). "starts" counts the
    // streams the hardware started since the last line, "moved" what the playback DMA played (240000 a line while a
    // stream runs), "under" the periods that played before the plug-in had written their end, "peak" the loudest sample
    // since the last line (16-bit units; 0: only silence reached the DMA); "io" the plug-in's playback IO cycles in the
    // window and the frames each wrote (the audio server's wake-ups and IO buffer size), "lead" the least write-ahead
    // the plug-in had at a period's end (frames; under 0 is an underrun), "lag" the period interrupts' least and most
    // lateness against the I2S clock model in us (see notePlayed; "-" without a period); then, as read now, ADMA ch0
    // STATUS and completed periods, I2S1 enable and RX (playback) status, ADMAIF1 TX status, the jack (INT_IRQ_ST,
    // bit 4 set = empty; PV6, 1 = empty) and route, and PWR_DIG1, PWR_ANLG1, PWR_MIXER, PWR_VOL, SPK_VOL, HP_VOL,
    // DAC1_DIG_VOL.
    // r16-idle: the line runs while there is audio to report: the first window after boot, then from a stream's start
    // (codecRun arms it) until a window with no stream, no start, nothing moved and the codec powered down; that window
    // reads nothing, writes no line and leaves the timer off, so a silent console takes no stats wakeups.
    static void statsFired(OSObject *owner, IOTimerEventSource *sender)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        const uint64_t frames = self->statFrames, moved = frames - self->statFramesShown;
        const uint32_t starts = self->statStarts - self->statStartsShown;
        if (self->statsShown && !self->control->state && !self->codecOn && !starts && !moved) {
            self->statsArmed = false;
            return;
        }
        self->statsShown = true;
        self->statFramesShown = frames;
        self->statStartsShown += starts;
        const int peak = self->statPeak;
        self->statPeak = 0;
        const uint64_t ioCycles = self->control->io_cycles, ioFrames = self->control->io_frames;
        const uint64_t cycles = ioCycles - self->statIoCyclesShown, written = ioFrames - self->statIoFramesShown;
        self->statIoCyclesShown = ioCycles;
        self->statIoFramesShown = ioFrames;
        char lead[24] = "-", lag[48] = "-";
        if (self->statLeadMin != INT64_MAX) snprintf(lead, sizeof(lead), "%lld", (long long)self->statLeadMin);
        if (self->statLagMax != INT64_MIN) {
            snprintf(lag, sizeof(lag), "%lld..%lld", (long long)self->statLagMin, (long long)self->statLagMax);
        }
        self->statLeadMin = INT64_MAX;
        self->statLagMin = INT64_MAX;
        self->statLagMax = INT64_MIN;
        static const char *const routes[] = {"none", "speaker", "headphones"};
        char line[416];
        snprintf(
            line, sizeof(line),
            "SwitchAudio stats: run %u starts %u moved %llu under %u peak %d io %llu x %llu f lead %s lag %s us | ADMA "
            "%#x xfer %u, I2S en %u rx %#x, ADMAIF tx %#x | jack %s st %#x pv6 %d, %s | codec %04x %04x %04x %04x vol "
            "%04x %04x dac %04x",
            self->control->state, starts, moved, self->statUnderruns, peak, cycles, cycles ? written / cycles : 0, lead,
            lag, r32(self->adma, CH_STATUS), r32(self->adma, CH_XFER_STATUS) & 0xffff, r32(self->ahub, I2S_ENABLE),
            r32(self->ahub, I2S_RX_STATUS), r32(self->ahub, ADMAIF_TX_STATUS), self->jackIn ? "in" : "out",
            self->codecRead(INT_IRQ_ST), tegra_gpio_read(kJackDetect), routes[self->route], self->codecRead(PWR_DIG1),
            self->codecRead(PWR_ANLG1), self->codecRead(PWR_MIXER), self->codecRead(PWR_VOL), self->codecRead(SPK_VOL),
            self->codecRead(HP_VOL), self->codecRead(DAC1_DIG_VOL));
        os_log(OS_LOG_DEFAULT, "%s", line);  // log only, never the boot console
        sender->setTimeoutMS(kStatsMS);
    }
    // Codec work loop: a stream starts; the stats line resumes if it had stopped (statsFired).
    void statsResume()
    {
        if (!statsTimer || statsArmed) return;
        statsArmed = true;
        statsTimer->setTimeoutMS(kStatsMS);
    }

    void codecStart()
    {
        codecLoop = IOWorkLoop::workLoop();
        codecGate = codecLoop ? IOCommandGate::commandGate(this) : nullptr;
        jackTimer = codecGate ? IOTimerEventSource::timerEventSource(this, &SwitchAudio::jackSettled) : nullptr;
        idleTimer = jackTimer ? IOTimerEventSource::timerEventSource(this, &SwitchAudio::codecIdle) : nullptr;
        if (!idleTimer || codecLoop->addEventSource(codecGate) != kIOReturnSuccess ||
            codecLoop->addEventSource(jackTimer) != kIOReturnSuccess ||
            codecLoop->addEventSource(idleTimer) != kIOReturnSuccess) {
            panic("SwitchAudio: codec work loop setup failed");
        }
        codecProbe();
        // PV6 as a GPIO input (CNF set, output off), so its IN bit is the pad's level; no event source, so its interrupt
        // stays masked: the codec's IRQ reports the jack.
        if (!tegra_gpio_interrupt_nub(kJackDetect, TEGRA_GPIO_EDGE_BOTH))
            panic("SwitchAudio: cannot make PV6 a GPIO input");
        // The jack is read first once its line has settled, with the IRQ already armed so no change is missed meanwhile.
        // 10-31 14:17: read right after the setup above (and before the IRQ was armed), JD_STATUS said plugged with
        // nothing in the jack and was never read again, so every stream went to the headphone amplifier and the speakers
        // stayed silent. (Linux reads it from a work item and confirms a plug for a second: rt5640_detect_headset.)
        IOService *nub = tegra_gpio_interrupt_nub(kCodecIrq, TEGRA_GPIO_EDGE_BOTH);
        jackSource = nub ? IOInterruptEventSource::interruptEventSource(this, &SwitchAudio::jackEdge, nub, 0) : nullptr;
        if (!jackSource || codecLoop->addEventSource(jackSource) != kIOReturnSuccess) {
            panic("SwitchAudio: cannot attach the RT5639 IRQ (GPIO %u, PBB4)", kCodecIrq);
        }
        jackSource->enable();
        jackTimer->setTimeoutMS(kJackSettleMS);
        uint32_t stats = 0;
        if (PE_parse_boot_argn("switch-audio-stats", &stats, sizeof(stats)) && stats) {
            statsTimer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::statsFired);
            if (!statsTimer || codecLoop->addEventSource(statsTimer) != kIOReturnSuccess)
                panic("SwitchAudio: stats timer setup failed");
            statsResume();
        }
    }

    // ---- Requests ------------------------------------------------------------------------------------------------
    static IOReturn requestGated(OSObject *owner, void *selector, void *, void *, void *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        SwitchAudioControl *c = self->control;
        const uint32_t was = c->state;
        switch ((uint32_t)(uintptr_t)selector) {
        case SwitchAudioStartOutput:
            if (!(was & SwitchAudioOutputRunning)) {
                if (!self->tegra) {
                    bzero(self->ring(SwitchAudioMemoryOutput), kOutBytes);
                    c->out_frames = self->nowFrame();
                }
                c->state = was | SwitchAudioOutputRunning;
            }
            break;
        case SwitchAudioStopOutput: c->state = was & ~(uint32_t)SwitchAudioOutputRunning; break;
        case SwitchAudioStartInput:
            if (!(was & SwitchAudioInputRunning)) {
                if (!self->tegra) c->in_frames = self->nowFrame();
                c->state = was | SwitchAudioInputRunning;
            }
            break;
        case SwitchAudioStopInput: c->state = was & ~(uint32_t)SwitchAudioInputRunning; break;
        default: return kIOReturnBadArgument;
        }
        // Both directions share one clock and one start: the hardware runs while either is requested.
        if (!was && c->state) {
            if (self->tegra)
                self->tegraStart();
            else
                self->timer->setTimeoutMS(kTickMS);
        } else if (was && !c->state) {
            if (self->tegra)
                self->tegraStop();
            else
                self->timer->cancelTimeout();
        }
        return kIOReturnSuccess;
    }

public:
    bool start(IOService *provider) override
    {
        if (!IOService::start(provider)) return false;
        uint32_t mode = 0;
        PE_parse_boot_argn("switch-audio", &mode, sizeof(mode));
        tegra = (mode & 1) != 0;
        tegraCapture = tegra && (mode & 2);
        speakerOnly = tegra && (mode & 4);
        const IOOptionBits options = (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryKernelUserShared |
                                     (IOOptionBits)kIOMemoryPhysicallyContiguous;
        const uint32_t sizes[3] = {(uint32_t)PAGE_SIZE, kOutBytes, kInBytes};
        for (int i = 0; i < 3; ++i) {
            memory[i] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, sizes[i], kDmaMask);
            if (!memory[i]) return false;
            bzero(memory[i]->getBytesNoCopy(), sizes[i]);
        }
        loop = IOWorkLoop::workLoop();
        gate = loop ? IOCommandGate::commandGate(this) : nullptr;
        if (!gate || loop->addEventSource(gate) != kIOReturnSuccess) return false;

        control = static_cast<SwitchAudioControl *>(memory[SwitchAudioMemoryControl]->getBytesNoCopy());
        control->magic = SWITCH_AUDIO_MAGIC;
        control->version = SWITCH_AUDIO_VERSION;
        control->rate = SWITCH_AUDIO_RATE;
        control->out_channels = SWITCH_AUDIO_OUT_CHANNELS;
        control->in_channels = SWITCH_AUDIO_IN_CHANNELS;
        control->ring_frames = SWITCH_AUDIO_RING_FRAMES;
        // The plug-in writes at least this far ahead of now: past one null tick, or past the ADMA's prefetch.
        control->lead_frames = 2 * SWITCH_AUDIO_PERIOD_FRAMES;
        hz = timerHz();
        if (tegra) {
            // The interrupt source goes on the loop before the deadline timer, so a pending interrupt is served first.
            tegraInit();
            codecStart();
            control->period_frames = kPeriodFrames;
            control->backend = SwitchAudioBackendTegraI2S;
            control->ticks_per_frame_q32 = tegraTicksPerFrame(38400000);
            periodTicksQ16 = (uint64_t)kPeriodFrames * control->ticks_per_frame_q32 >> 16;
            timer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::watchdogFired);
        } else {
            control->period_frames = SWITCH_AUDIO_PERIOD_FRAMES;
            control->backend = SwitchAudioBackendNull;
            control->ticks_per_frame_q32 = (hz << 32) / SWITCH_AUDIO_RATE;
            timer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::tick);
        }
        if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) return false;
        control->anchor_host = mach_absolute_time();

        // audiomxd's sandbox names no class of ours; the kernel's NativeSandboxGrantProcess check (IOUserClient.cpp)
        // lets exactly this process open the service. Our check compares the whole string to one process name.
        setProperty("NativeSandboxGrantProcess", "audiomxd");
        setProperty("IOUserClientClass", "SwitchAudioUserClient");
        setProperty("SwitchAudioBackend", tegra ? "tegra-i2s" : "null");
        registerService();
        if (tegra) {
            // The format and rate, once: S32_LE rings and APE path, 32-bit I2S slots at 64 fs, the codec's 24 bits.
            const uint64_t milliHz =
                switch_audio_rate_millihz(38400000, kPllaM, kPllaP, (uint64_t)kPllaNEff, kPllaOut0X2, kPllaOut0PerFs);
            IOLog("SwitchAudio: Tegra I2S1 + RT5639 ready (%s%s): S32_LE, I2S 32-bit slots at 64 fs (%u kHz BCLK), "
                  "RT5639 %u-bit, %llu.%03llu Hz\n",
                  tegraCapture ? "playback and capture" : "playback", speakerOnly ? ", speaker only" : "",
                  (unsigned)((milliHz * SWITCH_AUDIO_BCLK_FS + 500000) / 1000000), SWITCH_AUDIO_CODEC_BITS,
                  milliHz / 1000, milliHz % 1000);
        }
        return true;
    }
    void free() override
    {
        if (timer) {
            timer->cancelTimeout();
            if (loop) loop->removeEventSource(timer);
        }
        if (cascade && loop) {
            cascade->disable();
            loop->removeEventSource(cascade);
        }
        if (gate && loop) loop->removeEventSource(gate);
        if (codecLoop) {
            if (jackSource) {
                jackSource->disable();
                codecLoop->removeEventSource(jackSource);
            }
            if (statsTimer) statsTimer->cancelTimeout();
            IOEventSource *sources[] = {jackTimer, idleTimer, statsTimer, codecGate};
            for (IOEventSource *source : sources) {
                if (source) codecLoop->removeEventSource(source);
            }
        }
        OSSafeReleaseNULL(timer);
        OSSafeReleaseNULL(statsTimer);
        OSSafeReleaseNULL(cascade);
        OSSafeReleaseNULL(gate);
        OSSafeReleaseNULL(loop);
        OSSafeReleaseNULL(jackSource);
        OSSafeReleaseNULL(jackTimer);
        OSSafeReleaseNULL(idleTimer);
        OSSafeReleaseNULL(codecGate);
        OSSafeReleaseNULL(codecLoop);
        for (auto &m : memory) OSSafeReleaseNULL(m);
        IOService::free();
    }
    // The codec moves first both ways, as ASoC orders DAPM around a stream: powered and unmuted before the I2S clocks
    // data, muted before the clocks stop.
    IOReturn request(uint32_t selector)
    {
        if (selector > SwitchAudioStopInput) return kIOReturnBadArgument;
        if (tegra) {
            const bool run = selector == SwitchAudioStartOutput || selector == SwitchAudioStartInput;
            const uint32_t bit = (selector == SwitchAudioStartOutput || selector == SwitchAudioStopOutput)
                                     ? SwitchAudioOutputRunning
                                     : SwitchAudioInputRunning;
            if (run || !(control->state & ~bit)) codecGate->runAction(&SwitchAudio::codecGated, (void *)(uintptr_t)run);
        }
        return gate->runAction(&SwitchAudio::requestGated, (void *)(uintptr_t)selector);
    }
    IOMemoryDescriptor *copyMemory(uint32_t type)
    {
        if (type > SwitchAudioMemoryInput) return nullptr;
        memory[type]->retain();
        return memory[type];
    }
};
OSDefineMetaClassAndStructors(SwitchAudio, IOService)

    class SwitchAudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(SwitchAudioUserClient) task_t owner = nullptr;
    SwitchAudio *audio = nullptr;

public:
    bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override
    {
        if (!IOUserClient::initWithTask(task, security, type, properties)) return false;
        owner = task;
        return true;
    }
    bool start(IOService *provider) override
    {
        audio = OSDynamicCast(SwitchAudio, provider);
        return audio && IOUserClient::start(provider);
    }
    IOReturn clientClose() override
    {
        // The audio server went away: nothing feeds the rings any more.
        audio->request(SwitchAudioStopOutput);
        audio->request(SwitchAudioStopInput);
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override
    {
        IOMemoryDescriptor *m = audio->copyMemory(type);
        if (!m) return kIOReturnBadArgument;
        *options = 0;
        *memory = m;  // the caller consumes the reference
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override
    {
        if (args->structureInputSize || args->structureOutputSize || args->scalarOutputCount)
            return kIOReturnBadArgument;
        if (selector == SwitchAudioSetHostClock)
            return args->scalarInputCount == 1 ? setHostClock(owner, args->scalarInput[0]) : kIOReturnBadArgument;
        if (selector > SwitchAudioStopInput || args->scalarInputCount) return kIOReturnBadArgument;
        return audio->request(selector);
    }
};
OSDefineMetaClassAndStructors(SwitchAudioUserClient, IOUserClient)

    // Publishes the service under IOResources. Call once from the board thread (the Tegra backend sleeps on the GIC, the
    // GPIO controller and I2C1), any point before launchd starts audiomxd; returns 0 on success.
    extern "C" int ioswitch_audio_publish(void)
{
    static bool published;
    if (published) return 0;
    IOService *resources = IOService::getResourceService();
    SwitchAudio *audio = OSTypeAlloc(SwitchAudio);
    if (!resources || !audio || !audio->init() || !audio->attach(resources)) {
        OSSafeReleaseNULL(audio);
        IOLog("SwitchAudio: publish failed\n");
        return -1;
    }
    if (!audio->start(resources)) {
        audio->detach(resources);
        audio->release();
        IOLog("SwitchAudio: start failed\n");
        return -1;
    }
    audio->release();  // the attachment keeps it
    published = true;
    IOLog("SwitchAudio: started\n");
    return 0;
}
