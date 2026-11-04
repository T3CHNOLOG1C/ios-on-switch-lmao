// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 SDMMC1 raw block device: exposes the SD card's single APFS GPT
// partition as a whole-disk IOBlockStorageDevice and lets IOKit/APFS do the rest.
//
// The card is inherited from U-Boot already initialized, selected and in transfer
// state, at 3.3 V high speed, 4-bit, 48 MHz (~24 MB/s raw): switchroot U-Boot's
// tegra_mmc.c caps the bus at f_max 48 MHz with only HS host caps, and IMG_5553 read
// back hc=0x02 clock=0x0007 (4-bit, CAR clock passed straight through). By default that
// mode is kept untouched. With boot-arg ioswitch-sd-uhs=1, setup() takes the card back
// through SD initialization into UHS-I SDR104 (1.8 V, 199.68 MHz, hardware-tuned, ~100 MB/s
// raw; slower tuned clocks if that has no sampling window) the way hekate's bdk does on this
// SoC; that path has not run on hardware yet, hence opt-in. Data moves by SDHCI v4 SDMA
// through one physically contiguous bounce buffer, CMD18/CMD25 with Auto CMD12, with
// queued requests that continue each other on the card merged into one command;
// completion is signalled by the SDMMC1 interrupt (polled only with boot-arg
// ioswitch-sd-irq=0; an interrupt that cannot be had or never arrives panics) on a
// dedicated worker thread so callers never block or recurse.
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <os/log.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSKext.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <sys/types.h>
#include <sys/param.h>
#include <pexpert/pexpert.h>
#include "../ArmGIC/IOSwitchInterrupts.h"
#include "../TegraI2C/TegraI2C.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "block_backend.h"
#include "NativeIOSwitchBlock.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

namespace {
constexpr uint64_t kControllerPA = 0x700B0000;  // SDMMC1
constexpr uint64_t kMemoryControllerPA = 0x70019000;
// What the SDR104 bring-up touches besides the controller (hekate bdk/soc/t210.h:47/58/64): the
// clock and reset controller, APB_MISC (pinmux at +0x3000) and the PMC's page (PMC is at 0x7000E400).
constexpr uint64_t kCarPA = 0x60006000, kApbMiscPA = 0x70000000, kPmcPagePA = 0x7000E000;
// SDMMC1's interrupt: GIC_SPI 14, level-high (mainline tegra210.dtsi mmc@700b0000; Atmosphere uses 46 too).
constexpr uint32_t kInterrupt = 46;
// Longest single interrupt wait before the worker checks STATUS for a lost interrupt (see command()).
constexpr uint64_t kInterruptCheckNs = 100000000;
constexpr uint32_t kSector = 512;
// r8-sdboot: one physically contiguous 1 MiB bounce buffer, aligned to the 512 KiB SDMA boundary. A transfer of more
// than 512 KiB pauses at each boundary with the DMA interrupt and resumes when the next address is written (command(),
// as Linux sdhci_data_irq does). setup() checks that on this controller with a 1 MiB read against four 256 KiB ones
// (which never reach a boundary) before allowing more than kSafeSectors per command; otherwise commands stay at
// 256 KiB as before. Larger commands halve or quarter the per-command card latency, copy and completion turn-around
// that held 11-04's 512 KiB prefetch runs (two 256 KiB commands each) to ~67 MB/s on a ~95 MB/s bus.
// Writes stay at kSafeSectors: the check only proves the boundary resume for reads, so nothing larger is ever written
// until a write check exists (a 256 KiB write from the start of the boundary-aligned bounce never reaches a boundary).
constexpr uint32_t kBounceBytes = 1024 * 1024;
constexpr uint32_t kBounceAlign = 512 * 1024;
constexpr uint32_t kSdmaBoundary = 512 * 1024;
constexpr uint32_t kBounceSectors = kBounceBytes / kSector;
constexpr uint32_t kSafeSectors = 256 * 1024 / kSector;
// Most requests one merged command carries (APFS issues >= 4 KiB, so 256 still fills the buffer).
constexpr uint32_t kMaxChain = 256;
const char kRootUUID[] = "04769D26-2CA0-4825-B443-E2E5F88300F2";
const uint8_t kApfsType[16] = {0xef, 0x57, 0x34, 0x7c, 0x00, 0x00, 0xaa, 0x11,
                               0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac};
// Fixed development identifier consumed by the owner boot-manifest code.
const uint8_t kRootIdentifier[48] = {0xfd, 0x86, 0xb9, 0x5b, 0x70, 0x33, 0xe3, 0x52, 0x84, 0x3f, 0x98, 0xd1,
                                     0x20, 0x4a, 0xde, 0x29, 0x45, 0x9b, 0xf6, 0x08, 0xa7, 0xe1, 0xcc, 0xb1,
                                     0xa9, 0xb0, 0x4a, 0xc9, 0x35, 0x33, 0x29, 0x2e, 0xb3, 0x9d, 0xe7, 0xc3,
                                     0x34, 0xeb, 0xe9, 0x8b, 0x43, 0xd0, 0xdb, 0x47, 0x0f, 0xa1, 0x6e, 0x77};

// SDHCI register offsets and bits.
enum : uint32_t {
    BLOCK_SIZE = 0x04,
    BLOCK_COUNT = 0x06,
    ARGUMENT = 0x08,
    TRANSFER = 0x0c,
    COMMAND = 0x0e,
    RESPONSE = 0x10,
    STOP_RESPONSE = 0x1c,
    PRESENT = 0x24,
    HOST_CONTROL = 0x28,
    POWER = 0x29,
    CLOCK = 0x2c,
    TIMEOUT = 0x2e,
    RESET = 0x2f,
    STATUS = 0x30,
    STATUS_ENABLE = 0x34,
    SIGNAL_ENABLE = 0x38,
    AUTO_CMD_ERROR = 0x3c,
    HOST_CONTROL2 = 0x3e,
    CAPABILITIES = 0x40,
    CAPABILITIES_HI = 0x44,
    DMA_LOW = 0x58,
    DMA_HIGH = 0x5c,
    VERSION = 0xfe,
    // Tegra vendor block (hekate bdk/storage/sdmmc_t210.h:90-119).
    VENDOR_CLOCK = 0x100,
    VENDOR_IO_TRIM = 0x1ac,
    VENDOR_TUNING0 = 0x1c0,
    VENDOR_TUNING1 = 0x1c4,
    PAD_CONTROL = 0x1e0,
    AUTOCAL_CONFIG = 0x1e4,
    AUTOCAL_STATUS = 0x1ec,
    IO_SPARE = 0x1f0,
};
enum : uint32_t {
    INT_COMMAND = 1u << 0,
    INT_TRANSFER = 1u << 1,
    INT_DMA = 1u << 3,
    INT_READ_READY = 1u << 5,
    INT_ERROR = 1u << 15,
    INT_ERRORS = 0xffff8000u,
    // Error signal enables are the upper half of SIGNAL_ENABLE (normal bit 15 is fixed 0 in SDHCI): the error
    // bits STATUS_ENABLE latches, 16..25.
    ERROR_SIGNALS = 0x03ff0000u,
    PRESENT_BUSY = 0x307u,
    PRESENT_DAT0 = 1u << 20,
    PRESENT_DAT = 0xfu << 20,
    PRESENT_CARD = 0x30000u,
    // R1 error bits (Linux's set plus SD AKE_SEQ and CID/CSD overwrite). A stop
    // after reading a card's final block may report OUT_OF_RANGE: ignore it there.
    R1_ERRORS = 0xfdf90008u,
    R1_STOP_ERRORS = 0x7df90008u,
    R1_APP_CMD = 1u << 5,
    // COMMAND low byte per response type: R1/R6/R7 48-bit + CRC + index, R1b the same with busy,
    // R2 136-bit + CRC, R3 48-bit unchecked; DATA marks a data command.
    RSP_NONE = 0x00,
    RSP_R1 = 0x1a,
    RSP_R1B = 0x1b,
    RSP_R2 = 0x09,
    RSP_R3 = 0x02,
    CMD_DATA = 0x20,
    // ACMD41 argument/response (hekate bdk/storage/sd_def.h:91-104).
    OCR_VDD_32_33 = 1u << 20,
    OCR_S18 = 1u << 24,
    OCR_XPC = 1u << 28,
    OCR_CCS = 1u << 30,
    OCR_READY = 1u << 31,
    // HOST_CONTROL2 (hekate sdmmc_driver.h:122-132): UHS mode 2:0, 1.8 V signalling, tuning.
    UHS_SDR12 = 0,
    UHS_SDR25 = 1,
    UHS_SDR104 = 3,
    CONTROL2_1V8 = 1u << 3,
    CONTROL2_EXEC_TUNING = 1u << 6,
    CONTROL2_TUNED = 1u << 7,
};
// Clock and reset controller (hekate bdk/soc/clock.h:25/73/106/107/148/149/177/179/196/197; CLK_L_SDMMC1 = 14
// at :488), APB_MISC/pinmux (t210.h:242, pinmux.h:24/26/30-35/130) and PMC (pmc_t210.h:113-114) offsets.
enum : uint32_t {
    CAR_RST_DEVICES_L = 0x004,
    CAR_RST_DEV_L_SET = 0x300,
    CAR_RST_DEV_L_CLR = 0x304,
    CAR_CLK_SOURCE_SDMMC1 = 0x150,
    CAR_CLK_ENB_L_SET = 0x320,
    CAR_CLK_ENB_L_CLR = 0x324,
    CAR_PLLC4_BASE = 0x5a4,
    CAR_PLLC4_MISC = 0x5a8,
    CAR_SDMMC1 = 1u << 14,
    PLL_LOCK = 1u << 27,
    PLL_ENABLE = 1u << 30,
    PLLC4_IDDQ = 1u << 18,
    PLLC4_LOCK_DETECT = 1u << 30,
    // PLLC4 = 38.4 MHz / M 4 * N 104 = 998.4 MHz, P 1; OUT2 is VCO / 5 = 199.68 MHz (hekate clock.c:602).
    PLLC4_CONFIG = (104u << 8) | 4u,
    PLLC4_DIVIDERS = 0x00f8ffffu,
    // CLK_SOURCE bits 31:29 (hekate clock.c:725-726); PLLP_OUT0 is 408 MHz with a U7.1 divider below.
    SOURCE_PLLP = 0,
    SOURCE_PLLC4_OUT2 = 3,
    PLLP_KHZ = 408000,
    APB_GP_HIDREV = 0x804,
    APB_SDMMC1_CLK_LOOPBACK = 0x8d4,
    APB_SDMMC1_PAD_CONFIG = 0xa98,
    PINMUX_SDMMC1 = 0x3000,
    PINMUX_SCHMITT = 1u << 12,  // CLK, CMD, DAT3, DAT2, DAT1, DAT0 at +0..+0x14
    PMC_PWR_DET_VAL = 0x400 + 0xe4,
    PMC_SDMMC1_3V3 = 1u << 12,
    PMC_NO_IOPOWER = 0x400 + 0x44,
    PMC_SDMMC1_NO_POWER = 1u << 12,
};
// System sleep (hekate sdmmc1_disable_power / _sdmmc_config_sdmmc1 / sdmmc_init): the card's own supply is switched by
// GPIO PE4 (DMIC3_CLK), its I/O supply is LDO2; U-Boot's card clock is PLLP / 8.5 = 48 MHz (CLK_SOURCE U7.1 divider 15),
// and B01's SDMMC1 trimmer is 14 (VENDOR_CLOCK 28:24, hekate trim_values_t210b01).
constexpr unsigned kCardPowerGpio = TEGRA_GPIO(E, 4);
constexpr uint32_t kPllpDivider48MHz = 15, kTrimB01 = 14;
// MAX77620 LDO2 supplies the SDMMC1 pads (vddio_sdmmc1): LDO2_CFG 0x27, bits 5:0 = (uV - 0.8 V) / 50 mV, bits
// 7:6 power mode (hekate power/max77620.h:14/144/179, max7762x.c:83).
constexpr unsigned kPmicBus = 5;
constexpr uint8_t kPmicAddress = 0x3c, kPmicLdo2 = 0x27, kLdoVoltage = 0x3f, kLdo3V3 = 50, kLdo1V8 = 20;
// T210B01 SDMMC1 default sampling tap (hekate sdmmc_driver.c:131; U-Boot tegra_mmc.c:540 the same).
constexpr uint32_t kTapB01 = 11;
// SDR104 clocks, fastest first. The card stays in the SDR104 mode it switched to and the host in SDR104
// timing, re-tuned at each clock, as hekate's own fallback SDR82 does (sdmmc.c:1556-1561 sets the card to
// SDR104, sdmmc_driver.c:340 the host to SDR104 timing, clock.c:772-774 PLLP / 2.5, sdmmc_driver.c:771
// tunes it; sd.c:96-117 steps down to it after errors). Below that, the SDHCI divider halves
// PLLC4_OUT2. carDivider is CLK_SOURCE's U7.1 field (hekate CLK_SRC_DIV(d) = 2 * (d - 1),
// clock.h:202), divider the SDHCI one (base / 2n, 0 = base).
struct Speed {
    uint32_t source, carDivider, divider, kHz;
};
constexpr Speed kSpeeds[] = {{SOURCE_PLLC4_OUT2, 0, 0, 199680},
                             {SOURCE_PLLP, 3, 0, 163200},
                             {SOURCE_PLLC4_OUT2, 0, 1, 99840},
                             {SOURCE_PLLC4_OUT2, 0, 2, 49920}};
constexpr unsigned kSpeedCount = sizeof(kSpeeds) / sizeof(kSpeeds[0]);

inline uint64_t nowNs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}
inline uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
inline uint64_t le64(const uint8_t *p)
{
    return le32(p) | (uint64_t)le32(p + 4) << 32;
}
inline uint32_t rd(volatile uint8_t *base, uint32_t o)
{
    return *(volatile uint32_t *)(base + o);
}
inline void wr(volatile uint8_t *base, uint32_t o, uint32_t v)
{
    *(volatile uint32_t *)(base + o) = v;
}

struct Request {
    Request *next;
    IOMemoryDescriptor *buffer;
    uint64_t lba;  // absolute card LBA
    uint32_t sectors;
    bool write;
    IOStorageCompletion completion;
    // r6-lows: the request's IOStoragePriority class (IOStorageAttributes priority >> 6: 0 high (I/O tier 0, the UI's page-ins),
    // 1 default, 2 low, 3 background) and when it was queued.
    uint8_t klass;
    uint64_t queuedNs;
};
// r6-lows: a high-class read may be taken before older requests of lower classes (worker), never before an older request it
// overlaps, and never once the oldest request has waited kAgeNs (then FIFO again, so nothing starves). Writes keep FIFO order.
constexpr unsigned kClasses = 4;
constexpr uint64_t kAgeNs = 100000000;  // 100 ms
// Queue time (submit to the command that carries it) per class over one throughput line's interval.
struct ClassWait {
    uint64_t n, sumNs, maxNs;
};

struct Stats {
    uint64_t reads, readBytes, writes, writeBytes, errors, retries;
    uint64_t maxLatencyNs;
    uint32_t lastError, lastStatus;
    // Throughput: card commands after merging and the worker time they took (bus, card and copies; the
    // completion callbacks that run on the worker afterwards are completeNs), when the first request
    // arrived, requests that began where the previous one ended (sequential) or rode in another's
    // command (merged), requests of at most 4 KiB, SDMMC1 interrupts taken, SDR104 re-tunes, and the
    // deepest the queue got.
    uint64_t commands, busyNs, completeNs, firstNs, sequential, merged, small, interrupts, retunes;
    uint32_t maxQueued;
};
}

class NativeIOSwitchBlock;

class NativeIOSwitchBlockDevice : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(NativeIOSwitchBlockDevice) NativeIOSwitchBlock *owner = nullptr;
    uint64_t blocks = 0;

public:
    bool configure(NativeIOSwitchBlock *, uint64_t sectors);
    IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
    IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
    UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
    IOReturn doUnmap(IOBlockStorageDeviceExtent *, UInt32, IOStorageUnmapOptions) override
    {
        return kIOReturnUnsupported;
    }
    char *getVendorString() override { return const_cast<char *>("IOSwitch"); }
    char *getProductString() override { return const_cast<char *>("SD APFS"); }
    char *getRevisionString() override { return const_cast<char *>("2.0"); }
    char *getAdditionalDeviceInfoString() override { return const_cast<char *>("SDMMC1 APFS partition"); }
    IOReturn reportBlockSize(UInt64 *value) override
    {
        *value = kSector;
        return kIOReturnSuccess;
    }
    IOReturn reportMaxValidBlock(UInt64 *value) override
    {
        *value = blocks - 1;
        return kIOReturnSuccess;
    }
    IOReturn reportEjectability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }
    IOReturn reportRemovability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }
    IOReturn reportWriteProtection(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }
    IOReturn reportMediaState(bool *present, bool *changed) override
    {
        *present = true;
        if (changed) *changed = false;
        return kIOReturnSuccess;
    }
    // Writes complete only after the card releases busy and no card cache is
    // enabled, so every completed write is already durable.
    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
    IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
    IOReturn getWriteCacheState(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }
    IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *,
                              IOStorageCompletion *) override;
};

class NativeIOSwitchBlock : public IOService {
    OSDeclareDefaultStructors(NativeIOSwitchBlock) volatile uint8_t *regs = nullptr;
    volatile uint8_t *car = nullptr, *apb = nullptr, *pmc = nullptr;  // SDR104 bring-up only
    IOBufferMemoryDescriptor *bounceMemory = nullptr;
    uint8_t *bounce = nullptr;
    uint64_t bouncePA = 0;
    IOLock *queueLock = nullptr;
    Request *head = nullptr, *tail = nullptr;
    uint32_t queued = 0;                   // requests waiting, under queueLock
    uint32_t queuedReads[kClasses] = {};   // r6-lows: reads waiting per class, under queueLock
    ClassWait waits[kClasses] = {};        // r6-lows: per class since the last throughput line, under queueLock
    uint64_t passed = 0, passedTotal = 0;  // r6-lows: reads taken before older requests (this line, since boot)
    uint64_t submitted = 0, serviced = 0;  // for the root-wait report
    uint64_t lastEnd = 0;                  // card LBA after the previous request (sequential count)
    uint64_t statsEveryNs = 0, statsLastNs = 0;
    bool merge = true;
    // SDMMC1 interrupt: the controller's line is (STATUS & SIGNAL_ENABLE). The worker enables the signals it
    // waits for and sleeps on irqEvent; the filter disables them again (lowering the line) and wakes it.
    IOWorkLoop *irqLoop = nullptr;
    IOFilterInterruptEventSource *irqSource = nullptr;
    bool irqMode = false;
    int irqEvent = 0;
    // Bus state: what the card runs at, its RCA after our re-initialization, the last STATUS a bring-up
    // command saw, and whether SDR104 tuning is in effect (a failed transfer is re-tuned before its retry)
    // at which kSpeeds step.
    const char *busMode = "inherited";
    uint32_t busKHz = 0;
    uint32_t rca = 0, initStatus = 0;
    bool tuned = false;
    unsigned speed = 0;
    // System sleep: the worker stops between commands while suspended; the card was in 1.8 V signalling at boot (so it
    // is taken back there after SC7); LBA 1 as read at boot, compared after re-initialization.
    bool suspended = false, working = false, lowVoltageBus = false;
    uint8_t gptHeader[kSector] = {};

    uint8_t r8(uint32_t o) { return regs[o]; }
    uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(regs + o); }
    uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
    void w8(uint32_t o, uint8_t v) { regs[o] = v; }
    void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(regs + o) = v; }
    void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }
    static void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }

    bool waitIdle(bool dat0, uint64_t timeoutNs);
    bool startInterrupt();
    static bool interruptFilter(OSObject *, IOFilterInterruptEventSource *);
    bool waitSignal(uint32_t signals, uint64_t timeoutNs);
    void resetLines();
    void recover(bool data);
    int command(uint32_t lba, uint32_t sectors, bool write, uint32_t *status);
    static void worker(void *, wait_result_t);
    void serviceBatch(Request *, uint32_t sectors);
    void logStats();
    void logThroughput();
    // SDR104 bring-up (bounded register polling, setup() and re-tuning only).
    int cardCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data = 0);
    int appCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response);
    int switchFunction(bool set, unsigned group, uint32_t value);
    void setTiming(int uhs, bool highSpeed, int source, uint32_t carDivider, uint32_t divider);
    bool calibratePads();
    bool switchTo1V8(uint8_t ldo2);
    bool enablePllc4();
    void tuneStep();
    bool tune();
    bool clockAndTune(unsigned from);
    void retune(bool slower);
    bool raiseBusSpeed();
    bool reinitialize(uint32_t baseKHz, uint8_t ldo2, bool uhs, bool resume);
    void powerOffCard();
    void powerOnController();

public:
    void suspend();
    void resume();
    Stats stats = {};
    uint64_t firstSector = 0, sectorCount = 0;
    uint32_t maxSectors = kSafeSectors;  // per command; kBounceSectors once setup() has checked SDMA boundary resumes
    NativeIOSwitchBlockDevice *device = nullptr;

    bool setup();
    int transfer(uint64_t lba, uint32_t sectors, bool write, uint8_t *bytes);
    void submit(Request *);
    void reportState(const char *why);
};

OSDefineMetaClassAndStructors(NativeIOSwitchBlockDevice, IOBlockStorageDevice)
    OSDefineMetaClassAndStructors(NativeIOSwitchBlock, IOService)

        namespace
{
    NativeIOSwitchBlock *gOwner = nullptr;
    int gPublishStatus = -1;
    // r8-sdboot: the boot prefetch thread (IOSwitchCryptex.cpp). Its reads, issued from that thread down to here
    // (cluster_io -> spec_strategy -> IOMediaBSDClient -> IOBlockStorageDriver -> doAsyncReadWrite), take the high class,
    // first come first served with the foreground's reads: in the background class they waited out the 100 ms aging
    // behind a saturated foreground (11-04 r7h: 965 MiB took 7.5 s to 68.3 s uptime, ~2.5 MB/s after the first 10 s),
    // while the foreground page-faulted the same blocks one 4 KiB read at a time.
    thread_t gPrefetchThread = THREAD_NULL;
}

extern "C" void ioswitch_block_prefetch_thread(thread_t thread)
{
    __atomic_store_n(&gPrefetchThread, thread, __ATOMIC_RELEASE);
}

bool NativeIOSwitchBlockDevice::configure(NativeIOSwitchBlock *provider, uint64_t sectors)
{
    owner = provider;
    blocks = sectors;
    setProperty(kIOMaximumBlockCountReadKey, provider->maxSectors, 64);
    setProperty(kIOMaximumBlockCountWriteKey, kSafeSectors, 64);
    setProperty(kIOMaximumByteCountReadKey, provider->maxSectors * kSector, 64);
    setProperty(kIOMaximumByteCountWriteKey, kSafeSectors * kSector, 64);
    if (OSDictionary *protocol = OSDictionary::withCapacity(2)) {
        protocol->setObject(kIOPropertyPhysicalInterconnectTypeKey, OSString::withCStringNoCopy("SD"));
        protocol->setObject(kIOPropertyPhysicalInterconnectLocationKey,
                            OSString::withCStringNoCopy(kIOPropertyInternalKey));
        setProperty(kIOPropertyProtocolCharacteristicsKey, protocol);
        protocol->release();
    }
    if (OSDictionary *device = OSDictionary::withCapacity(1)) {
        device->setObject(kIOPropertyMediumTypeKey, OSString::withCStringNoCopy(kIOPropertyMediumTypeSolidStateKey));
        setProperty(kIOPropertyDeviceCharacteristicsKey, device);
        device->release();
    }
    setName("IOSwitchSD");
    return true;
}

IOReturn NativeIOSwitchBlockDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                                     IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !count || block >= blocks || count > blocks - block || count > owner->maxSectors ||
        buffer->getLength() < count * kSector)
        return kIOReturnBadArgument;
    IODirection direction = buffer->getDirection();
    if (direction != kIODirectionIn && direction != kIODirectionOut) return kIOReturnBadArgument;
    if (direction == kIODirectionOut && count > kSafeSectors) return kIOReturnBadArgument;
    auto *request = static_cast<Request *>(IOMalloc(sizeof(Request)));
    if (!request) return kIOReturnNoMemory;
    buffer->retain();
    // No attributes: the default class, as IOStorage's own default priority. The boot prefetch: high.
    const IOStoragePriority priority =
        current_thread() == __atomic_load_n(&gPrefetchThread, __ATOMIC_ACQUIRE) ? kIOStoragePriorityHigh
        : attributes                                                             ? attributes->priority
                                                                                 : kIOStoragePriorityDefault;
    *request = {nullptr,
                buffer,
                owner->firstSector + block,
                static_cast<uint32_t>(count),
                direction == kIODirectionOut,
                *completion,
                static_cast<uint8_t>(priority >> 6),
                0};
    owner->submit(request);
    return kIOReturnSuccess;
}

bool NativeIOSwitchBlock::waitIdle(bool dat0, uint64_t timeoutNs)
{
    uint64_t start = nowNs();
    for (;;) {
        uint32_t present = r32(PRESENT);
        if (!(present & PRESENT_BUSY) && (!dat0 || (present & PRESENT_DAT0))) return true;
        uint64_t elapsed = nowNs() - start;
        if (elapsed >= timeoutNs) return false;
        if (elapsed > 1000000) IOSleep(1);
    }
}

// Primary interrupt context. Masking the signals lowers the level line, so there is
// nothing left for a work loop: STATUS stays for the worker to read and clear.
bool NativeIOSwitchBlock::interruptFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<NativeIOSwitchBlock *>(owner);
    if (!(self->r32(STATUS) & self->r32(SIGNAL_ENABLE))) return false;
    self->w32(SIGNAL_ENABLE, 0);
    (void)self->r32(SIGNAL_ENABLE);  // the line is low before ArmCpu ends the interrupt
    __atomic_fetch_add(&self->stats.interrupts, 1, __ATOMIC_RELAXED);
    thread_wakeup(&self->irqEvent);
    return false;
}

// Sleeps until the controller raises one of `signals` or timeoutNs passes; false on
// timeout. The wait is asserted before the signals are enabled, so an interrupt that
// fires at once (a bit already latched) still wakes this thread.
bool NativeIOSwitchBlock::waitSignal(uint32_t signals, uint64_t timeoutNs)
{
    uint64_t interval = 0;
    nanoseconds_to_absolutetime(timeoutNs, &interval);
    assert_wait_deadline(&irqEvent, THREAD_UNINT, mach_absolute_time() + interval);
    w32(SIGNAL_ENABLE, signals);
    const wait_result_t result = thread_block(THREAD_CONTINUE_NULL);
    w32(SIGNAL_ENABLE, 0);
    return result == THREAD_AWAKENED;
}

bool NativeIOSwitchBlock::startInterrupt()
{
    unsigned enabled = 1;
    PE_parse_boot_argn("ioswitch-sd-irq", &enabled, sizeof(enabled));
    if (!enabled) return false;
    IOService *nub = ioswitch_interrupt_nub(kInterrupt, false);
    irqLoop = IOWorkLoop::workLoop();
    // Filter only: it never schedules the (absent) work-loop action.
    irqSource = irqLoop ? IOFilterInterruptEventSource::filterInterruptEventSource(
                              this, nullptr, &NativeIOSwitchBlock::interruptFilter, nub, 0)
                        : nullptr;
    if (!irqSource || irqLoop->addEventSource(irqSource) != kIOReturnSuccess) {
        panic("IOSwitchSD: cannot attach the SDMMC1 interrupt (INTID %u)", kInterrupt);
    }
    irqSource->enable();
    return true;
}

// Software-reset the CMD and DAT state machines (hekate _sdmmc_reset_cmd_data); STATUS is left alone.
void NativeIOSwitchBlock::resetLines()
{
    w8(RESET, 0x06);
    for (uint64_t start = nowNs(); (r8(RESET) & 0x06) && nowNs() - start < 100000000;) {}
}

// Reset the CMD and DAT state machines; for a failed data command,
// also stop the card (CMD12, abort type, R1b) so it returns to transfer state.
void NativeIOSwitchBlock::recover(bool data)
{
    resetLines();
    w32(STATUS, 0xffffffff);
    if (!data) return;
    if (!waitIdle(false, 100000000)) return;
    w16(TRANSFER, 0);
    w32(ARGUMENT, 0);
    barrier();
    w16(COMMAND, 0x0cdb);
    for (uint64_t start = nowNs(); nowNs() - start < 100000000;) {
        uint32_t s = r32(STATUS);
        if (s & (INT_COMMAND | INT_ERROR)) break;
    }
    w32(STATUS, 0xffffffff);
    if (!waitIdle(true, 2000000000)) {
        resetLines();
        w32(STATUS, 0xffffffff);
    }
}

// One SDMA command into/out of the bounce buffer. Returns 0 or a nonzero step
// code; *status receives the last interrupt status sample.
int NativeIOSwitchBlock::command(uint32_t lba, uint32_t sectors, bool write, uint32_t *status)
{
    *status = 0;
    if (!waitIdle(write, 2000000000)) return 1;
    w32(STATUS, 0xffffffff);
    w32(DMA_LOW, static_cast<uint32_t>(bouncePA));
    w32(DMA_HIGH, static_cast<uint32_t>(bouncePA >> 32));
    w16(BLOCK_SIZE, kSector | 0x7000);  // 512 KiB SDMA boundary
    w16(BLOCK_COUNT, static_cast<uint16_t>(sectors));
    w32(ARGUMENT, lba);
    const bool multi = sectors > 1;
    // DMA | block-count | Auto CMD12 | read | multi, as the qualified sequence.
    w16(TRANSFER, static_cast<uint16_t>(1 | (multi ? 0x26 : 0) | (write ? 0 : 0x10)));
    barrier();
    w16(COMMAND, static_cast<uint16_t>(((write ? (multi ? 25 : 24) : (multi ? 18 : 17)) << 8) | 0x3a));

    // One sleep per command: the worker waits for transfer complete, an error, or an SDMA boundary
    // (transfers over 512 KiB). Command complete latches in STATUS without raising the line and its R1 is
    // checked once the data has moved; a command the card refuses moves no data and so ends as a
    // data error or the time limit below. (Waiting for command complete first cost a second
    // interrupt and context switch per request, a real share of a ~4 KiB read.)
    const uint32_t awaited = INT_TRANSFER | INT_DMA | ERROR_SIGNALS;
    const uint64_t start = nowNs();
    const uint64_t limit = write ? 5000000000ull : 2000000000ull;
    uint32_t s, resume = 0;
    for (;;) {
        s = r32(STATUS);
        *status = s;
        if (s & INT_ERRORS) return (s & INT_COMMAND) ? 3 : 2;
        if (s & INT_TRANSFER) break;
        if (s & INT_DMA) {
            // Paused at the next 512 KiB boundary of the (boundary-aligned) bounce buffer: give it the address there.
            resume += kSdmaBoundary;
            if (resume > sectors * kSector) return 5;
            w32(STATUS, INT_DMA);
            w32(DMA_LOW, static_cast<uint32_t>(bouncePA + resume));
            w32(DMA_HIGH, static_cast<uint32_t>((bouncePA + resume) >> 32));
            continue;
        }
        uint64_t elapsed = nowNs() - start;
        if (elapsed >= limit) return (s & INT_COMMAND) ? 6 : 7;
        if (irqMode) {
            // Sleep until the transfer completes or fails. A wait that times out with an awaited
            // bit latched means the interrupt does not reach the CPU.
            const uint64_t left = limit - elapsed;
            if (!waitSignal(awaited, left < kInterruptCheckNs ? left : kInterruptCheckNs) && (r32(STATUS) & awaited)) {
                char gic[192];
                ioswitch_interrupt_describe(kInterrupt, gic, sizeof(gic));
                panic("IOSwitchSD: SDMMC1 interrupt lost: STATUS %#x awaited %#x, %s", r32(STATUS), awaited, gic);
            }
            continue;
        }
        // Spin through ordinary reads; yield the CPU for long writes and slow reads.
        if (elapsed > (write ? 1000000ull : 3000000ull)) IOSleep(1);
    }
    w32(STATUS, INT_COMMAND | INT_TRANSFER);
    if (!(s & INT_COMMAND) || (r32(RESPONSE) & R1_ERRORS)) return 4;
    if (multi) {
        if (r16(AUTO_CMD_ERROR) & 0x9f) return 8;
        if (r32(STOP_RESPONSE) & R1_STOP_ERRORS) return 9;
    }
    // Transfer complete follows the write busy period; DAT0 confirms it.
    if (!waitIdle(write, 2000000000)) return 10;
    return 0;
}

int NativeIOSwitchBlock::transfer(uint64_t lba, uint32_t sectors, bool write, uint8_t *bytes)
{
    const uint32_t length = sectors * kSector;
    if (lba > UINT32_MAX || sectors == 0 || sectors > (write ? kSafeSectors : kBounceSectors)) return -1;
    if (write && bytes != bounce) memcpy(bounce, bytes, length);
    // Clean (write) or clean+invalidate (read) before the device owns the lines.
    for (uintptr_t at = (uintptr_t)bounce, end = at + length; at < end; at += 64)
        __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
    barrier();
    uint32_t status = 0;
    int step = command(static_cast<uint32_t>(lba), sectors, write, &status);
    if (step) {
        uint32_t present = r32(PRESENT);
        IOLog("IOSwitchSD: %s lba=%llu n=%u failed step=%d status=0x%08x present=0x%08x acmd=0x%04x\n",
              write ? "write" : "read", lba, sectors, step, status, present, r16(AUTO_CMD_ERROR));
        stats.errors++;
        stats.lastError = static_cast<uint32_t>(step);
        stats.lastStatus = status;
        recover(true);
        stats.retries++;
        // At SDR104 a data error is most often a sampling point that drifted (temperature): tune
        // again before the one retry, as the Linux MMC core re-tunes on CRC errors.
        if (tuned) retune(false);
        if (write) {
            for (uintptr_t at = (uintptr_t)bounce, end = at + length; at < end; at += 64)
                __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
            barrier();
        }
        step = command(static_cast<uint32_t>(lba), sectors, write, &status);
        if (step) {
            IOLog("IOSwitchSD: retry %s lba=%llu n=%u failed step=%d status=0x%08x present=0x%08x\n",
                  write ? "write" : "read", lba, sectors, step, status, r32(PRESENT));
            stats.errors++;
            stats.lastError = static_cast<uint32_t>(step);
            stats.lastStatus = status;
            recover(true);
            // Failed again right after a re-tune: later I/O runs one clock step slower (hekate's own
            // answer to errors at SDR104 is its slower SDR82, sd.c:96-117).
            if (tuned && speed + 1 < kSpeedCount) retune(true);
            return step;
        }
    }
    if (!write) {
        for (uintptr_t at = (uintptr_t)bounce, end = at + length; at < end; at += 64)
            __asm__ volatile("dc ivac, %0" ::"r"(at) : "memory");
        barrier();
        if (bytes != bounce) memcpy(bytes, bounce, length);
    }
    return 0;
}

void NativeIOSwitchBlock::submit(Request *request)
{
    request->queuedNs = nowNs();
    IOLockLock(queueLock);
    if (tail)
        tail->next = request;
    else
        head = request;
    tail = request;
    if (++queued > stats.maxQueued) stats.maxQueued = queued;
    if (!request->write) ++queuedReads[request->klass];
    if (!stats.firstNs) stats.firstNs = statsLastNs = request->queuedNs;
    __atomic_fetch_add(&submitted, 1, __ATOMIC_RELAXED);
    IOLockWakeup(queueLock, &head, true);
    IOLockUnlock(queueLock);
}

// One card command for a chain of requests that continue each other on the card (same
// direction, each starting where the previous ended, worker() built it in FIFO order),
// staged through the bounce buffer at their offsets; then every request completes with
// its own length. A single request is a chain of one.
void NativeIOSwitchBlock::serviceBatch(Request *first, uint32_t sectors)
{
    // Each request was checked against the partition window in doAsyncReadWrite and the
    // chain is their contiguous union; checking the command itself keeps any bug here from
    // ever reaching outside the APFS partition.
    if (first->lba < firstSector || sectors > sectorCount || first->lba - firstSector > sectorCount - sectors)
        panic("IOSwitchSD: command lba=%llu n=%u outside the APFS partition [%llu, +%llu)", first->lba, sectors,
              firstSector, sectorCount);
    const bool write = first->write;
    const uint64_t start = nowNs();
    IOReturn result = kIOReturnSuccess;
    if (write) {
        uint32_t offset = 0;
        for (Request *r = first; r; r = r->next) {
            const uint32_t length = r->sectors * kSector;
            if (r->buffer->readBytes(0, bounce + offset, length) != length) result = kIOReturnIOError;
            offset += length;
        }
        // Stage directly in the bounce buffer; transfer() would copy onto itself.
        if (!result && transfer(first->lba, sectors, true, bounce)) result = kIOReturnIOError;
    } else if (transfer(first->lba, sectors, false, bounce)) {
        result = kIOReturnIOError;
    }
    // Copies first, completions after: busy time covers the card and the copies, completeNs the
    // callbacks (IOStorageFamily/APFS work that runs on this thread before the next command).
    IOReturn results[kMaxChain];
    unsigned count = 0;
    uint32_t offset = 0;
    for (Request *r = first; r; r = r->next, ++count) {
        const uint32_t length = r->sectors * kSector;
        IOReturn status = result;
        if (!write && !status && r->buffer->writeBytes(0, bounce + offset, length) != length) status = kIOReturnIOError;
        results[count] = status;
        offset += length;
        if (write) {
            stats.writes++;
            if (!status) stats.writeBytes += length;
        } else {
            stats.reads++;
            if (!status) stats.readBytes += length;
        }
        if (r->sectors <= 8) stats.small++;
        if (r->lba == lastEnd) stats.sequential++;
        lastEnd = r->lba + r->sectors;
    }
    const uint64_t done = nowNs(), latency = done - start;
    stats.commands++;
    stats.busyNs += latency;
    stats.merged += count - 1;
    if (latency > stats.maxLatencyNs) stats.maxLatencyNs = latency;
    count = 0;
    for (Request *r = first; r; ++count) {
        Request *next = r->next;
        const IOReturn status = results[count];
        const uint32_t length = r->sectors * kSector;
        IOMemoryDescriptor *buffer = r->buffer;
        IOStorageCompletion completion = r->completion;
        IOFree(r, sizeof(*r));
        buffer->release();
        // IOStorage::complete, without binding another original IOStorageFamily symbol.
        if (completion.action) completion.action(completion.target, completion.parameter, status, status ? 0 : length);
        __atomic_fetch_add(&serviced, 1, __ATOMIC_RELAXED);
        r = next;
    }
    stats.completeNs += nowNs() - done;
}

void NativeIOSwitchBlock::worker(void *context, wait_result_t)
{
    auto *self = static_cast<NativeIOSwitchBlock *>(context);
    for (;;) {
        IOLockLock(self->queueLock);
        while (!self->head || self->suspended) IOLockSleep(self->queueLock, &self->head, THREAD_UNINT);
        self->working = true;
        // Take the head and every queued request that continues it on the card in the same
        // direction, up to one bounce buffer: one command, one card access latency, one
        // interrupt instead of several. No write passes another.
        Request *prev = nullptr, *first = self->head;
        const uint64_t pickNs = nowNs();
        // r6-lows: a read of a higher IOStoragePriority class goes first (the UI's page-ins, I/O tier 0, behind up to ~100 queued
        // background requests at ~0.5 ms each otherwise): the oldest read of the best class queued, unless it overlaps an older
        // request or the head has waited kAgeNs.
        if (first->next && first->klass && pickNs - first->queuedNs < kAgeNs) {
            unsigned better = 0;
            while (better < first->klass && !self->queuedReads[better]) ++better;
            if (better < first->klass) {
                for (Request *before = first, *candidate = first->next; candidate;
                     before = candidate, candidate = candidate->next) {
                    if (candidate->write || candidate->klass != better) continue;
                    bool overlaps = false;
                    for (Request *older = self->head; older != candidate && !overlaps; older = older->next)
                        overlaps = older->lba < candidate->lba + candidate->sectors &&
                                   candidate->lba < older->lba + older->sectors;
                    if (overlaps) continue;
                    prev = before;
                    first = candidate;
                    ++self->passed;
                    ++self->passedTotal;
                    break;
                }
            }
        }
        Request *last = first;
        uint32_t sectors = first->sectors, count = 1;
        // A passing read's chain passes the older requests too: each request it takes must not overlap any of them either.
        auto overlapsOlder = [&](const Request *later) {
            if (!prev) return false;
            for (const Request *older = self->head; older != first; older = older->next)
                if (older->lba < later->lba + later->sectors && later->lba < older->lba + older->sectors) return true;
            return false;
        };
        while (self->merge && count < kMaxChain && last->next && last->next->write == first->write &&
               last->next->lba == last->lba + last->sectors &&
               sectors + last->next->sectors <= (first->write ? kSafeSectors : self->maxSectors) &&
               !overlapsOlder(last->next)) {
            last = last->next;
            sectors += last->sectors;
            ++count;
        }
        // Unlink first..last (the head's run, or a passing read's from the middle).
        if (prev)
            prev->next = last->next;
        else
            self->head = last->next;
        if (self->tail == last) self->tail = prev;
        last->next = nullptr;
        self->queued -= count;
        for (Request *r = first; r; r = r->next) {
            if (!r->write) --self->queuedReads[r->klass];
            ClassWait &wait = self->waits[r->klass];
            const uint64_t waited = pickNs > r->queuedNs ? pickNs - r->queuedNs : 0;
            ++wait.n;
            wait.sumNs += waited;
            if (waited > wait.maxNs) wait.maxNs = waited;
        }
        IOLockUnlock(self->queueLock);
        self->serviceBatch(first, sectors);
        IOLockLock(self->queueLock);
        self->working = false;
        if (self->suspended) IOLockWakeup(self->queueLock, &self->working, false);
        IOLockUnlock(self->queueLock);
        // Throughput line every ioswitch-sd-stats seconds while I/O is happening (one console line).
        if (self->statsEveryNs) {
            const uint64_t now = nowNs();
            if (now - self->statsLastNs >= self->statsEveryNs) {
                self->statsLastNs = now;
                self->logThroughput();
            }
        }
    }
}

void NativeIOSwitchBlock::logStats()
{
    IOLog("IOSwitchSD: reads=%llu (%llu KiB) writes=%llu (%llu KiB) errors=%llu max=%lluus last=%u/0x%x %s irq=%llu "
          "cmds=%llu callbacks=%llums\n",
          stats.reads, stats.readBytes >> 10, stats.writes, stats.writeBytes >> 10, stats.errors,
          stats.maxLatencyNs / 1000, stats.lastError, stats.lastStatus, irqMode ? "interrupt" : "polled",
          stats.interrupts, stats.commands, stats.completeNs / 1000000);
    logThroughput();
}

// One line to compare boots from a photo. busy is worker time per card command (bus, card and
// bounce copies, not completions); "of" is wall time since the first request, so the percentage
// is how much of that the SD card was the thing being waited on. MB/s is over busy time: the
// card's effective rate at the request sizes APFS actually issues.
void NativeIOSwitchBlock::logThroughput()
{
    const uint64_t requests = stats.reads + stats.writes, bytes = stats.readBytes + stats.writeBytes;
    const uint64_t wall = stats.firstNs ? nowNs() - stats.firstNs : 0, busy = stats.busyNs;
    const uint64_t rate = busy ? bytes * 10000 / busy : 0;  // bytes/ns * 1000 = MB/s, in tenths
    // r6-lows: queue time per IOStoragePriority class since the previous line (requests, mean and max in ms; hi = I/O tier 0,
    // the UI's own page-ins), and the reads taken before older requests (this line / since boot). A string argument: os_log
    // drops a message whose scalar arguments pass 256 bytes.
    char classes[200];
    {
        static const char *const names[kClasses] = {"hi", "def", "lo", "bg"};
        size_t at = 0;
        IOLockLock(queueLock);
        for (unsigned c = 0; c < kClasses; ++c) {
            const ClassWait w = waits[c];
            if (at < sizeof(classes))
                at += (size_t)snprintf(classes + at, sizeof(classes) - at, " %s %llu/%llu.%llu/%llu", names[c], w.n,
                                       w.n ? w.sumNs / w.n / 1000000 : 0, w.n ? w.sumNs / w.n / 100000 % 10 : 0,
                                       w.maxNs / 1000000);
            waits[c] = {};
        }
        if (at < sizeof(classes))
            snprintf(classes + at, sizeof(classes) - at, " passed %llu/%llu", passed, passedTotal);
        passed = 0;
        IOLockUnlock(queueLock);
    }
    // Periodic: to the log only (os_log), never the boot console.
    char out[512];
    snprintf(out, sizeof(out),
             "IOSwitchSD: %llu rq %llu MiB avg %lluK | busy %llu.%llus %llu.%llu MB/s %llu%% of %llus | "
             "<=4K %llu%% seq %llu%% merged %llu q%u | err %llu retune %llu | %s %u.%02u MHz | wait ms%s",
             requests, bytes >> 20, requests ? (bytes / requests) >> 10 : 0, busy / 1000000000, busy / 100000000 % 10,
             rate / 10, rate % 10, wall ? busy * 100 / wall : 0, wall / 1000000000,
             requests ? stats.small * 100 / requests : 0, requests ? stats.sequential * 100 / requests : 0,
             stats.merged, stats.maxQueued, stats.errors, stats.retunes, busMode, busKHz / 1000, busKHz % 1000 / 10,
             classes);
    os_log(OS_LOG_DEFAULT, "%s", out);
}

void NativeIOSwitchBlock::reportState(const char *why)
{
    logStats();
    IOLog("IOSwitchSD: %s present=0x%08x status=0x%08x hc=0x%02x hc2=0x%04x clock=0x%04x submitted=%llu serviced=%llu "
          "queued=%d\n",
          why, r32(PRESENT), r32(STATUS), r8(HOST_CONTROL), r16(HOST_CONTROL2), r16(CLOCK), submitted, serviced,
          head != nullptr);
}

// ---- Bus speed: take the inherited card to UHS-I SDR104 ----
//
// A card changes its signalling voltage only during initialization (CMD11 after ACMD41 with
// S18R), so the card goes back to idle with CMD0 and through SD initialization again, in the
// order hekate's bdk uses on this SoC (bdk/storage/sdmmc.c sdmmc_storage_init_sd :1994,
// _sd_storage_get_op_cond :1090, _sd_storage_set_uhs_bus_speed :1523; sdmmc_driver.c). hekate
// powers the card off before chainloading L4T (bootloader/l4t/l4t.c:1028 sd_end ->
// sdmmc1_disable_power), so U-Boot's 3.3 V init was the first since power-on and the card can
// still accept 1.8 V. Nothing here writes the card's data area: CMD6 (switch), ACMD6 (bus
// width) and ACMD42 (DAT3 pull-up) change volatile card state only. Commands are polled with
// bounded waits: this runs once in setup(), before the worker exists (and for a re-tune); the
// data path never polls.

// One card command; flags is the COMMAND low byte (RSP_*). data > 0 reads one block of that many
// bytes into the bounce buffer by SDMA. 0 or a nonzero code; *response gets RESPONSE[31:0] and
// initStatus the last STATUS sample.
int NativeIOSwitchBlock::cardCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data)
{
    if (!waitIdle(false, 100000000)) {
        initStatus = r32(PRESENT);
        return 1;
    }
    w32(STATUS, 0xffffffff);
    if (data) {
        for (uintptr_t at = (uintptr_t)bounce, end = at + data; at < end; at += 64)
            __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
        barrier();
        w32(DMA_LOW, static_cast<uint32_t>(bouncePA));
        w32(DMA_HIGH, static_cast<uint32_t>(bouncePA >> 32));
        w16(BLOCK_SIZE, static_cast<uint16_t>(data | 0x7000));
        w16(BLOCK_COUNT, 1);
    }
    w16(TRANSFER, data ? 0x11 : 0);  // DMA | read, one block; nothing for a command without data
    w32(ARGUMENT, arg);
    barrier();
    w16(COMMAND, static_cast<uint16_t>(index << 8 | flags | (data ? CMD_DATA : 0)));
    const uint32_t done = INT_COMMAND | (data ? INT_TRANSFER : 0);
    const uint64_t start = nowNs(), limit = data ? 1000000000ull : 100000000ull;
    uint32_t s = 0;
    do {
        s = r32(STATUS);
    } while (!(s & INT_ERRORS) && (s & done) != done && nowNs() - start < limit);
    initStatus = s;
    if ((s & INT_ERRORS) || (s & done) != done) {
        recover(false);
        return (s & INT_ERRORS) ? 2 : 3;
    }
    if (response) *response = r32(RESPONSE);
    w32(STATUS, 0xffffffff);
    if ((flags & 3) == 3 && !waitIdle(true, 1000000000)) return 4;  // R1b: card busy on DAT0
    if (data) {
        for (uintptr_t at = (uintptr_t)bounce, end = at + data; at < end; at += 64)
            __asm__ volatile("dc ivac, %0" ::"r"(at) : "memory");
        barrier();
    }
    return 0;
}

// CMD55 (with the current RCA, 0 before CMD3) then the application command.
int NativeIOSwitchBlock::appCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response)
{
    uint32_t r55 = 0;
    if (int error = cardCommand(55, rca << 16, RSP_R1, &r55)) return error;
    if (!(r55 & R1_APP_CMD) || (r55 & R1_ERRORS)) return 5;
    return cardCommand(index, arg, flags, response);
}

// CMD6, check (set=false) or switch, of `value` in function group `group` (0-based: 0 access
// mode, 3 power limit), the other groups left as they are (0xF). The 64-byte status lands at
// bounce[0..63]: bytes 0-1 maximum current (mA), 6-7 group 4 and 12-13 group 1 support bits,
// byte 15 high nibble group 4 and byte 16 low nibble group 1 result (hekate sdmmc.c:1305/1497).
int NativeIOSwitchBlock::switchFunction(bool set, unsigned group, uint32_t value)
{
    uint32_t arg = (set ? 0x80000000u : 0u) | 0x00ffffffu, r = 0;
    arg = (arg & ~(0xfu << (group * 4))) | value << (group * 4);
    int error = cardCommand(6, arg, RSP_R1, &r, 64);
    return error ? error : (r & R1_ERRORS) ? 6 : 0;
}

// hekate sdmmc_setup_clock (sdmmc_driver.c:307): card clock gated, default tap (TAP_HW_UPDATED
// off, :120/:133), CMD/DAT reset, timing bits (uhs < 0: 3.3 V legacy timing, HISPD as asked;
// else that UHS mode with 1.8 V signalling), CAR source and its U7.1 divider, always as a pair (source
// < 0: both unchanged; enable cleared around the change, the SET/CLR registers per hekate
// clock.c:72-79), the SDCLK divider (base / 2n, 0 passes base through), then the card clock back on
// if it was on. Only kSpeeds names CAR settings: PLLP with divider 0 would be 408 MHz.
void NativeIOSwitchBlock::setTiming(int uhs, bool highSpeed, int source, uint32_t carDivider, uint32_t divider)
{
    const uint16_t clock = r16(CLOCK);
    w16(CLOCK, clock & ~4);
    w32(VENDOR_TUNING0, r32(VENDOR_TUNING0) & ~(1u << 17));
    w32(VENDOR_CLOCK, (r32(VENDOR_CLOCK) & ~0xff0000u) | kTapB01 << 16);
    resetLines();
    w8(HOST_CONTROL, static_cast<uint8_t>(highSpeed ? r8(HOST_CONTROL) | 4 : r8(HOST_CONTROL) & ~4));
    if (uhs < 0)
        w16(HOST_CONTROL2, r16(HOST_CONTROL2) & ~(7 | CONTROL2_1V8));
    else
        w16(HOST_CONTROL2,
            static_cast<uint16_t>((r16(HOST_CONTROL2) & ~7) | static_cast<uint32_t>(uhs) | CONTROL2_1V8));
    (void)r16(CLOCK);
    if (source >= 0) {
        wr(car, CAR_CLK_ENB_L_CLR, CAR_SDMMC1);
        wr(car, CAR_CLK_SOURCE_SDMMC1, static_cast<uint32_t>(source) << 29 | (carDivider & 0xff));
        wr(car, CAR_CLK_ENB_L_SET, CAR_SDMMC1);
        (void)rd(car, CAR_RST_DEVICES_L);
    }
    const uint16_t next = static_cast<uint16_t>((clock & ~0xffc4u) | (divider & 0xff) << 8 | (divider >> 8 & 3) << 6);
    w16(CLOCK, next);
    for (uint64_t start = nowNs(); !(r16(CLOCK) & 2) && nowNs() - start < 10000000;) {}
    if (clock & 4) w16(CLOCK, next | 4);
}

// hekate _sdmmc_autocal_config_offset + _sdmmc_autocal_execute (sdmmc_driver.c:845/178) for SDMMC1
// at 1.8 V on T210B01: pull-up/down offsets 6/6 (the L4T prod value for every UHS mode,
// tegra210b01-nx-prods.dtsi), pad comparator powered for the run, and hekate's fixed B01 drive
// codes (PU 8, PD 8, :151-153) if calibration does not finish in 10 ms. B01 needs no periodic
// re-calibration (hekate sets periodic_calibration only on T210, :1362-1365).
bool NativeIOSwitchBlock::calibratePads()
{
    const uint16_t clock = r16(CLOCK);
    w16(CLOCK, clock & ~4);
    w32(AUTOCAL_CONFIG, (r32(AUTOCAL_CONFIG) & 0xffff8080u) | 6u << 8 | 6u);
    if (!(r32(PAD_CONTROL) & 0x80000000u)) {
        w32(PAD_CONTROL, r32(PAD_CONTROL) | 0x80000000u);
        (void)r16(CLOCK);
        IODelay(1);
    }
    w32(AUTOCAL_CONFIG, r32(AUTOCAL_CONFIG) | 1u << 29 | 1u << 31);
    (void)r16(CLOCK);
    IODelay(2);
    bool done = false;
    for (uint64_t start = nowNs(); !done && nowNs() - start < 10000000;) done = !(r32(AUTOCAL_STATUS) & 0x80000000u);
    if (!done) {
        w32(AUTOCAL_CONFIG, r32(AUTOCAL_CONFIG) & ~(1u << 29));
        wr(apb, APB_SDMMC1_PAD_CONFIG, (rd(apb, APB_SDMMC1_PAD_CONFIG) & 0xf8080fffu) | 0x808u << 12);
        (void)rd(apb, APB_SDMMC1_PAD_CONFIG);
    }
    w32(PAD_CONTROL, r32(PAD_CONTROL) & ~0x80000000u);
    if (clock & 4) w16(CLOCK, r16(CLOCK) | 4);
    return done;
}

// After CMD11: hekate sdmmc_setup_clock(SDR12) + sdmmc_enable_low_voltage (sdmmc_driver.c:1511):
// LDO2 to 1.8 V and its ramp, PMC told the SDMMC1 pads run at 1.8 V, Schmitt triggers on the six
// pads, pad calibration, 1.8 V in the SDHCI power register, 5 ms, then SDCLK and DAT[3:0] must
// read high within 1 ms. Unlike hekate, SDCLK stays stopped from the CMD11 response until after
// the 5 ms, as SD spec 4.2.4.2 and Linux (mmc_set_uhs_voltage, on this same controller) do.
bool NativeIOSwitchBlock::switchTo1V8(uint8_t ldo2)
{
    w16(CLOCK, r16(CLOCK) & ~4);
    setTiming(UHS_SDR12, false, -1, 0, 1);  // 48 MHz CAR / 2: 24 MHz SDR12 once the clock runs
    const uint8_t value = static_cast<uint8_t>((ldo2 & ~kLdoVoltage) | kLdo1V8);
    int error = TEGRA_I2C_EIO;
    // hekate _max7762x_set_reg retries a PMIC write 100 times 50 us apart (max7762x.c).
    for (unsigned attempt = 0; attempt < 100 && error; ++attempt) {
        if ((error = tegra_i2c_write(kPmicBus, kPmicAddress, kPmicLdo2, &value, 1))) IODelay(50);
    }
    uint8_t check = 0;
    if (error || tegra_i2c_read(kPmicBus, kPmicAddress, kPmicLdo2, &check, 1) || (check & kLdoVoltage) != kLdo1V8) {
        panic("IOSwitchSD: LDO2 1.8 V write failed (i2c %d, LDO2_CFG 0x%02x) with the card waiting in CMD11; "
              "power off, then boot without ioswitch-sd-uhs=1",
              error, check);
    }
    IODelay(1000 + 150);  // regulator ramp (hekate set_voltage) + settle (enable_low_voltage)
    wr(pmc, PMC_PWR_DET_VAL, rd(pmc, PMC_PWR_DET_VAL) & ~PMC_SDMMC1_3V3);
    (void)rd(pmc, PMC_PWR_DET_VAL);
    for (uint32_t pad = 0; pad < 6; ++pad)
        wr(apb, PINMUX_SDMMC1 + pad * 4, rd(apb, PINMUX_SDMMC1 + pad * 4) | PINMUX_SCHMITT);
    if (!calibratePads()) IOLog("IOSwitchSD: SDMMC1 pad calibration timed out; fixed B01 drive codes used\n");
    w8(POWER, 0x0a);  // hekate _sdmmc_set_io_power(1.8 V): voltage select, then bus power on
    w8(POWER, 0x0b);
    (void)r16(CLOCK);
    IOSleep(5);
    w16(CLOCK, r16(CLOCK) | 4);
    (void)r16(CLOCK);
    IODelay(1000);
    return (r32(PRESENT) & PRESENT_DAT) == PRESENT_DAT;
}

// hekate _clock_enable_pllc4 (clock.c:586). PLLC4 only feeds SDMMC on this platform; one already
// running with other dividers belongs to someone else and is left alone (no SDR104 then).
bool NativeIOSwitchBlock::enablePllc4()
{
    const uint32_t base = rd(car, CAR_PLLC4_BASE);
    if (base & PLL_ENABLE) return (base & PLLC4_DIVIDERS) == PLLC4_CONFIG && (base & PLL_LOCK);
    wr(car, CAR_PLLC4_MISC, PLLC4_LOCK_DETECT);
    wr(car, CAR_PLLC4_BASE, rd(car, CAR_PLLC4_BASE) & ~PLLC4_IDDQ);
    IODelay(10);
    wr(car, CAR_PLLC4_BASE, PLLC4_CONFIG);
    wr(car, CAR_PLLC4_BASE, PLLC4_CONFIG | PLL_ENABLE);
    for (unsigned us = 0; us < 1000 && !(rd(car, CAR_PLLC4_BASE) & PLL_LOCK); ++us) IODelay(1);
    IODelay(2);
    if (rd(car, CAR_PLLC4_BASE) & PLL_LOCK) return true;
    wr(car, CAR_PLLC4_BASE, (rd(car, CAR_PLLC4_BASE) & ~PLL_ENABLE) | PLLC4_IDDQ);  // back as it was
    return false;
}

// hekate _sdmmc_tuning_execute_once (sdmmc_driver.c:604) verbatim, including the CMD/DAT reset
// right after issuing CMD19 with the card clock gated: one 64-byte tuning block (4-bit bus), the
// controller compares it itself and moves to the next tap; buffer-read-ready says the step ended.
void NativeIOSwitchBlock::tuneStep()
{
    if (!waitIdle(false, 2000000000)) {
        resetLines();
        return;
    }
    w16(BLOCK_SIZE, 64);
    w16(BLOCK_COUNT, 1);
    w16(TRANSFER, 0x10);  // read, no DMA
    w32(STATUS_ENABLE, r32(STATUS_ENABLE) | INT_READ_READY);
    w16(STATUS, r16(STATUS));
    w16(CLOCK, r16(CLOCK) & ~4);
    w32(ARGUMENT, 0);
    w16(COMMAND, 19 << 8 | RSP_R1 | CMD_DATA);
    (void)r16(CLOCK);
    IODelay(1);
    resetLines();
    w16(CLOCK, r16(CLOCK) | 4);
    (void)r16(CLOCK);
    for (uint64_t start = nowNs(); nowNs() - start < 5000000;) {
        if (r16(STATUS) & INT_READ_READY) {
            w16(STATUS, INT_READ_READY);
            w32(STATUS_ENABLE, r32(STATUS_ENABLE) & ~INT_READ_READY);
            (void)r16(CLOCK);
            IODelay(1);  // 8 card clocks
            return;
        }
    }
    resetLines();
    w32(STATUS_ENABLE, r32(STATUS_ENABLE) & ~INT_READ_READY);
    (void)r16(CLOCK);
    IODelay(1);
}

// hekate sdmmc_tuning_execute (sdmmc_driver.c:760) for SDR104: step size 1, 128 tries, 1x
// multiplier, hardware tap update; the controller walks the taps one CMD19 at a time, clears
// EXEC_TUNING when it is done and sets TUNED_CLK when it found a window (stricter than hekate,
// which looks at TUNED_CLK only: a controller still in tuning mode is a failure too).
bool NativeIOSwitchBlock::tune()
{
    w32(VENDOR_TUNING1, 0);
    w32(VENDOR_TUNING0, (r32(VENDOR_TUNING0) & 0xffff1fffu) | 2u << 13);
    w32(VENDOR_TUNING0, (r32(VENDOR_TUNING0) & 0xffffe03fu) | 1u << 6);
    w32(VENDOR_TUNING0, r32(VENDOR_TUNING0) | 1u << 17);
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) | CONTROL2_EXEC_TUNING);
    for (unsigned i = 0; i < 128 && (r16(HOST_CONTROL2) & CONTROL2_EXEC_TUNING); ++i) tuneStep();
    w32(STATUS, 0xffffffff);
    if ((r16(HOST_CONTROL2) & (CONTROL2_EXEC_TUNING | CONTROL2_TUNED)) == CONTROL2_TUNED) return true;
    // No window: leave tuning mode and fixed sampling, as Linux sdhci_reset_tuning does.
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) & ~(CONTROL2_EXEC_TUNING | CONTROL2_TUNED));
    resetLines();
    return false;
}

// Runs the card at kSpeeds[from], or the first slower step whose tuning finds a window; false
// when none does. The card stays in SDR104 throughout; only the clock and sampling point move.
bool NativeIOSwitchBlock::clockAndTune(unsigned from)
{
    for (unsigned i = from; i < kSpeedCount; ++i) {
        setTiming(UHS_SDR104, false, static_cast<int>(kSpeeds[i].source), kSpeeds[i].carDivider, kSpeeds[i].divider);
        if (tune()) {
            speed = i;
            tuned = true;
            busMode = "SDR104";
            busKHz = kSpeeds[i].kHz;
            return true;
        }
        IOLog("IOSwitchSD: no SDR104 sampling window at %u kHz\n", kSpeeds[i].kHz);
    }
    return false;
}

// After a failed SDR104 transfer (the card is back in transfer state after recover()): tune again
// at this clock, or with `slower` (the retry after a re-tune failed too) go one clock step down;
// either way, steps down while tuning finds no window. Tuning moves no data through the bounce
// buffer (CMD19 blocks stay in the controller), so a write waiting there is untouched. Panics only
// when not even 49.92 MHz tunes: then no transfer can succeed and every I/O would fail anyway.
void NativeIOSwitchBlock::retune(bool slower)
{
    stats.retunes++;
    if (slower ? !clockAndTune(speed + 1) : !tune() && !clockAndTune(speed + 1)) {
        panic("IOSwitchSD: SDR104 tuning finds no sampling window down to %u kHz (hc2 0x%04x, %llu errors); "
              "boot without ioswitch-sd-uhs=1",
              kSpeeds[kSpeedCount - 1].kHz, r16(HOST_CONTROL2), stats.errors);
    }
    IOLog("IOSwitchSD: SDR104 re-tuned at %u kHz, tap %u\n", busKHz, (r32(VENDOR_CLOCK) >> 16) & 0xff);
}

// Returns true when the card was re-initialized (re-verify reads), false when the inherited mode
// was kept without touching the card. Every precondition is checked before CMD0; after CMD0 the
// inherited state is gone and no I/O is possible without finishing, so any failure panics naming
// the step (power off clears the card; without ioswitch-sd-uhs=1 none of this runs).
bool NativeIOSwitchBlock::raiseBusSpeed()
{
    if (!car || !apb || !pmc) {
        IOLog("IOSwitchSD: SDR104 skipped (CAR/APB/PMC not mapped); inherited bus kept\n");
        return false;
    }
    const uint32_t source = rd(car, CAR_CLK_SOURCE_SDMMC1);
    const uint16_t clock = r16(CLOCK);
    const uint32_t divider = (clock >> 8 & 0xff) | (clock >> 6 & 3) << 8;
    const uint32_t baseKHz = source >> 29 == SOURCE_PLLP ? PLLP_KHZ * 2 / ((source & 0xff) + 2) : 0;
    busKHz = divider ? baseKHz / (2 * divider) : baseKHz;
    busMode = "inherited";
    // Opt-in (boot-arg): without it nothing below runs (no CMD0, no PLLC4, no PMIC access).
    unsigned enabled = 0;
    PE_parse_boot_argn("ioswitch-sd-uhs", &enabled, sizeof(enabled));
    const char *skip = nullptr;
    uint8_t ldo2 = 0;
    if (!enabled)
        skip = "opt-in: boot-arg ioswitch-sd-uhs=1";
    else if (((rd(apb, APB_GP_HIDREV) >> 4) & 0xf) < 2)
        skip = "not T210B01 (the pad/tap/trim values are B01's)";
    else if (!(r32(CAPABILITIES_HI) & 2))
        skip = "controller reports no SDR104";
    else if (!baseKHz || baseKHz > 50000 || baseKHz < 25000)
        skip = "SDMMC1 CAR clock not PLLP 25-50 MHz";
    else if (r16(HOST_CONTROL2) & CONTROL2_1V8)
        skip = "bus already at 1.8 V";
    else if (tegra_i2c_init(kPmicBus) || tegra_i2c_read(kPmicBus, kPmicAddress, kPmicLdo2, &ldo2, 1))
        skip = "PMIC (I2C5 0x3c) not reachable";
    else if ((ldo2 & kLdoVoltage) != kLdo3V3)
        skip = "LDO2 not at 3.3 V";
    else if (!enablePllc4())
        skip = "PLLC4 busy with other dividers or no lock";
    if (skip) {
        IOLog("IOSwitchSD: SDR104 skipped (%s; LDO2_CFG 0x%02x PLLC4 0x%08x); bus stays at %u kHz\n", skip, ldo2,
              rd(car, CAR_PLLC4_BASE), busKHz);
        return false;
    }
    return reinitialize(baseKHz, ldo2, true, false);
}

// SD initialization from the idle state into the fastest mode the card and host share: at boot from U-Boot's card
// (raiseBusSpeed), and after system sleep from a freshly powered card (resume, `uhs` = it ran at 1.8 V before).
bool NativeIOSwitchBlock::reinitialize(uint32_t baseKHz, uint8_t ldo2, bool uhs, bool resume)
{
    const uint64_t start = nowNs();
    auto fail = [&](const char *step, int error) {
        if (resume) {
            panic("IOSwitchSD: SD re-init after sleep failed at %s (error %d, STATUS 0x%08x PRESENT 0x%08x hc2 0x%04x)",
                  step, error, initStatus, r32(PRESENT), r16(HOST_CONTROL2));
        }
        panic("IOSwitchSD: SD re-init failed at %s (error %d, STATUS 0x%08x PRESENT 0x%08x hc2 0x%04x); "
              "power off, then boot without ioswitch-sd-uhs=1",
              step, error, initStatus, r32(PRESENT), r16(HOST_CONTROL2));
    };
    rca = 0;
    tuned = false;
    speed = 0;

    // Host defaults hekate's sdmmc_init sets and U-Boot does not (sdmmc_driver.c:1390-1391, 1257):
    // command output enable one cycle late, the band-gap regulator supplying the DLL, and the
    // SDMMC1 CLK pad's deep loopback that the read sampling path relies on.
    w32(IO_SPARE, r32(IO_SPARE) | 1u << 19);
    w32(VENDOR_IO_TRIM, r32(VENDOR_IO_TRIM) & ~4u);
    wr(apb, APB_SDMMC1_CLK_LOOPBACK, 1);
    // Identification: 1-bit, legacy timing, <= 400 kHz, 1 ms + 74 clocks before the first command.
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~0x26);
    setTiming(-1, false, -1, 0, (baseKHz + 799) / 800);
    IODelay(1000 + 200);
    uint32_t r = 0;
    int error;
    if ((error = cardCommand(0, 0, RSP_NONE, nullptr))) fail("CMD0", error);
    if ((error = cardCommand(8, 0x1aa, RSP_R1, &r)) || (r & 0xfff) != 0x1aa)
        fail("CMD8", error ? error : int(r & 0xfff));
    // ACMD41 until the card leaves busy: block addressing, max performance, ask for 1.8 V.
    for (uint64_t ready = nowNs();;) {
        if ((error = appCommand(41, OCR_CCS | OCR_XPC | (uhs ? OCR_S18 : 0) | OCR_VDD_32_33, RSP_R3, &r)))
            fail("ACMD41", error);
        if (r & OCR_READY) break;
        if (nowNs() - ready > 1500000000ull) fail("ACMD41 busy", int(r >> 24));
        IOSleep(10);
    }
    if (!(r & OCR_CCS)) fail("ACMD41 byte-addressed card", int(r >> 24));
    const bool lowVoltage = uhs && (r & OCR_S18);
    if (lowVoltage) {
        uint32_t r11 = 0;
        if ((error = cardCommand(11, 0, RSP_R1, &r11)) || (r11 & R1_ERRORS)) fail("CMD11", error ? error : 6);
        if (!switchTo1V8(ldo2)) fail("1.8 V switch (DAT[3:0] not released)", int((r32(PRESENT) >> 20) & 0xf));
    }
    if ((error = cardCommand(2, 0, RSP_R2, nullptr))) fail("CMD2", error);
    for (uint64_t ready = nowNs(); !rca;) {
        if ((error = cardCommand(3, 0, RSP_R1, &r))) fail("CMD3", error);
        if ((rca = r >> 16)) break;
        if (nowNs() - ready > 1500000000ull) fail("CMD3 RCA 0", 0);
        IOSleep(1);  // hekate _sd_storage_get_rca: ask again 1 ms later
    }
    // Out of identification: default speed (24 MHz) at 3.3 V as hekate (sdmmc.c:2054); the 1.8 V path
    // already runs SDR12 at that clock since switchTo1V8.
    if (!lowVoltage) setTiming(-1, false, -1, 0, 1);
    if ((error = cardCommand(7, rca << 16, RSP_R1B, &r)) || (r & R1_ERRORS)) fail("CMD7", error ? error : 6);
    if ((error = appCommand(42, 0, RSP_R1, &r)) || (r & R1_ERRORS)) fail("ACMD42", error ? error : 6);
    if ((error = appCommand(6, 2, RSP_R1, &r)) || (r & R1_ERRORS)) fail("ACMD6", error ? error : 6);
    w8(HOST_CONTROL, r8(HOST_CONTROL) | 2);
    if ((error = switchFunction(false, 0, 0xf))) fail("CMD6 query", error);
    const uint32_t access = bounce[13] | bounce[12] << 8, power = bounce[7] | bounce[6] << 8;
    const char *limit = "";
    if (lowVoltage && (access & 1u << UHS_SDR104)) {
        // hekate: 1.44 W power limit when offered (its result is not checked there either: the card
        // keeps 0.72 W), then check and switch to SDR104 if the card draws at most 800 mA there (the
        // board's max-current-800ma), else SDR25.
        limit = switchFunction(true, 3, power & 2 ? 1 : 0) ? ", power limit unchanged"
                : ((bounce[15] >> 4) & 0xf) == 1           ? ", 1.44 W"
                                                           : ", 0.72 W";
        if ((error = switchFunction(false, 0, UHS_SDR104))) fail("CMD6 check SDR104", error);
        if ((bounce[16] & 0xf) == UHS_SDR104 && (bounce[0] << 8 | bounce[1]) <= 800) {
            if ((error = switchFunction(true, 0, UHS_SDR104)) || (bounce[16] & 0xf) != UHS_SDR104)
                fail("CMD6 SDR104", error);
            // 199.68 MHz, or the first slower clock that tunes (the card stays in SDR104).
            if (!clockAndTune(0)) fail("SDR104 tuning (no window down to 49.92 MHz)", r16(HOST_CONTROL2));
        }
    }
    if (!tuned) {
        // No SDR104 (card, or its current at SDR104 over budget): SDR25 at 1.8 V or high speed at
        // 3.3 V, both the inherited 48 MHz (group 1 function 1 is the same CMD6 for either); a card
        // without even that stays at default speed, 24 MHz.
        const bool fast = access & 2;
        if (fast && ((error = switchFunction(true, 0, 1)) || (bounce[16] & 0xf) != 1)) fail("CMD6 high speed", error);
        setTiming(fast && lowVoltage ? int(UHS_SDR25)
                  : lowVoltage       ? int(UHS_SDR12)
                                     : -1,
                  fast && !lowVoltage, -1, 0, fast ? 0 : 1);
        busMode = !fast ? (lowVoltage ? "SDR12" : "DS") : lowVoltage ? "SDR25" : "HS";
        busKHz = fast ? baseKHz : baseKHz / 2;
    }
    if ((error = cardCommand(13, rca << 16, RSP_R1, &r)) || (r & R1_ERRORS) || ((r >> 9) & 0xf) != 4)
        fail("CMD13 transfer state", error ? error : int(r >> 9 & 0xf));
    lowVoltageBus = lowVoltage;
    if (!resume) {
        IOLog("IOSwitchSD: bus now %s %u kHz %s V 4-bit%s, tap %u%s, RCA 0x%04x, %llu ms\n", busMode, busKHz,
              lowVoltage ? "1.8" : "3.3", limit, (r32(VENDOR_CLOCK) >> 16) & 0xff, tuned ? " (tuned)" : "", rca,
              (nowNs() - start) / 1000000);
    }
    return true;
}

// Card and SDMMC1 pads unpowered, as hekate sdmmc1_disable_power (the card must restart from power-off after SC7: the
// controller is reset and the card cannot leave 1.8 V signalling otherwise). Thread context, I2C up.
void NativeIOSwitchBlock::powerOffCard()
{
    w16(CLOCK, r16(CLOCK) & ~4);
    w8(POWER, 0);
    uint8_t ldo2 = 0;
    int error = tegra_i2c_read(kPmicBus, kPmicAddress, kPmicLdo2, &ldo2, 1);
    if (!error) {
        const uint8_t off = static_cast<uint8_t>(ldo2 & kLdoVoltage);  // power mode 7:6 = 0: off
        error = tegra_i2c_write(kPmicBus, kPmicAddress, kPmicLdo2, &off, 1);
    }
    if (error) panic("IOSwitchSD: LDO2 off before sleep failed (I2C5 %d)", error);
    IOSleep(4);
    if (tegra_gpio_output(kCardPowerGpio, 0)) panic("IOSwitchSD: cannot switch the card supply (GPIO PE4) off");
    IOSleep(10);
    wr(pmc, PMC_NO_IOPOWER, rd(pmc, PMC_NO_IOPOWER) | PMC_SDMMC1_NO_POWER);
    wr(pmc, PMC_PWR_DET_VAL, rd(pmc, PMC_PWR_DET_VAL) | PMC_SDMMC1_3V3);
    (void)rd(pmc, PMC_PWR_DET_VAL);
}

// After SC7: SDMMC1 clocked from PLLP at U-Boot's 48 MHz and out of reset, pads and card powered at 3.3 V (hekate
// _sdmmc_config_sdmmc1), the host reset and set up as hekate sdmmc_init and setup() leave it, identification clock on.
void NativeIOSwitchBlock::powerOnController()
{
    wr(car, CAR_RST_DEV_L_SET, CAR_SDMMC1);
    wr(car, CAR_CLK_ENB_L_CLR, CAR_SDMMC1);
    wr(car, CAR_CLK_SOURCE_SDMMC1, SOURCE_PLLP << 29 | kPllpDivider48MHz);
    wr(car, CAR_CLK_ENB_L_SET, CAR_SDMMC1);
    (void)rd(car, CAR_RST_DEVICES_L);
    IODelay(2);
    wr(car, CAR_RST_DEV_L_CLR, CAR_SDMMC1);
    (void)rd(car, CAR_RST_DEVICES_L);
    wr(pmc, PMC_NO_IOPOWER, rd(pmc, PMC_NO_IOPOWER) & ~PMC_SDMMC1_NO_POWER);
    (void)rd(pmc, PMC_NO_IOPOWER);
    if (tegra_gpio_output(kCardPowerGpio, 1)) panic("IOSwitchSD: cannot switch the card supply (GPIO PE4) on");
    IOSleep(10);
    wr(pmc, PMC_PWR_DET_VAL, rd(pmc, PMC_PWR_DET_VAL) | PMC_SDMMC1_3V3);
    const uint8_t on = static_cast<uint8_t>(0xc0 | kLdo3V3);  // power mode normal, 3.3 V
    int error = tegra_i2c_write(kPmicBus, kPmicAddress, kPmicLdo2, &on, 1);
    if (error) panic("IOSwitchSD: LDO2 3.3 V after sleep failed (I2C5 %d)", error);
    IOSleep(1);
    w8(RESET, 1);
    for (uint64_t begin = nowNs(); r8(RESET) & 1;) {
        if (nowNs() - begin > 100000000ull) panic("IOSwitchSD: SDMMC1 reset did not complete after sleep");
    }
    w32(IO_SPARE, r32(IO_SPARE) | 1u << 19);
    w32(VENDOR_IO_TRIM, r32(VENDOR_IO_TRIM) & ~4u);
    w32(VENDOR_CLOCK, (r32(VENDOR_CLOCK) & 0xe0fffffbu) | kTrimB01 << 24);
    w16(CLOCK, 1 | 0x80 << 8);  // internal clock on, slowest divider until setTiming
    for (uint64_t begin = nowNs(); !(r16(CLOCK) & 2);) {
        if (nowNs() - begin > 20000000ull) panic("IOSwitchSD: SDMMC1 internal clock not stable after sleep");
    }
    w8(POWER, 0x0e);  // 3.3 V, then bus power on (hekate _sdmmc_set_io_power)
    w8(POWER, 0x0f);
    w16(CLOCK, r16(CLOCK) | 4);
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~0x18);
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) | 0x3000);
    w8(TIMEOUT, 0x0e);
    w32(SIGNAL_ENABLE, 0);
    w32(STATUS_ENABLE, 0x03ff800bu);
    w32(STATUS, 0xffffffff);
    barrier();
}

// System sleep (SwitchSuspend's sleep and wake actions, thread context, I2C up). Requests keep queueing while
// suspended; the worker finishes the command it is on and waits. LBA 1 must read back as at boot after resume.
void NativeIOSwitchBlock::suspend()
{
    IOLockLock(queueLock);
    suspended = true;
    while (working) IOLockSleep(queueLock, &working, THREAD_UNINT);
    IOLockUnlock(queueLock);
    if (irqSource) irqSource->disable();
    powerOffCard();
}

void NativeIOSwitchBlock::resume()
{
    powerOnController();
    if (irqSource) irqSource->enable();
    const bool uhs = lowVoltageBus && enablePllc4();
    if (lowVoltageBus && !uhs)
        panic("IOSwitchSD: PLLC4 does not lock after sleep (PLLC4_BASE 0x%08x)", rd(car, CAR_PLLC4_BASE));
    (void)reinitialize(PLLP_KHZ * 2 / (kPllpDivider48MHz + 2), 0xc0 | kLdo3V3, uhs, true);
    uint8_t again[kSector];
    const int step = transfer(1, 1, false, again);
    if (step || memcmp(again, gptHeader, sizeof(again))) {
        panic("IOSwitchSD: LBA 1 differs after sleep (read step %d, bus %s): not the card this boot started from", step,
              busMode);
    }
    IOLockLock(queueLock);
    suspended = false;
    IOLockWakeup(queueLock, &head, false);
    IOLockUnlock(queueLock);
}

bool NativeIOSwitchBlock::setup()
{
    regs = reinterpret_cast<volatile uint8_t *>(ml_io_map(kControllerPA, 0x1000));
    auto *mc = reinterpret_cast<volatile uint32_t *>(ml_io_map(kMemoryControllerPA, 0x1000));
    // CAR, APB_MISC/pinmux and PMC serve the SDR104 bring-up only; without them it is skipped.
    car = reinterpret_cast<volatile uint8_t *>(ml_io_map(kCarPA, 0x1000));
    apb = reinterpret_cast<volatile uint8_t *>(ml_io_map(kApbMiscPA, 0x4000));
    pmc = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPmcPagePA, 0x1000));
    queueLock = IOLockAlloc();
    if (!regs || !mc || !queueLock) {
        IOLog("IOSwitchSD: map/alloc failed\n");
        return false;
    }

    if ((r32(PRESENT) & PRESENT_CARD) != PRESENT_CARD || !(r16(CLOCK) & 4)) {
        IOLog("IOSwitchSD: no card or SD clock off\n");
        return false;
    }
    // SDMMC1 group translation must be off for physical DMA (MC_SMMU_SDMMC1A_ASID).
    if (mc[0xa94 / 4] & 0x80000000u) {
        IOLog("IOSwitchSD: SMMU translation enabled for SDMMC1\n");
        return false;
    }

    bounceMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        kBounceBytes, 0x00000000ffffffffull & ~static_cast<uint64_t>(kBounceAlign - 1));
    if (!bounceMemory) {
        IOLog("IOSwitchSD: bounce allocation failed\n");
        return false;
    }
    bounce = static_cast<uint8_t *>(bounceMemory->getBytesNoCopy());
    bouncePA = bounceMemory->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    if (!bounce || !bouncePA || (bouncePA & (kBounceAlign - 1)) || bouncePA + kBounceBytes > (1ull << 32)) {
        IOLog("IOSwitchSD: bad bounce PA 0x%llx\n", bouncePA);
        return false;
    }

    // SDHCI v4 64-bit SDMA (Tegra210 HOST v4). Interrupt signals stay off except
    // while the worker waits for one (waitSignal).
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~0x18);
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) | 0x3000);
    w8(TIMEOUT, 0x0e);
    w32(SIGNAL_ENABLE, 0);
    w32(STATUS_ENABLE, 0x03ff800bu);
    w32(STATUS, 0xffffffff);
    barrier();
    irqMode = startInterrupt();
    unsigned seconds = 10;
    PE_parse_boot_argn("ioswitch-sd-stats", &seconds, sizeof(seconds));
    statsEveryNs = seconds * 1000000000ull;
    unsigned merging = 1;
    PE_parse_boot_argn("ioswitch-sd-merge", &merging, sizeof(merging));
    merge = merging != 0;

    // GPT: header at LBA 1, then entries. Exactly one APFS partition. The header is read in the
    // inherited bus mode first; after a re-initialization it must read back byte for byte, which
    // checks the new mode end to end (sampling, tuning) beyond the bus CRC.
    uint8_t header[kSector];
    if (transfer(1, 1, false, header)) {
        IOLog("IOSwitchSD: GPT header read failed\n");
        return false;
    }
    memcpy(gptHeader, header, sizeof(gptHeader));
    if (raiseBusSpeed()) {
        uint8_t again[kSector];
        int step = transfer(1, 1, false, again);
        if (step || memcmp(header, again, sizeof(header)))
            panic("IOSwitchSD: LBA 1 differs after switching to %s (read step %d); boot without ioswitch-sd-uhs=1",
                  busMode, step);
    }
    uint64_t entriesLBA = le64(header + 72);
    uint32_t entryCount = le32(header + 80), entrySize = le32(header + 84);
    if (memcmp(header, "EFI PART", 8) || entrySize < 128 || entrySize > 512 || (entrySize & 7) || !entryCount ||
        entryCount * entrySize > 64 * 1024 || entriesLBA < 2) {
        IOLog("IOSwitchSD: invalid GPT header\n");
        return false;
    }
    uint32_t entrySectors = (entryCount * entrySize + kSector - 1) / kSector;
    static uint8_t entries[64 * 1024];
    for (uint32_t done = 0; done < entrySectors;) {
        uint32_t n = entrySectors - done > 64 ? 64 : entrySectors - done;
        if (transfer(entriesLBA + done, n, false, entries + done * kSector)) {
            IOLog("IOSwitchSD: GPT entry read failed\n");
            return false;
        }
        done += n;
    }
    unsigned found = 0;
    for (uint32_t i = 0; i < entryCount; ++i) {
        const uint8_t *e = entries + i * entrySize;
        if (memcmp(e, kApfsType, 16)) continue;
        uint64_t first = le64(e + 32), last = le64(e + 40);
        if (!first || last < first) continue;
        firstSector = first;
        sectorCount = last - first + 1;
        ++found;
    }
    if (found != 1 || firstSector + sectorCount > UINT32_MAX) {
        IOLog("IOSwitchSD: expected one APFS partition, found %u\n", found);
        return false;
    }

    // Commands over 512 KiB cross the SDMA boundary (command()). Read the partition's first MiB in four 256 KiB commands
    // and in one; only identical data with no error allows the larger commands. The 1 MiB read goes straight to
    // command() so that a failure is not retried and re-tuned (transfer() would step the clock down); a failure keeps
    // 256 KiB, with one line.
    if (sectorCount >= kBounceSectors) {
        uint8_t *reference = static_cast<uint8_t *>(IOMallocData(kBounceBytes));
        int step = reference ? 0 : -2;
        for (uint32_t done = 0; !step && done < kBounceSectors; done += kSafeSectors)
            step = transfer(firstSector + done, kSafeSectors, false, reference + done * kSector);
        if (!step) {
            for (uintptr_t at = (uintptr_t)bounce, end = at + kBounceBytes; at < end; at += 64)
                __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
            barrier();
            uint32_t status = 0;
            step = command(static_cast<uint32_t>(firstSector), kBounceSectors, false, &status);
            if (step) {
                recover(true);
            } else {
                for (uintptr_t at = (uintptr_t)bounce, end = at + kBounceBytes; at < end; at += 64)
                    __asm__ volatile("dc ivac, %0" ::"r"(at) : "memory");
                barrier();
            }
        }
        const bool same = !step && !memcmp(reference, bounce, kBounceBytes);
        if (same)
            maxSectors = kBounceSectors;
        else
            IOLog("IOSwitchSD: 1 MiB commands failed their check (step %d, %s); keeping 256 KiB\n", step,
                  step ? "error" : "data differs");
        if (reference) IOFreeData(reference, kBounceBytes);
    }

    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(&NativeIOSwitchBlock::worker, this, &thread) != KERN_SUCCESS) {
        IOLog("IOSwitchSD: worker start failed\n");
        return false;
    }
    thread_deallocate(thread);
    return true;
}

extern "C" void ioswitch_block_suspend(void)
{
    if (gOwner) gOwner->suspend();
}

extern "C" void ioswitch_block_resume(void)
{
    if (gOwner) gOwner->resume();
}

extern "C" int ioswitch_block_publish(void)
{
    if (gPublishStatus >= 0) return gPublishStatus;
    gPublishStatus = IOSW_BLOCK_NOT_READY;
    IOService *resources = IOService::getResourceService();
    auto *owner = OSTypeAlloc(NativeIOSwitchBlock);
    if (!owner || !owner->init() || !owner->attach(resources) || !owner->start(resources) || !owner->setup()) {
        IOLog("IOSwitchSD: publish failed\n");
        return gPublishStatus;
    }
    auto *device = OSTypeAlloc(NativeIOSwitchBlockDevice);
    if (!device || !device->init(static_cast<OSDictionary *>(nullptr)) ||
        !device->configure(owner, owner->sectorCount) || !device->attach(owner) || !device->start(owner)) {
        IOLog("IOSwitchSD: device start failed\n");
        return gPublishStatus;
    }
    owner->device = device;
    gOwner = owner;
    device->registerService();
    device->release();
    gPublishStatus = IOSW_BLOCK_OK;
    return gPublishStatus;
}

// ---- Volume lookup for the raw root/Preboot/cryptex boot path ----

namespace {
// Returns the unique IOMedia with this UUID if it sits below our block device.
// On success fills device/path; NOT_READY until IOMediaBSDClient has named it.
int findVolume(const char *uuid, dev_t *device, char *path, size_t capacity)
{
    *device = NODEV;
    if (path && capacity) path[0] = 0;
    if (!gOwner || !gOwner->device) return IOSW_BLOCK_NOT_READY;
    OSDictionary *match = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    OSString *wanted = OSString::withCString(uuid);
    if (!match || !properties || !wanted) {
        OSSafeReleaseNULL(match);
        OSSafeReleaseNULL(properties);
        OSSafeReleaseNULL(wanted);
        return IOSW_BLOCK_NOMEM;
    }
    properties->setObject("UUID", wanted);
    match->setObject(gIOPropertyMatchKey, properties);
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    properties->release();
    wanted->release();
    if (!it) return IOSW_BLOCK_NOT_READY;
    int status = IOSW_BLOCK_NOT_READY;
    while (auto *media = OSDynamicCast(IOService, it->getNextObject())) {
        bool ours = false;
        IORegistryEntry *entry = media->copyParentEntry(gIOServicePlane);
        for (unsigned depth = 0; entry && depth < 32 && !ours; ++depth) {
            ours = entry == gOwner->device;
            IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
            entry->release();
            entry = parent;
        }
        OSSafeReleaseNULL(entry);
        if (!ours) continue;
        auto *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
        auto *majorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
        auto *minorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
        if (!name || !majorNumber || !minorNumber) continue;  // BSD client not attached yet
        *device = makedev(majorNumber->unsigned32BitValue(), minorNumber->unsigned32BitValue());
        if (path && capacity) snprintf(path, capacity, "/dev/%s", name->getCStringNoCopy());
        status = IOSW_BLOCK_OK;
        break;
    }
    it->release();
    return status;
}

// Whether a BSD device is media below our block device with the given UUID (or any).
bool ownsDevice(dev_t device, const char *uuid)
{
    if (device == NODEV || !gOwner || !gOwner->device) return false;
    OSDictionary *match = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(2);
    OSNumber *majorNumber = OSNumber::withNumber(major(device), 32);
    OSNumber *minorNumber = OSNumber::withNumber(minor(device), 32);
    bool ok = match && properties && majorNumber && minorNumber;
    if (ok) {
        properties->setObject(kIOBSDMajorKey, majorNumber);
        properties->setObject(kIOBSDMinorKey, minorNumber);
        match->setObject(gIOPropertyMatchKey, properties);
    }
    OSIterator *it = ok ? IOService::getMatchingServices(match) : nullptr;
    OSSafeReleaseNULL(match);
    OSSafeReleaseNULL(properties);
    OSSafeReleaseNULL(majorNumber);
    OSSafeReleaseNULL(minorNumber);
    if (!it) return false;
    bool result = false;
    if (auto *media = OSDynamicCast(IOService, it->getNextObject())) {
        auto *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
        if (identity && (!uuid || !strcmp(identity->getCStringNoCopy(), uuid))) {
            IORegistryEntry *entry = media->copyParentEntry(gIOServicePlane);
            for (unsigned depth = 0; entry && depth < 32 && !result; ++depth) {
                result = entry == gOwner->device;
                IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
                entry->release();
                entry = parent;
            }
            OSSafeReleaseNULL(entry);
        }
    }
    it->release();
    return result;
}
}

extern "C" int ioswitch_raw_volume_status(const char *uuid, dev_t *device, char *path, size_t capacity, uint64_t *bytes)
{
    if (!uuid || !device || !path || !bytes || capacity < 6) return IOSW_BLOCK_INVALID;
    *bytes = 0;
    int status = findVolume(uuid, device, path, capacity);
    if (!status) *bytes = gOwner->sectorCount * kSector;
    return status;
}

extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t capacity)
{
    if (!uuid || !device || !path || capacity < 6) return 0;
    // Only the OS root is reported here; other volumes use the raw query.
    if (strcmp(uuid, kRootUUID)) {
        *device = NODEV;
        path[0] = 0;
        return 0;
    }
    return findVolume(uuid, device, path, capacity) == IOSW_BLOCK_OK;
}

extern "C" int native_verified_block_root_authorized(dev_t device)
{
    return ownsDevice(device, kRootUUID);
}

extern "C" int native_verified_block_os_root_authorized(dev_t actual, dev_t selected)
{
    return actual != NODEV && actual == selected && ownsDevice(actual, kRootUUID);
}

extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
    if (!digest) return 0;
    bzero(digest, 48);
    if (!uuid || strcmp(uuid, kRootUUID) || !ownsDevice(device, kRootUUID)) return 0;
    memcpy(digest, kRootIdentifier, sizeof(kRootIdentifier));
    return 1;
}

extern "C" volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];
extern "C" void ioswitch_print_blocked_kext_threads(const char *(*name_of)(const void *), unsigned max_threads);
extern "C" void ioswitch_iokit_jobs(int *outstanding, int *threads, int *waiting, int *max_threads);

namespace {
// Bundle identifier of the kext containing `address` (kexts are never unloaded here).
const char *kextName(const void *address)
{
    OSKext *kext = OSKext::lookupKextWithAddress(reinterpret_cast<vm_address_t>(address));
    if (!kext) return nullptr;
    const char *name = kext->getIdentifierCString();
    kext->release();
    return name;
}

// The service-plane subtree under the block device: class, name, busy count and
// IOService state bits (2 registered, 4 matched, 8 first published, 0x10 first matched).
void printServiceTree(IORegistryEntry *entry, unsigned depth, unsigned *budget)
{
    if (!*budget) return;
    --*budget;
    auto *service = OSDynamicCast(IOService, entry);
    IOLog("IOSwitchSD: %*s%s %s busy=%u state=0x%x\n", static_cast<int>(depth * 2), "",
          entry->getMetaClass()->getClassName(), entry->getName(), service ? service->getBusyState() : 0,
          service ? static_cast<unsigned>(service->getState()) : 0);
    if (OSIterator *children = entry->getChildIterator(gIOServicePlane)) {
        while (auto *child = OSDynamicCast(IORegistryEntry, children->getNextObject()))
            printServiceTree(child, depth + 1, budget);
        children->release();
    }
}
}

extern "C" void ioswitch_raw_root_wait_diagnostic(void)
{
    IOLog("IOSwitchSD: per-core timer/IPI interrupts %u/%u %u/%u %u/%u %u/%u\n", armcpu_core_timer[0],
          armcpu_core_sgi[0], armcpu_core_timer[1], armcpu_core_sgi[1], armcpu_core_timer[2], armcpu_core_sgi[2],
          armcpu_core_timer[3], armcpu_core_sgi[3]);
    if (!gOwner) {
        IOLog("IOSwitchSD: not published (status %d)\n", gPublishStatus);
        return;
    }
    gOwner->reportState("root wait timed out");
    if (gOwner->device) {
        unsigned budget = 14;
        printServiceTree(gOwner->device, 0, &budget);
    }
    int outstanding = 0, threads = 0, waiting = 0, maxThreads = 0;
    ioswitch_iokit_jobs(&outstanding, &threads, &waiting, &maxThreads);
    IOLog("IOSwitchSD: IOKit matching jobs outstanding=%d config threads=%d waiting=%d max=%d\n", outstanding, threads,
          waiting, maxThreads);
    ioswitch_print_blocked_kext_threads(kextName, 6);
}

// FAT-profile entry points: the FAT image path is gone.
extern "C" int ioswitch_data_volume_status(const char *, dev_t *device, char *path, size_t capacity, uint64_t *bytes)
{
    if (device) *device = NODEV;
    if (path && capacity) path[0] = 0;
    if (bytes) *bytes = 0;
    return IOSW_BLOCK_INVALID;
}
extern "C" int ioswitch_data_volume_path(const char *uuid, dev_t *device, char *path, size_t capacity, uint64_t *bytes)
{
    return ioswitch_data_volume_status(uuid, device, path, capacity, bytes) == IOSW_BLOCK_OK;
}
extern "C" int ioswitch_block_volume_path(unsigned, char *path, unsigned capacity)
{
    if (path && capacity) path[0] = 0;
    return IOSW_BLOCK_NOT_READY;
}
