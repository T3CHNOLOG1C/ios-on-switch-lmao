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
// SoC; that path has not run on hardware yet, hence opt-in. Data moves by SDHCI v4 ADMA2
// straight between the card and the requests' own pages (IOSwitchAdma.h), or by SDMA through
// one physically contiguous bounce buffer for what ADMA2 cannot take (and for everything if
// setup()'s ADMA2 check fails), CMD18/CMD25 with Auto CMD12, with
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
#include <vm/pmap.h>
#include "../ArmGIC/IOSwitchInterrupts.h"
#include "../TegraI2C/TegraI2C.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "block_backend.h"
#include "NativeIOSwitchBlock.h"
#include "IOSwitchClientCopy.h"
#include "IOSwitchAdma.h"
#include "IOSwitchQueue.h"
#include "IOSwitchSdCard.h"

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
// The ADMA2 path's page checks and cache maintenance (osfmk/arm/pmap/pmap.c, arm64/arm_vm_init.c): whether a page is
// managed RAM, its cache attributes (VM_WIMG_DEFAULT 0x2, osfmk/arm/memory_types.h: write-back cacheable), and its
// address in the kernel's physical aperture.
boolean_t pmap_valid_page(ppnum_t pn);
unsigned int pmap_cache_attributes(ppnum_t pn);
vm_offset_t phystokv(uint64_t pa);
extern uint64_t gPhysBase, gPhysSize;  // the RAM phystokv covers (it panics on anything else)
}

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
// r12-sdio: how many queued requests a read chain looks through for reads that continue it (IOSwitchQueue.h): as many as
// one chain may hold, so it never takes fewer than the old rule did from a queue of reads right behind each other.
constexpr unsigned kMergeScan = kMaxChain;
// r10-adma2: with ADMA2, reads go up to kBounceSectors per command (the cap SDMA had once its boundary check passed) and
// writes up to kDirectWriteSectors. The first kWriteChecks ADMA2 writes of every boot are read back and compared
// (proveWrite). r12-sdio: writes up to 1 MiB too. The 256 KiB cap existed for the SDMA boundary resume (0d184ec), which
// ADMA2 never uses; r7p5 verified its 32 ADMA2 writes, and a longer ADMA2 write differs only in its block count and
// descriptor count. Still, the first kLargeWriteChecks writes over 256 KiB of every boot are read back too; one that
// differs is written again through the bounce buffer and writes longer than 256 KiB take the bounce path's 256 KiB SDMA
// commands from then on (writeSectors; the advertised maximum cannot shrink once IOBlockStorageDriver has read it).
constexpr uint32_t kDirectWriteSectors = kBounceSectors;
constexpr uint32_t kWriteChecks = 32, kLargeWriteChecks = 16;
// r14-sdcq: multi-block ADMA2 commands announce their length with Auto CMD23 (SET_BLOCK_COUNT) instead of ending with
// Auto CMD12, once setup()'s check (checkCmd23) has read kCmd23Pairs 4 KiB blocks both ways with equal bytes. The first
// kCmd23WriteChecks such writes of every boot are read back as proveWrite does for ADMA2's; one that differs is written
// again through the bounce buffer (CMD12 framing) and writes go back to CMD12 until reboot.
constexpr uint32_t kCmd23Pairs = 16, kCmd23WriteChecks = 16;
// r14-sdcq: a read chain also takes queued reads that start up to kGapSectors past its end (or end that far before its
// start), and the card reads straight through the gap into a sink nobody reads (IOSwitchQueue.h). On r7r a command cost
// ~0.58 ms plus ~10 us per KiB moved (r12-sdio's model, 4 KiB reads at 0.62 ms): a 32 KiB gap is ~0.32 ms of transfer
// against ~0.58 ms for the separate command it saves, so even the longest gap taken saves ~0.26 ms; kGapBudget keeps one
// command's read-through to 64 KiB (~0.64 ms, against ~1.16 ms for the two commands its two longest gaps save). The first
// kGapProofs such chains of every boot are read again by SDMA and compared before they complete (proveGaps).
constexpr uint32_t kGapSectors = 32 * 1024 / kSector, kGapBudget = 64 * 1024 / kSector, kGapProofs = 8;
// r12-sdio: what a read that proves ADMA2 above 4 GiB fills those pages with first (prepareDirect, proveHigh).
constexpr uint8_t kPoison = 0x5a;
// r12-sdio: the most separate stretches of a read chain staged through the bounce buffer (IOSwitchAdmaStaging; consecutive
// segments of one request make one): one per page of a 1 MiB chain of 4 KiB pages. More: the whole chain bounces.
constexpr uint32_t kStages = 256;
using Staging = IOSwitchAdmaStaging<IOMemoryDescriptor>;
// Descriptor table: 16 KiB, contiguous, below 4 GiB; 1024 descriptors, four times what a 1 MiB chain of scattered 4 KiB
// pages needs (256). A chain that needs more takes the bounce buffer.
constexpr uint32_t kAdmaDescriptors = 1024;
constexpr uint32_t kAdmaTableBytes = kAdmaDescriptors * sizeof(IOSwitchAdmaDescriptor);
// command()'s step beyond the SDMA ones: the ADMA engine stopped on an error (ADMA error interrupt): the host's DMA,
// not the card or the bus.
constexpr int kStepAdmaError = 11;
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
    ARGUMENT2 = 0x00,  // SDMA System Address / Argument 2; in v4 mode (SDMA at 0x58) only the Auto CMD23 argument
    BLOCK_SIZE = 0x04,
    BLOCK_COUNT = 0x06,
    ARGUMENT = 0x08,
    TRANSFER = 0x0c,
    COMMAND = 0x0e,
    RESPONSE = 0x10,
    STOP_RESPONSE = 0x1c,  // also RESPONSE word 3 of an R2
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
    ADMA_ERROR = 0x54,
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
    INT_ADMA_ERROR = 1u << 25,
    INT_ERRORS = 0xffff8000u,
    // HOST_CONTROL DMA select (bits 4:3): SDMA, or ADMA2 (with HOST_CONTROL2's v4 + 64-bit addressing: 128-bit
    // descriptors; sdhci.c selects SDHCI_CTRL_ADMA32 for any v4 host).
    DMA_SELECT = 0x18,
    DMA_ADMA2 = 0x10,
    CONTROL2_V4_64BIT = 0x3000,
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
    bool chained;  // r12-sdio: taken into the chain being picked (IOSwitchQueue.h), under queueLock
};
// r12-sdio: one chain's way through the worker (worker()): picked, begun (beginBatch: the table built and, when the chain is
// one ADMA2 command, its pages synced and the command issued), ended (endBatch: that command waited for, retried and
// checked; or the whole bounce path), then completed (completeBatch). The worker begins the next chain before it completes
// the previous one, so the completion callbacks (IOStorageFamily, APFS, the VM's page-in wake-ups: on r7p5 100-300 us of
// card idle per command while requests queued) run while the card moves the next chain's data, not between commands.
struct Batch {
    Request *first;  // the chain, in card order
    uint32_t sectors, count;
    bool write, direct, proving, issued, staged;
    bool cmd23;  // r14-sdcq: framed with Auto CMD23 (a multi-block ADMA2 command while that is on for its direction)
    uint32_t gapSectors;  // r14-sdcq: of `sectors`, those in gaps between the requests (read and discarded)
    IOSwitchAdmaRefusal why;
    int issueStep;                // issue()'s result when the command never reached the card
    uint64_t beganNs, overlapNs;  // when beginBatch started; the previous chain's completions that ran inside this one
    IOReturn result;              // endBatch's, for every request of the chain
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
    // Worker time copying between the bounce buffer and the requests' memory (part of busyNs).
    uint64_t copyNs;
    uint32_t maxQueued;
    // r10-adma2: requests that went straight to their own pages by ADMA2 and their descriptors, requests that went
    // through the bounce buffer by why (IOSwitchAdmaRefusal; kAdmaOk: ADMA2 off), ADMA2 writes read back and found
    // equal, ADMA errors, worker time on the ADMA2 path's cache maintenance (part of busyNs), and busy time per direction.
    uint64_t direct, descriptors, bounced[kAdmaRefusals], verified, admaErrors, syncNs, readNs, writeNs;
    // r12-sdio: commands issued before the previous chain's completions ran, and of those the ones already finished when
    // the worker came back to them (no interrupt taken). Read chains that went by ADMA2 with some segments staged through
    // the bounce buffer, and those bytes.
    uint64_t overlapped, early, stagedChains, stagedBytes;
    // r14-sdcq: data commands framed with Auto CMD23; read chains with gaps, the gaps' bytes read through and discarded,
    // and of those chains the ones compared with SDMA and found equal.
    uint64_t cmd23, gapChains, gapBytes, gapProven;
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
    volatile uint32_t *mc = nullptr;                                  // memory controller (SMMU state for SDMMC1)
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
    // r12-sdio: reads merge with continuing reads anywhere in the queue (IOSwitchQueue.h) until a chain fails its check.
    bool mergeAnywhere = true;
    // Boot-arg ioswitch-sd-copy-flush=1: requests' bytes through IOMemoryDescriptor::readBytes/writeBytes again, with
    // the clean+invalidate of every destination line they add on this kernel (IOSwitchClientCopy.h), for A/B boots.
    // It keeps ADMA2 off too, so every request takes the bounce buffer as before r10-adma2.
    bool copyFlush = false;
    // r10-adma2: the ADMA2 descriptor table (one, rebuilt for every ADMA2 command; 16 KiB contiguous below 4 GiB), whether
    // ADMA2 carries data (setup()'s check passed and no ADMA error since) and writes (no write read back different), the
    // state of the last ADMA error (command()), SDMA's own read limit for the bounce path (kBounceSectors once setup()
    // has checked the boundary resume), and the read-back of the first ADMA2 writes (writeChecks left; into checkBuffer).
    IOBufferMemoryDescriptor *tableMemory = nullptr;
    IOSwitchAdmaTable table = {};
    uint64_t tablePA = 0;
    bool adma = false, admaWrites = false;
    uint8_t admaErrorState = 0;
    uint64_t admaErrorAddress = 0;
    uint32_t sdmaReadSectors = kSafeSectors;
    uint32_t writeChecks = 0;
    // r12-sdio: the longest write chain one ADMA2 command carries (kDirectWriteSectors, or kSafeSectors once a longer one
    // read back different), and the read-backs of writes over kSafeSectors left this boot.
    uint32_t writeSectors = kSafeSectors, largeWriteChecks = 0;
    // r12-sdio: whether ADMA2 reaches pages above 4 GiB (IOSwitchAdma.h), and the SDMA copy the proofs (proveHigh,
    // proveWrite) compare against: kBounceBytes, freed once no proof is left (releaseChecks).
    IOSwitchAdmaHigh high = {};
    uint8_t *checkBuffer = nullptr;
    // r12-sdio: a read chain's segments that may not go straight to the card go through the bounce buffer alone
    // (IOSwitchAdmaStaging), once setup()'s check has run it on the hardware (stagingOn).
    Staging staging = {};
    Staging::Stage *stages = nullptr;
    bool stagingOn = false;
    // r14-sdcq: Auto CMD23 framing per direction (checkCmd23 passed; writes off again if a CMD23 write read back
    // different) and the CMD23 writes still to read back this boot.
    bool cmd23Reads = false, cmd23Writes = false;
    uint32_t cmd23WriteChecks = 0;
    // r14-sdcq: read chains with gaps (IOSwitchQueue.h) while gapsOn: the sink their gaps are read into (kGapSectors,
    // contiguous below 4 GiB, never read or written by the CPU after setup) and the proofs still to do this boot.
    IOBufferMemoryDescriptor *sinkMemory = nullptr;
    uint64_t sinkPA = 0;
    bool gapsOn = false;
    uint32_t gapProofs = 0;
    bool inSink(uint64_t pa) const { return sinkPA && pa >= sinkPA && pa - sinkPA < uint64_t(kGapSectors) * kSector; }
    char admaWhy[128] = {};
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
    // r14-sdcq: the card's registers as identify() read them once at boot (CID and CSD during reinitialize(), the rest in
    // transfer state), for the identification lines and for what CMD23 and the command queue may use.
    uint8_t cid[16] = {}, csd[16] = {}, scrBytes[8] = {}, ssrBytes[64] = {};
    bool haveCid = false, haveCsd = false, haveScr = false, haveSsr = false, havePerf = false, havePower = false;
    IOSwitchSdScr scr = {};
    IOSwitchSdStatus sdStatus = {};
    IOSwitchSdGeneralInfo extInfo = {};
    IOSwitchSdPerf perf = {};
    IOSwitchSdPower power = {};
    const char *extWhy = nullptr;

    uint8_t r8(uint32_t o) { return regs[o]; }
    uint16_t r16(uint32_t o) { return *(volatile uint16_t *)(regs + o); }
    uint32_t r32(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
    void w8(uint32_t o, uint8_t v) { regs[o] = v; }
    void w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(regs + o) = v; }
    void w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }
    static void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
    bool stage(IOMemoryDescriptor *memory, uint32_t from, uint8_t *at, uint32_t length, bool toClient)
    {
        if (copyFlush)
            return (toClient ? memory->writeBytes(from, at, length) : memory->readBytes(from, at, length)) == length;
        return ioswitchClientCopy(memory, at, length, toClient, copypv, from);
    }

    bool waitIdle(bool dat0, uint64_t timeoutNs);
    bool startInterrupt();
    static bool interruptFilter(OSObject *, IOFilterInterruptEventSource *);
    bool waitSignal(uint32_t signals, uint64_t timeoutNs);
    void resetLines();
    void recover(bool data);
    int command(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, uint32_t *status, bool cmd23 = false);
    // r12-sdio: command()'s halves, so the worker can do other work while the card moves the data. r14-sdcq: `cmd23`
    // frames a multi-block command with Auto CMD23 instead of Auto CMD12 (ioswitchSdBlockFrame).
    int issue(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, bool cmd23 = false);
    int complete(uint32_t sectors, bool write, unsigned descriptors, uint32_t *status, bool *early, bool cmd23 = false);
    bool failed(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write, unsigned descriptors,
                bool retry);
    int run(uint64_t lba, uint32_t sectors, bool write, unsigned descriptors);
    int retry(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write, unsigned descriptors);
    static void worker(void *, wait_result_t);
    Request *pickLocked(uint32_t *sectors, uint32_t *count);
    void beginBatch(Batch &);
    void endBatch(Batch &);
    void completeBatch(Batch &);
    // r10-adma2: the ADMA2 path, the bounce path in SDMA-sized pieces, the write read-back, the setup check, the
    // power-state hooks.
    IOSwitchAdmaRefusal buildTable(Request *first, uint32_t sectors, bool write);
    void prepareDirect(bool write, bool poison);
    void finishDirect(bool write);
    bool stageChain(Request *first, uint32_t from, uint32_t length, uint8_t *buffer, bool toClient);
    bool bounceChain(Request *first, uint32_t sectors);
    bool proveWrite(Request *first, uint32_t sectors, bool cmd23);
    bool proveHigh(Request *first, uint32_t sectors);
    bool proveGaps(Request *first, uint32_t sectors);
    void releaseChecks();
    bool copyStaged();
    const char *checkAdma(const uint8_t *reference, uint32_t *descriptors, uint32_t *highPages, uint32_t *highOk,
                          uint32_t *staged);
    const char *checkCmd23(uint64_t *withNs, uint64_t *withoutNs);
    char cmd23Why[96] = {};
    char stagingReason[96] = {};
    void dmaSuspend();
    void dmaResume();
    void logStats();
    void logThroughput();
    // SDR104 bring-up (bounded register polling, setup() and re-tuning only).
    int cardCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data = 0);
    int appCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data = 0);
    void readR2(uint8_t out[16]);
    // r14-sdcq: the card's identity and capabilities, read and logged once at boot.
    bool extRead(uint8_t fno, uint32_t address, uint32_t length, uint8_t *out);
    void identify();
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
    // Per request and per merged command, as advertised: reads kBounceSectors with ADMA2 or once setup() has checked
    // SDMA boundary resumes, else kSafeSectors; writes kDirectWriteSectors with ADMA2, else kSafeSectors.
    uint32_t maxSectors = kSafeSectors, maxWriteSectors = kSafeSectors;
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
    setProperty(kIOMaximumBlockCountWriteKey, provider->maxWriteSectors, 64);
    setProperty(kIOMaximumByteCountReadKey, provider->maxSectors * kSector, 64);
    setProperty(kIOMaximumByteCountWriteKey, provider->maxWriteSectors * kSector, 64);
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
    if (direction == kIODirectionOut && count > owner->maxWriteSectors) return kIOReturnBadArgument;
    auto *request = static_cast<Request *>(IOMalloc(sizeof(Request)));
    if (!request) return kIOReturnNoMemory;
    buffer->retain();
    // No attributes: the default class, as IOStorage's own default priority. The boot prefetch: high.
    const IOStoragePriority priority = current_thread() == __atomic_load_n(&gPrefetchThread, __ATOMIC_ACQUIRE)
                                           ? kIOStoragePriorityHigh
                                       : attributes ? attributes->priority
                                                    : kIOStoragePriorityDefault;
    *request = {nullptr,
                buffer,
                owner->firstSector + block,
                static_cast<uint32_t>(count),
                direction == kIODirectionOut,
                *completion,
                static_cast<uint8_t>(priority >> 6),
                0,
                false};
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

// One data command: by ADMA2 through the descriptor table when `descriptors` is nonzero (built and synced by the
// caller), else by SDMA into/out of the bounce buffer. Returns 0 or a nonzero step code; *status receives the last
// interrupt status sample. r12-sdio: issue() then complete(); the worker's ADMA2 commands call the two apart.
int NativeIOSwitchBlock::command(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, uint32_t *status,
                                 bool cmd23)
{
    *status = 0;
    if (const int step = issue(lba, sectors, write, descriptors, cmd23)) return step;
    return complete(sectors, write, descriptors, status, nullptr, cmd23);
}

// The command's first half: the partition check, the registers and the command itself. 0, or step 1 when the controller
// never went idle (then nothing was issued).
int NativeIOSwitchBlock::issue(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, bool cmd23)
{
    // Every card write passes here, whichever path staged it: none may reach outside the APFS partition. (Until setup()
    // has found the partition, sectorCount is 0 and so no write can be issued at all.)
    if (write && (lba < firstSector || sectors > sectorCount || lba - firstSector > sectorCount - sectors))
        panic("IOSwitchSD: write lba=%u n=%u outside the APFS partition [%llu, +%llu)", lba, sectors, firstSector,
              sectorCount);
    if (!waitIdle(write, 2000000000)) return 1;
    w32(STATUS, 0xffffffff);
    // DMA select per command: the bring-up commands' SDMA (cardCommand) and the data path share the controller.
    const uint64_t address = descriptors ? tablePA : bouncePA;
    w8(HOST_CONTROL, static_cast<uint8_t>((r8(HOST_CONTROL) & ~DMA_SELECT) | (descriptors ? DMA_ADMA2 : 0)));
    w32(DMA_LOW, static_cast<uint32_t>(address));
    w32(DMA_HIGH, static_cast<uint32_t>(address >> 32));
    w16(BLOCK_SIZE, kSector | 0x7000);  // 512 KiB SDMA boundary (ADMA2 ignores it)
    w16(BLOCK_COUNT, static_cast<uint16_t>(sectors));
    w32(ARGUMENT, lba);
    // DMA | block-count | Auto CMD12 (or r14-sdcq: Auto CMD23, its block count in Argument 2) | read | multi, as the
    // qualified sequence.
    const IOSwitchSdFrame frame = ioswitchSdBlockFrame(sectors, write, cmd23);
    if (frame.argument2) w32(ARGUMENT2, frame.argument2);
    w16(TRANSFER, frame.transfer);
    barrier();
    w16(COMMAND, frame.command);
    return 0;
}

// The command's second half, for the command issue() started: 0 or a nonzero step code. Its time limit runs from here,
// not from issue(): the worker may have completed the previous chain's requests in between. `*early` (when asked): the
// command had already finished when this began, so it took no interrupt.
int NativeIOSwitchBlock::complete(uint32_t sectors, bool write, unsigned descriptors, uint32_t *status, bool *early,
                                  bool cmd23)
{
    *status = 0;
    const bool multi = sectors > 1;
    // One sleep per command: the worker waits for transfer complete, an error, or an SDMA boundary
    // (transfers over 512 KiB). Command complete latches in STATUS without raising the line and its R1 is
    // checked once the data has moved; a command the card refuses moves no data and so ends as a
    // data error or the time limit below. (Waiting for command complete first cost a second
    // interrupt and context switch per request, a real share of a ~4 KiB read.) ADMA2 has no boundary to resume: its
    // DMA interrupt only follows a descriptor with the Int attribute, which none of ours has.
    const uint32_t awaited = INT_TRANSFER | (descriptors ? 0 : INT_DMA) | ERROR_SIGNALS;
    const uint64_t start = nowNs();
    const uint64_t limit = write ? 5000000000ull : 2000000000ull;
    uint32_t s, resume = 0;
    bool waited = false;
    for (;;) {
        s = r32(STATUS);
        *status = s;
        if (descriptors && (s & INT_ADMA_ERROR)) {
            // The engine's state and the descriptor it stopped at (ADMA Error Status; ADMA System Address), for failed().
            admaErrorState = r8(ADMA_ERROR);
            admaErrorAddress = r32(DMA_LOW) | static_cast<uint64_t>(r32(DMA_HIGH)) << 32;
            return kStepAdmaError;
        }
        if (s & INT_ERRORS) return (s & INT_COMMAND) ? 3 : 2;
        if (s & INT_TRANSFER) break;
        if (!descriptors && (s & INT_DMA)) {
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
        waited = true;
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
    if (early) *early = !waited;
    w32(STATUS, INT_COMMAND | INT_TRANSFER);
    if (!(s & INT_COMMAND) || (r32(RESPONSE) & R1_ERRORS)) return 4;
    if (multi) {
        // Auto CMD12's or (r14-sdcq) Auto CMD23's errors; only a CMD12 leaves a response in STOP_RESPONSE.
        if (r16(AUTO_CMD_ERROR) & 0x9f) return 8;
        if (!cmd23 && (r32(STOP_RESPONSE) & R1_STOP_ERRORS)) return 9;
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
    if (const int step = run(lba, sectors, write, 0)) return step;
    if (!write) {
        for (uintptr_t at = (uintptr_t)bounce, end = at + length; at < end; at += 64)
            __asm__ volatile("dc ivac, %0" ::"r"(at) : "memory");
        barrier();
        if (bytes != bounce) memcpy(bytes, bounce, length);
    }
    return 0;
}

// Logs a failed data command and recovers the card. True when ADMA2 itself failed (the host's DMA, not the card or the
// bus): no retry and no re-tune then; the caller moves the data through the bounce buffer instead.
bool NativeIOSwitchBlock::failed(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write,
                                 unsigned descriptors, bool retry)
{
    IOLog("IOSwitchSD: %s%s lba=%llu n=%u failed step=%d status=0x%08x present=0x%08x acmd=0x%04x\n",
          retry ? "retry " : "", write ? "write" : "read", lba, sectors, step, status, r32(PRESENT),
          r16(AUTO_CMD_ERROR));
    stats.errors++;
    stats.lastError = static_cast<uint32_t>(step);
    stats.lastStatus = status;
    const bool host = step == kStepAdmaError;
    if (host) {
        stats.admaErrors++;
        IOLog("IOSwitchSD: ADMA2 error: state 0x%02x, at 0x%llx (table 0x%llx, %u descriptors)\n", admaErrorState,
              admaErrorAddress, tablePA, descriptors);
    }
    recover(true);
    return host;
}

// One data command (command(); `descriptors` as there) with the error handling every data command gets: a failure is
// logged and the card recovered; at SDR104 a data error is most often a sampling point that drifted (temperature), so the
// bus is tuned again before the one retry, as the Linux MMC core re-tunes on CRC errors. Failing again right after a
// re-tune, later I/O runs one clock step slower (hekate's own answer to errors at SDR104 is its slower SDR82,
// sd.c:96-117).
int NativeIOSwitchBlock::run(uint64_t lba, uint32_t sectors, bool write, unsigned descriptors)
{
    uint32_t status = 0;
    const int step = command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status);
    return step ? retry(step, status, lba, sectors, write, descriptors) : 0;
}

// run()'s error handling for a command that failed with `step` (also for the worker's split ADMA2 commands): logged and
// recovered, re-tuned, issued once more. r14-sdcq: the retry stops by Auto CMD12 whatever the first try's framing was.
int NativeIOSwitchBlock::retry(int step, uint32_t status, uint64_t lba, uint32_t sectors, bool write,
                               unsigned descriptors)
{
    if (failed(step, status, lba, sectors, write, descriptors, false)) return step;
    stats.retries++;
    if (tuned) retune(false);
    if (write && !descriptors) {
        for (uintptr_t at = (uintptr_t)bounce, end = at + sectors * kSector; at < end; at += 64)
            __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
        barrier();
    }
    step = command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status);
    if (step && !failed(step, status, lba, sectors, write, descriptors, true) && tuned && speed + 1 < kSpeedCount)
        retune(true);
    return step;
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

// A page the ADMA2 path may DMA to or from and maintain by line through the kernel's physical aperture: managed RAM with
// the default write-back cacheable attributes (VM_WIMG_DEFAULT). A write-combined, uncached or device page (a GPU buffer,
// a carve-out) takes the bounce buffer, where copypv reaches it with its own attributes.
static bool cacheableRam(uint64_t page)
{
    const ppnum_t pn = static_cast<ppnum_t>(atop_64(page));
    return page >= gPhysBase && page - gPhysBase < gPhysSize && pmap_valid_page(pn) && pmap_cache_attributes(pn) == 0x2;
}

// By line over every byte the table moves, through the kernel's physical aperture, a page at a time: clean (the card
// reads what the CPU wrote), clean+invalidate (before a read: no dirty line of the client's can later be evicted over
// what the card wrote) or invalidate (after a read: drops what the A57 prefetched meanwhile). The table only holds whole
// lines of managed RAM (IOSwitchAdma.h), so no other data shares a line and phystokv covers every page.
// r14-sdcq: entries into [skip, skip + skipBytes) (the gap sink, which the CPU never touches) are left out.
template <int op> static void syncLines(const IOSwitchAdmaTable &table, uint64_t skip = 0, uint64_t skipBytes = 0)
{
    for (uint32_t i = 0; i < table.count; ++i) {
        uint64_t pa = ioswitchAdmaAddress(table.entries[i]);
        if (pa - skip < skipBytes) continue;
        for (const uint64_t end = pa + table.entries[i].length; pa < end;) {
            const uint64_t page = (pa | (kAdmaPage - 1)) + 1, stop = page < end ? page : end;
            for (uintptr_t at = phystokv(pa), last = at + (stop - pa); at < last; at += kAdmaLine) {
                if (op == 0)
                    __asm__ volatile("dc cvac, %0" ::"r"(at) : "memory");
                else if (op == 1)
                    __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
                else
                    __asm__ volatile("dc ivac, %0" ::"r"(at) : "memory");
            }
            pa = stop;
        }
    }
}

// The descriptor table for a chain, from each request's physical segments; kAdmaOk, or why the chain takes the bounce
// buffer instead. How far up it may reach depends on the direction and on how far ADMA2 above 4 GiB is proven (high).
IOSwitchAdmaRefusal NativeIOSwitchBlock::buildTable(Request *first, uint32_t sectors, bool write)
{
    table.reset();
    staging.count = 0;
    table.limit = high.limit(write);
    const auto pageOk = [&](uint64_t page) { return cacheableRam(page) && high.pageOk(page, write); };
    // r14-sdcq: a read chain's gaps go to the sink, one descriptor each (no gap is longer than the sink).
    uint64_t lba = first->lba;
    const auto gap = [&](const Request *r) -> uint64_t { return (r->lba - lba) * kSector; };
    if (!write && stagingOn) {
        // r12-sdio: a read's refused segments alone through the bounce buffer (IOSwitchAdmaStaging).
        staging.reset(&table, bouncePA, stages, kStages);
        table.limit = high.limit(write);
        for (Request *r = first; r; r = r->next) {
            IOSwitchAdmaRefusal why = kAdmaOk;
            if (gap(r))
                why = gap(r) > uint64_t(kGapSectors) * kSector || !sinkPA ? kAdmaMissing
                                                                          : staging.addSink(sinkPA, gap(r));
            if (!why) why = staging.addMemory(r->buffer, uint64_t(r->sectors) * kSector, pageOk);
            if (why) {
                staging.count = 0;
                return why;
            }
            lba = r->lba + r->sectors;
        }
        if (!staging.finish()) {
            staging.count = 0;
            return kAdmaFull;
        }
        return table.bytes == uint64_t(sectors) * kSector ? kAdmaOk : kAdmaMissing;
    }
    for (Request *r = first; r; r = r->next) {
        IOSwitchAdmaRefusal why = kAdmaOk;
        if (gap(r))
            why = gap(r) > uint64_t(kGapSectors) * kSector || !sinkPA ? kAdmaMissing : table.addSealed(sinkPA, gap(r));
        if (!why) why = table.addMemory(r->buffer, uint64_t(r->sectors) * kSector, pageOk);
        if (why) return why;
        lba = r->lba + r->sectors;
    }
    return table.bytes == uint64_t(sectors) * kSector && table.finish() ? kAdmaOk : kAdmaMissing;
}

// An ADMA2 command for the chain in `table` (buildTable) goes straight to or from the requests' pages. Before it
// (prepareDirect): the descriptors and the pages cleaned (write) or cleaned and invalidated (read). After it
// (finishDirect): a read's pages invalidated again. `poison` (a read that proves ADMA2 above 4 GiB, proveHigh): the
// table's pages up there are filled with kPoison first, so bytes the engine never delivered show (the clean+invalidate
// takes the fill to DRAM before the card's bytes arrive).
void NativeIOSwitchBlock::prepareDirect(bool write, bool poison)
{
    const uint64_t began = nowNs();
    for (uintptr_t at = (uintptr_t)table.entries, end = at + table.count * sizeof(IOSwitchAdmaDescriptor); at < end;
         at += kAdmaLine)
        __asm__ volatile("dc cvac, %0" ::"r"(at) : "memory");
    for (uint32_t i = 0; poison && !write && i < table.count; ++i) {
        uint64_t pa = ioswitchAdmaAddress(table.entries[i]);
        for (const uint64_t end = pa + table.entries[i].length; pa < end;) {
            const uint64_t page = (pa | (kAdmaPage - 1)) + 1, stop = page < end ? page : end;
            if (pa >= kAdma4GiB) memset(reinterpret_cast<void *>(phystokv(pa)), kPoison, stop - pa);
            pa = stop;
        }
    }
    if (write)
        syncLines<0>(table);
    else
        syncLines<1>(table, sinkPA, sinkPA ? uint64_t(kGapSectors) * kSector : 0);
    barrier();
    stats.syncNs += nowNs() - began;
}

// r12-sdio: a read's staged segments from the bounce buffer out to their requests (after finishDirect). False when a copy
// failed (the chain completes with an I/O error).
bool NativeIOSwitchBlock::copyStaged()
{
    const uint64_t began = nowNs();
    bool ok = true;
    for (uint32_t i = 0; ok && i < staging.count; ++i) {
        const Staging::Stage &stageAt = staging.stages[i];
        ok = stage(stageAt.memory, static_cast<uint32_t>(stageAt.offset), bounce + stageAt.at, stageAt.length, true);
    }
    stats.copyNs += nowNs() - began;
    return ok;
}

void NativeIOSwitchBlock::finishDirect(bool write)
{
    if (write) return;
    const uint64_t began = nowNs();
    syncLines<2>(table, sinkPA, sinkPA ? uint64_t(kGapSectors) * kSector : 0);
    barrier();
    stats.syncNs += nowNs() - began;
}

// The chain's bytes [from, from + length) between the requests' memory and the start of `buffer` (the bounce buffer, or
// the proofs' SDMA copy). r14-sdcq: toward the requests a read chain's gaps are skipped; from the requests (a write, or a
// read-back's comparison copy) every byte must come from one (ioswitchChainCopy).
bool NativeIOSwitchBlock::stageChain(Request *first, uint32_t from, uint32_t length, uint8_t *buffer, bool toClient)
{
    const uint64_t began = nowNs();
    const bool ok = ioswitchChainCopy(
        first, from, length, buffer,
        [&](Request *r, uint32_t offset, uint8_t *at, uint32_t n) { return stage(r->buffer, offset, at, n, toClient); },
        toClient);
    stats.copyNs += nowNs() - began;
    return ok;
}

// The chain through the bounce buffer by SDMA, in as many commands as SDMA's own limits need (writes kSafeSectors, reads
// sdmaReadSectors: a chain merged for ADMA2 can be longer), each piece staged before or copied out after its command.
bool NativeIOSwitchBlock::bounceChain(Request *first, uint32_t sectors)
{
    const bool write = first->write;
    const uint32_t limit = write ? kSafeSectors : sdmaReadSectors;
    for (uint32_t done = 0; done < sectors;) {
        const uint32_t n = sectors - done < limit ? sectors - done : limit;
        if (write && !stageChain(first, done * kSector, n * kSector, bounce, false)) return false;
        if (transfer(first->lba + done, n, write, bounce)) return false;
        if (!write && !stageChain(first, done * kSector, n * kSector, bounce, true)) return false;
        done += n;
    }
    return true;
}

// The first kWriteChecks ADMA2 writes of a boot, and the first writes from pages above 4 GiB (high), are read back by SDMA
// (the proven path) and compared with the requests' memory. False only for data that differs: the caller then writes the
// chain again through the bounce buffer and ADMA2 writes stay off until reboot. A read-back that fails proves nothing
// either way and is not counted.
bool NativeIOSwitchBlock::proveWrite(Request *first, uint32_t sectors, bool cmd23)
{
    const uint32_t length = sectors * kSector;
    if (!checkBuffer || sectors > kBounceSectors) return true;
    for (uint32_t done = 0; done < sectors;) {
        const uint32_t n = sectors - done < kSafeSectors ? sectors - done : kSafeSectors;
        if (transfer(first->lba + done, n, false, checkBuffer + done * kSector)) return true;
        done += n;
    }
    if (!stageChain(first, 0, length, bounce, false)) return true;
    if (!memcmp(bounce, checkBuffer, length)) {
        stats.verified++;
        if (writeChecks) --writeChecks;
        if (table.high) high.writeChecked();
        if (sectors > kSafeSectors && largeWriteChecks) --largeWriteChecks;
        if (cmd23 && cmd23WriteChecks) --cmd23WriteChecks;
        releaseChecks();
        return true;
    }
    uint32_t at = 0;
    while (bounce[at] == checkBuffer[at]) ++at;
    if (cmd23) {
        // r14-sdcq: Auto CMD23 is what is new about this write (ADMA2 writes have read back equal for many boots).
        IOLog(
            "IOSwitchSD: CMD23 write lba=%llu n=%u reads back different from byte %u; written again through the bounce "
            "buffer, writes stop by CMD12 until reboot\n",
            first->lba, sectors, at);
        cmd23Writes = false;
    } else if (sectors > kSafeSectors) {
        // r12-sdio: only writes over 256 KiB are new: they take the bounce path's 256 KiB SDMA commands from now on.
        IOLog(
            "IOSwitchSD: ADMA2 write lba=%llu n=%u (%llu bytes above 4 GiB) reads back different from byte %u; written "
            "again through the bounce buffer, writes over 256 KiB in 256 KiB commands until reboot\n",
            first->lba, sectors, table.high, at);
        writeSectors = kSafeSectors;
    } else {
        IOLog(
            "IOSwitchSD: ADMA2 write lba=%llu n=%u (%llu bytes above 4 GiB) reads back different from byte %u; written "
            "again through the bounce buffer, ADMA2 writes off until reboot\n",
            first->lba, sectors, table.high, at);
        admaWrites = false;
    }
    releaseChecks();
    return false;
}

// r12-sdio: a read chain whose table reached above 4 GiB while that is being proven (high), right after its ADMA2 command
// (prepareDirect poisoned those pages first): the same blocks again by SDMA into checkBuffer, compared with every byte the
// table moved. Equal: one proof more (the last one turns ADMA2 above 4 GiB on, one line). Different: SDMA's copy goes into
// the requests' memory, so they complete with the proven path's bytes, and ADMA2 stays below 4 GiB until reboot (one
// line). If the SDMA read fails, nothing is proven and the chain is read again through the bounce buffer. False only when
// the requests' bytes could not be made right (they complete with an I/O error).
bool NativeIOSwitchBlock::proveHigh(Request *first, uint32_t sectors)
{
    const uint32_t length = sectors * kSector;
    for (uint32_t done = 0; checkBuffer && done < sectors;) {
        const uint32_t n = sectors - done < kSafeSectors ? sectors - done : kSafeSectors;
        if (transfer(first->lba + done, n, false, checkBuffer + done * kSector)) return bounceChain(first, sectors);
        done += n;
    }
    if (!checkBuffer) return bounceChain(first, sectors);
    // The table's descriptors are in chain order: compare the requests' own pages page by page through the kernel's
    // physical aperture (as syncLines reaches them), counting what still holds the poison. Runs staged in the bounce
    // buffer were copied out already (and the buffer has been read into since): their bytes are skipped here.
    uint64_t at = 0, poisoned = 0, differs = UINT64_MAX, differsPA = 0;
    for (uint32_t i = 0; i < table.count; ++i) {
        uint64_t pa = ioswitchAdmaAddress(table.entries[i]);
        if ((pa >= bouncePA && pa - bouncePA < kBounceBytes) || inSink(pa)) {
            at += table.entries[i].length;  // r14-sdcq: a gap's bytes in the sink are nobody's
            continue;
        }
        for (const uint64_t end = pa + table.entries[i].length; pa < end;) {
            const uint64_t page = (pa | (kAdmaPage - 1)) + 1, stop = page < end ? page : end;
            const auto *memory = reinterpret_cast<const uint8_t *>(phystokv(pa));
            if (memcmp(memory, checkBuffer + at, stop - pa)) {
                for (uint64_t k = 0; k < stop - pa; ++k) {
                    if (memory[k] == checkBuffer[at + k]) continue;
                    if (differs == UINT64_MAX) {
                        differs = at + k;
                        differsPA = pa + k;
                    }
                    if (memory[k] == kPoison) ++poisoned;
                }
            }
            at += stop - pa;
            pa = stop;
        }
    }
    if (differs == UINT64_MAX) {
        const uint64_t bytes = table.high;
        if (high.proved(bytes)) {
            IOLog("IOSwitchSD: ADMA2 above 4 GiB proven: %u reads (%llu KiB up there) match SDMA; requests' pages go "
                  "straight up to %llu GiB, the first %u writes from up there read back\n",
                  IOSwitchAdmaHigh::kProofs, high.provenBytes >> 10, kAdmaDmaLimit >> 30, high.writeProofsLeft);
        }
        releaseChecks();
        return true;
    }
    high.refuse();
    IOLog(
        "IOSwitchSD: ADMA2 read lba=%llu n=%u (%llu bytes above 4 GiB) differs from SDMA at byte %llu (PA 0x%llx, %llu "
        "bytes still poisoned); SDMA's copy used, ADMA2 stays below 4 GiB until reboot\n",
        first->lba, sectors, table.high, differs, differsPA, poisoned);
    const bool repaired = stageChain(first, 0, length, checkBuffer, true);
    releaseChecks();
    return repaired;
}

// r14-sdcq: the first kGapProofs read chains with gaps of every boot, right after their command: the command's blocks again
// by SDMA into checkBuffer (the proven path, stopped by CMD12), and each request's bytes compared with their place there.
// Equal: one proof more (the last one logs). Different: SDMA's bytes go into the requests, so they complete with the
// proven path's data, and gap reads stay off until reboot (one line). If the SDMA read fails nothing is proven and the
// chain is read again through the bounce buffer. False only when the requests' bytes could not be made right.
bool NativeIOSwitchBlock::proveGaps(Request *first, uint32_t sectors)
{
    if (!checkBuffer) return true;
    for (uint32_t done = 0; done < sectors;) {
        const uint32_t n = sectors - done < kSafeSectors ? sectors - done : kSafeSectors;
        if (transfer(first->lba + done, n, false, checkBuffer + done * kSector)) return bounceChain(first, sectors);
        done += n;
    }
    // Each request's bytes into the bounce buffer at its place in the command (the gaps' places stay as they are).
    uint64_t differs = UINT64_MAX;
    for (Request *r = first; r && differs == UINT64_MAX; r = r->next) {
        const uint32_t at = static_cast<uint32_t>(r->lba - first->lba) * kSector, length = r->sectors * kSector;
        if (!stage(r->buffer, 0, bounce + at, length, false))
            return stageChain(first, 0, sectors * kSector, checkBuffer, true);
        if (!memcmp(bounce + at, checkBuffer + at, length)) continue;
        differs = at;
        while (bounce[differs] == checkBuffer[differs]) ++differs;
    }
    if (differs == UINT64_MAX) {
        ++stats.gapProven;
        if (gapProofs && !--gapProofs)
            IOLog("IOSwitchSD: gap reads proven: %u chains read through their gaps match SDMA\n", kGapProofs);
        releaseChecks();
        return true;
    }
    gapsOn = false;
    IOLog("IOSwitchSD: read chain lba=%llu n=%u with gaps differs from SDMA at byte %llu; SDMA's copy used, "
          "gap reads off until reboot\n",
          first->lba, sectors, differs);
    const bool repaired = stageChain(first, 0, sectors * kSector, checkBuffer, true);
    releaseChecks();
    return repaired;
}

// The proofs' SDMA buffer, once neither the first writes nor ADMA2 above 4 GiB need one.
void NativeIOSwitchBlock::releaseChecks()
{
    if (!checkBuffer || (admaWrites && (writeChecks || (largeWriteChecks && writeSectors > kSafeSectors))) ||
        (adma && high.checking(admaWrites)) || (admaWrites && cmd23Writes && cmd23WriteChecks) || (gapsOn && gapProofs))
        return;
    IOFreeData(checkBuffer, kBounceBytes);
    checkBuffer = nullptr;
}

// One card command for a chain of requests that continue each other on the card (same
// direction, each starting where the previous ended, worker() built it in FIFO order); then
// every request completes with its own length. A single request is a chain of one.
// r12-sdio: in three steps (Batch). beginBatch: the checks, the table and, for a chain that is one ADMA2 command, the
// cache maintenance and the command issued. Nothing in it waits for the card.
void NativeIOSwitchBlock::beginBatch(Batch &b)
{
    Request *const first = b.first;
    const uint32_t sectors = b.sectors;
    // Each request was checked against the partition window in doAsyncReadWrite and the
    // chain is their contiguous union; checking the command itself keeps any bug here from
    // ever reaching outside the APFS partition (issue() checks every write again).
    if (first->lba < firstSector || sectors > sectorCount || first->lba - firstSector > sectorCount - sectors)
        panic("IOSwitchSD: command lba=%llu n=%u outside the APFS partition [%llu, +%llu)", first->lba, sectors,
              firstSector, sectorCount);
    // r14-sdcq: the sectors no request of the chain covers (a read chain's gaps). A write chain has none: every byte it
    // puts on the card comes from its requests (pickLocked takes only the requests right behind a write).
    uint64_t payload = 0;
    for (const Request *r = first; r; r = r->next) payload += r->sectors;
    if (first->write && payload != sectors)
        panic("IOSwitchSD: write chain lba=%llu n=%u carries %llu sectors of requests", first->lba, sectors, payload);
    b.gapSectors = static_cast<uint32_t>(sectors - payload);
    b.write = first->write;
    b.beganNs = nowNs();
    b.overlapNs = 0;
    b.issued = false;
    b.issueStep = 0;
    // r10-adma2: straight between the card and the requests' pages by ADMA2 when it is on (writes: unless proveWrite
    // turned them off) and the whole chain qualifies; through the bounce buffer otherwise, counted by why.
    b.why = kAdmaOk;
    b.direct = adma && (!b.write || (admaWrites && sectors <= writeSectors)) &&
               (b.why = buildTable(first, sectors, b.write)) == kAdmaOk;
    // r12-sdio: a read that reaches above 4 GiB while that is unproven is compared with SDMA before it completes.
    b.proving = b.direct && high.proofDue(b.write, table.high);
    b.staged = b.direct && staging.count;
    b.cmd23 = b.direct && sectors > 1 && (b.write ? cmd23Writes : cmd23Reads);
    if (!b.direct) return;
    prepareDirect(b.write, b.proving);
    b.issueStep = issue(static_cast<uint32_t>(first->lba), sectors, b.write, table.count, b.cmd23);
    b.issued = true;
}

// endBatch: the issued command waited for (the interrupt, unless it finished meanwhile), retried and checked, then the
// proofs; or the whole bounce path. The stats.
void NativeIOSwitchBlock::endBatch(Batch &b)
{
    Request *const first = b.first;
    const uint32_t sectors = b.sectors;
    const bool write = b.write;
    IOReturn result = kIOReturnSuccess;
    if (b.direct) {
        uint32_t status = 0;
        bool early = false;
        int step = b.issueStep ? b.issueStep : complete(sectors, write, table.count, &status, &early, b.cmd23);
        if (b.cmd23) ++stats.cmd23;
        if (!b.issueStep && b.overlapNs) {
            ++stats.overlapped;
            if (early) ++stats.early;
        }
        if (step) step = retry(step, status, first->lba, sectors, write, table.count);
        finishDirect(write);
        // r12-sdio: staged segments out to their requests before any proof reuses the bounce buffer.
        if (!step && b.staged) {
            if (!copyStaged()) result = kIOReturnIOError;
            ++stats.stagedChains;
            stats.stagedBytes += staging.staged;
        }
        if (step == kStepAdmaError && b.proving) {
            // Only the pages above 4 GiB are new to the engine: they stay out of it, and this chain goes by SDMA.
            high.refuse();
            b.direct = false;
            IOLog(
                "IOSwitchSD: ADMA2 error on a read above 4 GiB (state 0x%02x at 0x%llx); ADMA2 stays below 4 GiB until "
                "reboot\n",
                admaErrorState, admaErrorAddress);
            releaseChecks();
        } else if (step == kStepAdmaError) {
            // The host's DMA failed, not the card: this chain and every later one through the bounce buffer.
            adma = false;
            b.direct = false;
            IOLog("IOSwitchSD: ADMA2 off until reboot (step %d); requests go through the bounce buffer\n", step);
        } else if (step) {
            result = kIOReturnIOError;
        } else if (b.proving) {
            if (!proveHigh(first, sectors)) result = kIOReturnIOError;
        } else if (b.gapSectors && gapProofs) {
            if (!proveGaps(first, sectors)) result = kIOReturnIOError;
        } else if (write &&
                   (writeChecks || high.writeCheckDue(write, table.high) ||
                    (sectors > kSafeSectors && largeWriteChecks) || (b.cmd23 && cmd23WriteChecks)) &&
                   !proveWrite(first, sectors, b.cmd23)) {
            b.direct = false;
        }
    }
    if (b.direct) {
        stats.direct += b.count;
        stats.descriptors += table.count;
    } else {
        if (!bounceChain(first, sectors)) result = kIOReturnIOError;
        stats.bounced[b.why] += b.count;
    }
    b.result = result;
    // Card and copies first, completions after: busy time covers the card, the copies and the cache maintenance,
    // completeNs the callbacks (IOStorageFamily/APFS work that runs on this thread; r12-sdio: while the next command
    // runs, their time inside this chain's span is not this chain's busy time).
    for (Request *r = first; r; r = r->next) {
        const uint32_t length = r->sectors * kSector;
        if (write) {
            stats.writes++;
            if (!result) stats.writeBytes += length;
        } else {
            stats.reads++;
            if (!result) stats.readBytes += length;
        }
        if (r->sectors <= 8) stats.small++;
        if (r->lba == lastEnd) stats.sequential++;
        lastEnd = r->lba + r->sectors;
    }
    const uint64_t span = nowNs() - b.beganNs, latency = span > b.overlapNs ? span - b.overlapNs : 0;
    stats.commands++;
    if (b.gapSectors) {
        ++stats.gapChains;
        stats.gapBytes += uint64_t(b.gapSectors) * kSector;
    }
    stats.busyNs += latency;
    (write ? stats.writeNs : stats.readNs) += latency;
    stats.merged += b.count - 1;
    if (latency > stats.maxLatencyNs) stats.maxLatencyNs = latency;
}

// completeBatch: every request of the chain completes with endBatch's result.
void NativeIOSwitchBlock::completeBatch(Batch &b)
{
    const uint64_t began = nowNs();
    for (Request *r = b.first; r;) {
        Request *next = r->next;
        const uint32_t length = r->sectors * kSector;
        IOMemoryDescriptor *buffer = r->buffer;
        IOStorageCompletion completion = r->completion;
        IOFree(r, sizeof(*r));
        buffer->release();
        // IOStorage::complete, without binding another original IOStorageFamily symbol.
        if (completion.action)
            completion.action(completion.target, completion.parameter, b.result, b.result ? 0 : length);
        __atomic_fetch_add(&serviced, 1, __ATOMIC_RELAXED);
        r = next;
    }
    b.first = nullptr;
    stats.completeNs += nowNs() - began;
}

// The next chain off the queue (queueLock held, queue not empty), unlinked: the head, or a passing read (r6-lows), and the
// queued requests that continue it on the card.
Request *NativeIOSwitchBlock::pickLocked(uint32_t *chainSectors, uint32_t *chainCount)
{
    // Take the head and every queued request that continues it on the card in the same
    // direction, up to one command's limit: one command, one card access latency, one
    // interrupt instead of several. No write passes another.
    Request *prev = nullptr, *first = head;
    const uint64_t pickNs = nowNs();
    // r6-lows: a read of a higher IOStoragePriority class goes first (the UI's page-ins, I/O tier 0, behind up to ~100 queued
    // background requests at ~0.5 ms each otherwise): the oldest read of the best class queued, unless it overlaps an older
    // request or the head has waited kAgeNs.
    if (first->next && first->klass && pickNs - first->queuedNs < kAgeNs) {
        unsigned better = 0;
        while (better < first->klass && !queuedReads[better]) ++better;
        if (better < first->klass) {
            for (Request *before = first, *candidate = first->next; candidate;
                 before = candidate, candidate = candidate->next) {
                if (candidate->write || candidate->klass != better) continue;
                bool overlaps = false;
                for (Request *older = head; older != candidate && !overlaps; older = older->next)
                    overlaps = older->lba < candidate->lba + candidate->sectors &&
                               candidate->lba < older->lba + older->sectors;
                if (overlaps) continue;
                prev = before;
                first = candidate;
                ++passed;
                ++passedTotal;
                break;
            }
        }
    }
    uint32_t sectors = first->sectors, count = 1;
    bool gathered = false;
    if (merge && mergeAnywhere && !first->write) {
        // r12-sdio: a read takes every queued read that continues it on the card, before or after it, wherever it is
        // queued, unless that read overlaps an older request still queued (IOSwitchQueue.h). Back in card order. The
        // chain must fit its command exactly (tile it, but for gaps within the bounds); one that does not (never seen;
        // host-tested) stays queued as it was, and this boot goes back to the old rule, with one line.
        // r14-sdcq: and, while gap reads are on, queued reads up to kGapSectors away (kGapBudget per command).
        uint64_t start = 0;
        const uint32_t gap = gapsOn ? kGapSectors : 0, budget = gapsOn ? kGapBudget : 0;
        count = ioswitchGatherReads(head, first, maxSectors, kMaxChain, kMergeScan, &start, &sectors, gap, budget);
        if (ioswitchChainFits(head, start, sectors, count, gap, budget)) {
            first = ioswitchUnlinkChain(head, tail, count);
            gathered = true;
        } else {
            ioswitchClearChain(head);
            mergeAnywhere = false;
            IOLog("IOSwitchSD: read chain lba=%llu n=%u (%u requests) does not fit; reads merge only with the requests "
                  "right behind them until reboot\n",
                  start, sectors, count);
            sectors = first->sectors;
            count = 1;
        }
    }
    if (!gathered) {
        Request *last = first;
        const uint32_t limit = first->write ? writeSectors : maxSectors;
        // A passing read's chain passes the older requests too: each request it takes must not overlap any of them either.
        auto overlapsOlder = [&](const Request *later) {
            if (!prev) return false;
            for (const Request *older = head; older != first; older = older->next)
                if (older->lba < later->lba + later->sectors && later->lba < older->lba + older->sectors) return true;
            return false;
        };
        while (merge && count < kMaxChain && last->next && last->next->write == first->write &&
               last->next->lba == last->lba + last->sectors && sectors + last->next->sectors <= limit &&
               !overlapsOlder(last->next)) {
            last = last->next;
            sectors += last->sectors;
            ++count;
        }
        // Unlink first..last (the head's run, or a passing read's from the middle).
        if (prev)
            prev->next = last->next;
        else
            head = last->next;
        if (tail == last) tail = prev;
        last->next = nullptr;
    }
    queued -= count;
    for (Request *r = first; r; r = r->next) {
        if (!r->write) --queuedReads[r->klass];
        ClassWait &wait = waits[r->klass];
        const uint64_t waited = pickNs > r->queuedNs ? pickNs - r->queuedNs : 0;
        ++wait.n;
        wait.sumNs += waited;
        if (waited > wait.maxNs) wait.maxNs = waited;
    }
    *chainSectors = sectors;
    *chainCount = count;
    return first;
}

void NativeIOSwitchBlock::worker(void *context, wait_result_t)
{
    auto *self = static_cast<NativeIOSwitchBlock *>(context);
    // r12-sdio: the chain whose command runs (next) and the one whose requests are still to complete (pending), in turns.
    Batch batches[2] = {};
    unsigned turn = 0;
    Batch *pending = nullptr;
    for (;;) {
        IOLockLock(self->queueLock);
        while (!pending && (!self->head || self->suspended)) IOLockSleep(self->queueLock, &self->head, THREAD_UNINT);
        Batch *next = nullptr;
        if (self->head && !self->suspended) {
            next = &batches[turn];
            turn ^= 1;
            next->first = self->pickLocked(&next->sectors, &next->count);
            self->working = true;
        }
        IOLockUnlock(self->queueLock);
        // The next chain's command goes to the card first; the previous chain's completions run while it moves data. A
        // chain that is not one ADMA2 command (the bounce path) only has its table built here, so those completions are
        // not held behind its several commands.
        if (next) self->beginBatch(*next);
        if (pending) {
            const uint64_t began = nowNs();
            self->completeBatch(*pending);
            if (next && next->issued) next->overlapNs = nowNs() - began;
            pending = nullptr;
        }
        if (!next) continue;
        self->endBatch(*next);
        pending = next;
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
// bounce copies, not completions), copy the part of it spent copying between the bounce buffer and the
// requests' memory; "of" is wall time since the first request, so the percentage
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
    // r14-sdcq: read chains with gaps and the KiB read through those gaps (gap chains/KiB).
    // r12-sdio: chains (cmds; the bounce path's extra SDMA pieces not counted; r14-sdcq: c23 of them framed with Auto
    // CMD23), SDMMC1 interrupts taken (irq), and of the
    // ADMA2 commands issued before the previous chain's completions ran, those that had already finished when the worker
    // came back to them (ovl finished/issued; they took no interrupt).
    // r10-adma2: requests by ADMA2 (dma) and through the bounce buffer, the latter by why when any did (IOSwitchAdmaRefusal:
    // off = ADMA2 off), ADMA2 descriptors per ADMA2 request, ADMA2 writes read back equal (vfy), cache maintenance time
    // (sync), and MB/s per direction over each direction's own busy time. r12-sdio: of the ADMA2 read chains, those with
    // segments staged through the bounce buffer, and those bytes (part chains/KiB).
    char bounces[96] = "";
    {
        static const char *const names[kAdmaRefusals] = {"off", "align", "mem", "high", "full", "gone"};
        size_t at = 0;
        for (unsigned i = 0; i < kAdmaRefusals; ++i)
            if (stats.bounced[i] && at < sizeof(bounces))
                at += (size_t)snprintf(bounces + at, sizeof(bounces) - at, "%s%s %llu", at ? " " : " (", names[i],
                                       stats.bounced[i]);
        if (at && at < sizeof(bounces)) snprintf(bounces + at, sizeof(bounces) - at, ")");
    }
    uint64_t bounced = 0;
    for (unsigned i = 0; i < kAdmaRefusals; ++i) bounced += stats.bounced[i];
    const uint64_t perRequest = stats.direct ? stats.descriptors * 10 / stats.direct : 0;
    // r12-sdio: ADMA2 above 4 GiB: on, off, or proofs done/needed.
    char highState[16];
    if (!adma || high.state == IOSwitchAdmaHigh::kOff)
        strlcpy(highState, "off", sizeof(highState));
    else if (high.state == IOSwitchAdmaHigh::kOn)
        strlcpy(highState, "on", sizeof(highState));
    else
        snprintf(highState, sizeof(highState), "%u/%u", IOSwitchAdmaHigh::kProofs - high.proofsLeft,
                 IOSwitchAdmaHigh::kProofs);
    const uint64_t readRate = stats.readNs ? stats.readBytes * 10000 / stats.readNs : 0;
    const uint64_t writeRate = stats.writeNs ? stats.writeBytes * 10000 / stats.writeNs : 0;
    // Periodic: to the log only (os_log), never the boot console.
    char out[640];
    snprintf(
        out, sizeof(out),
        "IOSwitchSD: %llu rq %llu MiB avg %lluK | busy %llu.%llus copy %llums sync %llums %llu.%llu MB/s "
        "(rd %llu.%llu wr %llu.%llu) %llu%% of %llus | dma %llu bounce %llu%s seg %llu.%llu part %llu/%lluK vfy %llu "
        "wmax %uK 4G+ %s | "
        "<=4K %llu%% seq %llu%% merged %llu gap %llu/%lluK q%u | cmds %llu c23 %llu irq %llu ovl %llu/%llu | err %llu "
        "adma %llu retune %llu | "
        "%s %u.%02u MHz | wait ms%s",
        requests, bytes >> 20, requests ? (bytes / requests) >> 10 : 0, busy / 1000000000, busy / 100000000 % 10,
        stats.copyNs / 1000000, stats.syncNs / 1000000, rate / 10, rate % 10, readRate / 10, readRate % 10,
        writeRate / 10, writeRate % 10, wall ? busy * 100 / wall : 0, wall / 1000000000, stats.direct, bounced, bounces,
        perRequest / 10, perRequest % 10, stats.stagedChains, stats.stagedBytes >> 10, stats.verified,
        writeSectors * kSector / 1024, highState, requests ? stats.small * 100 / requests : 0,
        requests ? stats.sequential * 100 / requests : 0, stats.merged, stats.gapChains, stats.gapBytes >> 10,
        stats.maxQueued, stats.commands, stats.cmd23, __atomic_load_n(&stats.interrupts, __ATOMIC_RELAXED), stats.early,
        stats.overlapped, stats.errors, stats.admaErrors, stats.retunes, busMode, busKHz / 1000, busKHz % 1000 / 10,
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
        w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT);  // SDMA (the data path may have left ADMA2 selected)
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

// CMD55 (with the current RCA, 0 before CMD3) then the application command (`data` as cardCommand's).
int NativeIOSwitchBlock::appCommand(uint32_t index, uint32_t arg, uint32_t flags, uint32_t *response, uint32_t data)
{
    uint32_t r55 = 0;
    if (int error = cardCommand(55, rca << 16, RSP_R1, &r55)) return error;
    if (!(r55 & R1_APP_CMD) || (r55 & R1_ERRORS)) return 5;
    return cardCommand(index, arg, flags, response, data);
}

// r14-sdcq: the R2 response cardCommand() just received (CMD2's CID, CMD9's CSD), most significant byte first.
void NativeIOSwitchBlock::readR2(uint8_t out[16])
{
    const uint32_t rep[4] = {r32(RESPONSE), r32(RESPONSE + 4), r32(RESPONSE + 8), r32(RESPONSE + 12)};
    ioswitchSdR2(rep, out);
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

// r14-sdcq: CMD48, `length` bytes of the extension register space at `address` (page * 512 + offset, one page) of
// function `fno`, into `out`. The data block is always 512 bytes, the register's bytes first (5.7.2.1). Setup only.
bool NativeIOSwitchBlock::extRead(uint8_t fno, uint32_t address, uint32_t length, uint8_t *out)
{
    uint32_t r = 0;
    if (!ioswitchSdExtRange(address, length)) return false;
    if (cardCommand(48, ioswitchSdExtArgument(fno, address, length), RSP_R1, &r, kSector) || (r & R1_ERRORS))
        return false;
    memcpy(out, bounce, length);
    return true;
}

// r14-sdcq: who the card is and what it can do, read once at boot in transfer state and logged as two lines (IOLog,
// one-shot): the identity and the verdict first (is it A2, does it offer the command queue), then the raw registers and
// the extension functions. SCR (ACMD51), SD Status (ACMD13) and, when the SCR lists CMD48/49, the General Information
// and the Power Management and Performance Enhancement register sets (CMD48). Read-only: nothing here writes the card.
// A command that fails only leaves its part out. Needs the RCA, so the re-initialized bus (raiseBusSpeed).
void NativeIOSwitchBlock::identify()
{
    if (!rca) {
        IOLog("IOSwitchSD: card identification skipped (inherited bus, RCA unknown)\n");
        return;
    }
    uint32_t r = 0;
    if (!appCommand(51, 0, RSP_R1, &r, sizeof(scrBytes)) && !(r & R1_ERRORS)) {
        memcpy(scrBytes, bounce, sizeof(scrBytes));
        scr = ioswitchSdParseScr(scrBytes);
        haveScr = true;
    }
    if (!appCommand(13, 0, RSP_R1, &r, sizeof(ssrBytes)) && !(r & R1_ERRORS)) {
        memcpy(ssrBytes, bounce, sizeof(ssrBytes));
        sdStatus = ioswitchSdParseStatus(ssrBytes);
        haveSsr = true;
    }
    static uint8_t regs[1024];  // setup() runs once; too big for its stack
    if (!haveScr) {
        extWhy = "no SCR";
    } else if (!(scr.cmdSupport & kSdCmd48)) {
        extWhy = "SCR lists no CMD48/49";
    } else if (!extRead(0, 0, kSector, regs)) {
        extWhy = "general information read failed";
    } else {
        extInfo = ioswitchSdParseGeneralInfo(regs, kSector);
        if (extInfo.why && extInfo.length > kSector && extInfo.length <= sizeof(regs) &&
            extRead(0, kSector, kSector, regs + kSector))
            extInfo = ioswitchSdParseGeneralInfo(regs, sizeof(regs));
        extWhy = extInfo.why;
        if (!extWhy && extInfo.power.found && ioswitchSdExtRange(extInfo.power.address, 2) &&
            extRead(extInfo.power.fno, extInfo.power.address, 2, regs)) {
            power = ioswitchSdParsePower(regs);
            havePower = true;
        }
        // The performance register set is used only when its first 263 bytes (up to the CQ control byte) sit in one page.
        if (!extWhy && extInfo.perf.found) {
            if (!ioswitchSdCqControlAddress(extInfo.perf))
                extWhy = "performance register set crosses a page";
            else if (!extRead(extInfo.perf.fno, extInfo.perf.address, kSdPerfCqControl + 1, regs))
                extWhy = "performance register read failed";
            else {
                perf = ioswitchSdParsePerf(regs);
                havePerf = true;
            }
        }
    }
    const IOSwitchSdCid id = ioswitchSdParseCid(cid);
    const IOSwitchSdCsd size = ioswitchSdParseCsd(csd);
    const char *maker = ioswitchSdMaker(id.mid);
    char makerHex[8];
    snprintf(makerHex, sizeof(makerHex), "0x%02x", id.mid);
    static const char *const kinds[] = {"SDSC", "SDHC/SDXC", "SDUC", "?"};
    const uint8_t queue = havePerf ? perf.cqDepth : haveSsr ? sdStatus.cqDepth : 0;
    const bool a2 = haveSsr && sdStatus.appClass >= 2;
    const char *verdict = a2 && queue ? "A2 with a command queue: queued reads can overlap on the card"
                          : queue     ? "not A2, but it offers a command queue"
                          : a2        ? "A2 by its SD Status, but no command queue in its registers"
                          : haveSsr   ? "not A2 and no command queue: reads stay one command at a time"
                                      : "SD Status unread: A2 and command queue unknown";
    char classes[48] = "SD Status unread";
    if (haveSsr)
        snprintf(classes, sizeof(classes), "class %u U%u V%u A%u", unsigned(sdStatus.speedClass),
                 unsigned(sdStatus.uhsGrade), unsigned(sdStatus.videoClass), unsigned(sdStatus.appClass));
    char line[512];
    snprintf(line, sizeof(line),
             "IOSwitchSD: card %s%s%s%s \"%s\" OEM \"%s\" rev %u.%u #%08x %04u-%02u, %llu GiB %s | SD %u.x, CMD23 %s, "
             "CMD48/49 %s | %s | queue depth %u, cache %s -> %s",
             maker ? maker : makerHex, maker ? " (" : "", maker ? makerHex : "", maker ? ")" : "",
             haveCid ? id.pnm : "?", haveCid ? id.oid : "?", unsigned(id.prv >> 4), unsigned(id.prv & 0xf), id.psn,
             unsigned(id.year), unsigned(id.month), size.bytes >> 30, haveCsd ? kinds[size.structure & 3] : "(no CSD)",
             haveScr ? unsigned(scr.major) : 0u,
             !haveScr                      ? "?"
             : (scr.cmdSupport & kSdCmd23) ? "yes"
                                           : "no",
             !haveScr                      ? "?"
             : (scr.cmdSupport & kSdCmd48) ? "yes"
                                           : "no",
             classes, unsigned(queue),
             havePerf  ? (perf.cache ? "yes" : "no")
             : haveSsr ? (sdStatus.cache ? "yes" : "no")
                       : "?",
             verdict);
    IOLog("%s\n", line);
    // The raw registers (the CID's serial included) and the extension functions as read.
    char hex[4][132];
    const uint8_t *const raw[4] = {cid, csd, scrBytes, ssrBytes};
    static const unsigned lengths[4] = {16, 16, 8, 25};  // SD Status: bits 511..312, every field defined
    for (unsigned k = 0; k < 4; ++k) {
        hex[k][0] = 0;
        for (unsigned i = 0; i < lengths[k]; ++i) snprintf(hex[k] + 2 * i, sizeof(hex[k]) - 2 * i, "%02x", raw[k][i]);
    }
    char ext[256];
    if (extWhy || !extInfo.extensions) {
        snprintf(ext, sizeof(ext), "ext registers: %s", extWhy ? extWhy : "no functions");
    } else {
        snprintf(
            ext, sizeof(ext),
            "ext rev %u len %u, %u functions; power %sfno %u @0x%05x (rev %u, off-notify %u, sustain %u, down %u); "
            "perf %sfno %u @0x%05x (rev %u, FX %u, maint card %u host %u, cache %u on %u, queue %u control 0x%02x, "
            "task errors 0x%02x)",
            unsigned(extInfo.revision), unsigned(extInfo.length), unsigned(extInfo.extensions),
            havePower ? "" : "unread ", unsigned(extInfo.power.fno), extInfo.power.address, unsigned(power.revision),
            unsigned(power.offNotification), unsigned(power.sustenance), unsigned(power.powerDown),
            havePerf ? "" : "unread ", unsigned(extInfo.perf.fno), extInfo.perf.address, unsigned(perf.revision),
            unsigned(perf.fxEvent), unsigned(perf.cardMaintenance), unsigned(perf.hostMaintenance),
            unsigned(perf.cache), unsigned(perf.cacheEnable & 1), unsigned(perf.cqDepth), unsigned(perf.cqControl),
            unsigned(perf.taskErrors));
    }
    IOLog("IOSwitchSD: card registers CID %s CSD %s SCR %s SSR %s | %s\n", haveCid ? hex[0] : "-",
          haveCsd ? hex[1] : "-", haveScr ? hex[2] : "-", haveSsr ? hex[3] : "-", ext);
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
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT);  // as hekate tunes: SDMA selected, never the data path's ADMA2
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
    if (!resume) {
        readR2(cid);
        haveCid = true;
    }
    for (uint64_t ready = nowNs(); !rca;) {
        if ((error = cardCommand(3, 0, RSP_R1, &r))) fail("CMD3", error);
        if ((rca = r >> 16)) break;
        if (nowNs() - ready > 1500000000ull) fail("CMD3 RCA 0", 0);
        IOSleep(1);  // hekate _sd_storage_get_rca: ask again 1 ms later
    }
    // r14-sdcq: the CSD while the card is in stand-by (CMD9 is a class 0 command of that state only). Logged by identify();
    // a failure only leaves it out.
    if (!resume && !cardCommand(9, rca << 16, RSP_R2, nullptr)) {
        readR2(csd);
        haveCsd = true;
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
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) | CONTROL2_V4_64BIT);
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
    dmaSuspend();
    if (irqSource) irqSource->disable();
    powerOffCard();
}

// r10-adma2 power-state hooks. Before sleep (the worker is between commands, so no DMA is in flight): SDMA selected and
// the DMA address cleared, so nothing points the engine at the descriptor table while the controller loses power. After
// wake (powerOnController() reset the controller and set v4 + 64-bit addressing again; reinitialize() and the LBA 1 check
// ran by SDMA): ADMA2 carries data again only if that addressing mode took and the SMMU still leaves SDMMC1 untranslated.
// The table needs nothing more: every ADMA2 command builds it afresh, and command() selects ADMA2 or SDMA per command.
void NativeIOSwitchBlock::dmaSuspend()
{
    w8(HOST_CONTROL, r8(HOST_CONTROL) & ~DMA_SELECT);
    w32(DMA_LOW, 0);
    w32(DMA_HIGH, 0);
    table.reset();
}

void NativeIOSwitchBlock::dmaResume()
{
    if (!adma) return;
    const uint16_t control2 = r16(HOST_CONTROL2);
    const bool translated = mc[0xa94 / 4] & 0x80000000u;
    if ((control2 & CONTROL2_V4_64BIT) == CONTROL2_V4_64BIT && !translated) return;
    adma = false;
    IOLog(
        "IOSwitchSD: ADMA2 off after sleep (HOST_CONTROL2 0x%04x, SMMU %s for SDMMC1); requests go through the bounce "
        "buffer\n",
        control2, translated ? "on" : "off");
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
    dmaResume();
    IOLockLock(queueLock);
    suspended = false;
    IOLockWakeup(queueLock, &head, false);
    IOLockUnlock(queueLock);
}

// setup(): nullptr when ADMA2 may carry the data path, else why not. The controller must offer ADMA2 and keep v4 64-bit
// addressing, and a 1 MiB read through ADMA2 must match `reference` (the same MiB read by SDMA) byte for byte. Its table
// comes from the data path's own builder, over runs chosen to meet what the data path will: full-length descriptors in
// the contiguous bounce buffer, a short one that ends inside a sector and one that completes it, and single kernel pages
// out of physical order, which on this board land above 4 GiB about half the time (RAM is 0x80000000-0x17fffffff). Every
// destination byte starts in DRAM as the complement of what the card should deliver, so a byte the engine skipped or
// misplaced shows. Test pages above 4 GiB that kept their fill while everything else arrived mean the engine drops the
// high address bits: ADMA2 then stays below 4 GiB, as it does when no test page was above 4 GiB. (Such a write cannot
// have reached RAM: 4 GiB lower lies below DRAM at 0x80000000, outside the memory controller's EMEM aperture; pages
// whose alias would fall in IRAM at 0x40000000 are not used.)
const char *NativeIOSwitchBlock::checkAdma(const uint8_t *reference, uint32_t *descriptors, uint32_t *highPages,
                                           uint32_t *highOk, uint32_t *staged)
{
    *descriptors = *highPages = *highOk = *staged = 0;
    if (copyFlush) return "boot-arg ioswitch-sd-copy-flush=1";
    if (!(r32(CAPABILITIES) & (1u << 19))) return "the controller reports no ADMA2";
    if ((r16(HOST_CONTROL2) & CONTROL2_V4_64BIT) != CONTROL2_V4_64BIT)
        return "HOST_CONTROL2 does not keep v4 64-bit addressing";
    tableMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        kAdmaTableBytes, 0x00000000ffffffffull & ~(kAdmaPage - 1));
    if (!tableMemory) return "no memory for the descriptor table";
    table.entries = static_cast<IOSwitchAdmaDescriptor *>(tableMemory->getBytesNoCopy());
    tablePA = tableMemory->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    if (!table.entries || !tablePA || (tablePA & (kAdmaPage - 1)) || tablePA + kAdmaTableBytes > (1ull << 32))
        return "the descriptor table is not page-aligned below 4 GiB";
    table.capacity = kAdmaDescriptors;
    table.limit = kAdmaDmaLimit;

    constexpr uint32_t kTestPages = 128;
    IOBufferMemoryDescriptor *pages =
        IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, kTestPages * kAdmaPage, kAdmaPage);
    if (!pages) return "no memory for the test pages";
    if (pages->prepare() != kIOReturnSuccess) {
        pages->release();
        return "the test pages cannot be wired";
    }
    auto *const pageBytes = static_cast<uint8_t *>(pages->getBytesNoCopy());
    struct Run {
        uint64_t pa;
        uint8_t *at;
        uint32_t length;
    };
    static Run runs[kTestPages + 3];  // setup() runs once; ~3 KiB is too much for its stack
    unsigned count = 0;
    uint32_t streamed = 0;
    auto take = [&](uint64_t pa, uint8_t *at, uint32_t length) {
        runs[count++] = {pa, at, length};
        streamed += length;
    };
    const char *why = nullptr;
    const uint64_t page0 = pages->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    if (pageBytes && page0) {
        take(bouncePA, bounce, 0x2f000);                    // three full-length descriptors and a short one
        take(page0 + 0x40, pageBytes + 0x40, 0x40);         // ends inside a sector...
        take(bouncePA + 0x30000, bounce + 0x30000, 0x1c0);  // ...which this one completes
        for (uint32_t i = kTestPages - 1; i; --i) {
            const uint64_t pa = pages->getPhysicalSegment(i * kAdmaPage, nullptr, kIOMemoryMapperNone);
            if (pa && pa - kAdmaIramAlias >= kAdmaIramBytes) take(pa, pageBytes + i * kAdmaPage, kAdmaPage);
        }
        take(bouncePA + 0x40000, bounce + 0x40000, kBounceBytes - streamed);  // the rest, in the bounce buffer
        table.reset();
        uint32_t at = 0;
        for (unsigned i = 0; i < count && !why; ++i) {
            for (uint32_t k = 0; k < runs[i].length; ++k) runs[i].at[k] = static_cast<uint8_t>(~reference[at + k]);
            if (runs[i].pa >= (1ull << 32)) ++*highPages;
            if (table.add(runs[i].pa, runs[i].length, cacheableRam)) why = "the builder refused the test memory";
            at += runs[i].length;
        }
        if (!why && !table.finish()) why = "no descriptors";
    } else {
        why = "the test pages have no physical address";
    }
    if (!why) {
        // As the data path's ADMA2 commands, without run()'s retry and re-tune: a failure here only keeps the bounce
        // buffer.
        *descriptors = table.count;
        for (uintptr_t at = (uintptr_t)table.entries, end = at + table.count * sizeof(IOSwitchAdmaDescriptor); at < end;
             at += kAdmaLine)
            __asm__ volatile("dc cvac, %0" ::"r"(at) : "memory");
        syncLines<1>(table);
        barrier();
        uint32_t status = 0;
        const int step = command(static_cast<uint32_t>(firstSector), kBounceSectors, false, table.count, &status);
        syncLines<2>(table);
        barrier();
        if (step) {
            snprintf(admaWhy, sizeof(admaWhy), "the read failed: step %d, STATUS 0x%08x, ADMA state 0x%02x at 0x%llx",
                     step, status, admaErrorState, admaErrorAddress);
            why = admaWhy;
            recover(true);
        }
    }
    unsigned highMissing = 0;
    for (uint32_t i = 0, at = 0; !why && i < count; at += runs[i++].length) {
        if (!memcmp(runs[i].at, reference + at, runs[i].length)) {
            if (runs[i].pa >= (1ull << 32)) ++*highOk;
            continue;
        }
        bool untouched = runs[i].pa >= (1ull << 32);
        for (uint32_t k = 0; untouched && k < runs[i].length; ++k)
            untouched = runs[i].at[k] == static_cast<uint8_t>(~reference[at + k]);
        if (untouched)
            ++highMissing;
        else
            why = "the 1 MiB read differs from SDMA's";
    }
    // r12-sdio: the read path's per-segment staging (IOSwitchAdmaStaging) once on the hardware, before any request uses it:
    // the partition's first half MiB into the test pages, every page whose number is a multiple of 3 refused and so staged
    // through the bounce buffer, then copied out as the data path does; all of it must match SDMA's read. A failure leaves
    // staging off (a read chain with a refused segment then bounces whole, as before), with the reason on the ADMA2 line.
    *staged = 0;
    stages = why ? nullptr : static_cast<Staging::Stage *>(IOMalloc(kStages * sizeof(Staging::Stage)));
    const char *stagingWhy = why ? "" : !stages ? "no memory for the stages" : nullptr;
    if (!stagingWhy) {
        constexpr uint32_t kTestBytes = kTestPages * kAdmaPage;
        for (uint32_t k = 0; k < kTestBytes; ++k) pageBytes[k] = static_cast<uint8_t>(~reference[k]);
        staging.reset(&table, bouncePA, stages, kStages);
        table.limit = kAdma4GiB;
        const auto refuseEveryThird = [](uint64_t page) { return cacheableRam(page) && atop_64(page) % 3; };
        if (staging.addMemory(pages, kTestBytes, refuseEveryThird) || !staging.finish() || table.bytes != kTestBytes ||
            !staging.count || staging.staged == kTestBytes) {
            stagingWhy = "the builder refused the test pages";
        } else {
            *staged = static_cast<uint32_t>(staging.staged / kAdmaPage);
            prepareDirect(false, false);
            uint32_t status = 0;
            const int step =
                command(static_cast<uint32_t>(firstSector), kTestBytes / kSector, false, table.count, &status);
            finishDirect(false);
            if (step) {
                recover(true);
                stagingWhy = "its read failed";
            } else if (!copyStaged()) {
                stagingWhy = "a staged copy failed";
            } else if (memcmp(pageBytes, reference, kTestBytes)) {
                stagingWhy = "its read differs from SDMA's";
            }
        }
        staging.count = 0;
        table.reset();
    }
    stagingOn = !stagingWhy;
    if (stagingWhy && *stagingWhy)
        snprintf(stagingReason, sizeof(stagingReason), ", per-segment staging off (%s)", stagingWhy);
    pages->complete();
    pages->release();
    // r12-sdio: test pages above 4 GiB that matched prove the high address word; ones that kept their fill disprove it;
    // with none up there (the usual case at boot) the data path's first read chains that reach there prove it (high).
    high.state = highMissing ? IOSwitchAdmaHigh::kOff : *highPages ? IOSwitchAdmaHigh::kOn : IOSwitchAdmaHigh::kProving;
    return why;
}

// r14-sdcq: setup(): nullptr when multi-block ADMA2 commands may announce their length with Auto CMD23 (SET_BLOCK_COUNT)
// instead of stopping with Auto CMD12, else why not; and what one 4 KiB read took each way (the card's access time is
// most of it; the difference is the stop command's cost, or CMD23's). kCmd23Pairs 4 KiB reads (the size two-thirds of
// requests have) at blocks spread over the APFS partition, each read once with CMD12 and once with CMD23 by ADMA2 into
// the bounce buffer's first two pages, which start as 0xff and 0x00 so a read that delivered nothing differs. The order
// alternates so the card's own read cache favours neither. Every pair must match byte for byte; an error or a difference
// keeps CMD12, with the reason. Runs once in setup() after checkAdma (it uses the same table and syncs).
const char *NativeIOSwitchBlock::checkCmd23(uint64_t *withNs, uint64_t *withoutNs)
{
    constexpr uint32_t kSectors = 8, kBytes = kSectors * kSector;
    *withNs = *withoutNs = 0;
    const uint64_t blocks = sectorCount / kSectors;
    if (blocks < kCmd23Pairs) return "the partition is too small";
    const char *why = nullptr;
    for (uint32_t i = 0; i < kCmd23Pairs && !why; ++i) {
        // A multiplicative hash spreads the reads over the whole partition, 4 KiB-aligned.
        const uint64_t lba = firstSector + (static_cast<uint64_t>(i + 1) * 2654435761ull % blocks) * kSectors;
        for (unsigned pass = 0; pass < 2 && !why; ++pass) {
            const bool with = (pass == 0) == ((i & 1) != 0);
            const uint32_t at = with ? kBytes : 0;
            memset(bounce + at, with ? 0x00 : 0xff, kBytes);
            table.reset();
            if (table.add(bouncePA + at, kBytes, cacheableRam) || !table.finish()) {
                why = "the builder refused the bounce buffer";
                break;
            }
            prepareDirect(false, false);
            uint32_t status = 0;
            const uint64_t began = nowNs();
            const int step = command(static_cast<uint32_t>(lba), kSectors, false, table.count, &status, with);
            (with ? *withNs : *withoutNs) += nowNs() - began;
            finishDirect(false);
            if (step) {
                recover(true);
                snprintf(cmd23Why, sizeof(cmd23Why), "read %u with %s failed: step %d, STATUS 0x%08x", i,
                         with ? "CMD23" : "CMD12", step, status);
                why = cmd23Why;
            }
        }
        if (!why && memcmp(bounce, bounce + kBytes, kBytes)) {
            uint32_t at = 0;
            while (bounce[at] == bounce[kBytes + at]) ++at;
            snprintf(cmd23Why, sizeof(cmd23Why), "read %u (lba %llu) differs from CMD12's at byte %u", i, lba, at);
            why = cmd23Why;
        }
    }
    table.reset();
    return why;
}

bool NativeIOSwitchBlock::setup()
{
    regs = reinterpret_cast<volatile uint8_t *>(ml_io_map(kControllerPA, 0x1000));
    mc = reinterpret_cast<volatile uint32_t *>(ml_io_map(kMemoryControllerPA, 0x1000));
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
    w16(HOST_CONTROL2, r16(HOST_CONTROL2) | CONTROL2_V4_64BIT);
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
    unsigned flushing = 0;
    PE_parse_boot_argn("ioswitch-sd-copy-flush", &flushing, sizeof(flushing));
    copyFlush = flushing != 0;
    // What a scatter-gather (ADMA2) path straight into the requests' pages would rest on: CAPABILITIES bit 19 (ADMA2),
    // 28 (64-bit addressing, v3) and 27 (64-bit addressing, v4). The log only, once.
    const uint32_t caps = r32(CAPABILITIES), capsHi = r32(CAPABILITIES_HI);
    os_log(OS_LOG_DEFAULT, "IOSwitchSD: host version %#06x caps %#010x %#010x (ADMA2 %u, 64-bit v3 %u v4 %u), copy %s",
           r16(VERSION), caps, capsHi, (caps >> 19) & 1, (caps >> 28) & 1, (caps >> 27) & 1,
           copyFlush ? "readBytes/writeBytes (flushing)" : "copypv (no flush)");

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
    identify();
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

    // DMA for the data path. Both checks read the partition's first MiB and compare it with the same MiB read as four
    // 256 KiB SDMA commands (which never reach an SDMA boundary). ADMA2 straight to the requests' pages once checkAdma()
    // passes; then reads go up to 1 MiB and writes up to kDirectWriteSectors per command, and the bounce fallback splits
    // longer chains into 256 KiB SDMA commands. Otherwise SDMA through the bounce buffer as before: commands over 512 KiB
    // cross the SDMA boundary (command()), so one 1 MiB SDMA read must match too before reads may be that long. It goes
    // straight to command() so that a failure is not retried and re-tuned (transfer() would step the clock down); a
    // failure keeps 256 KiB, with one line.
    if (sectorCount >= kBounceSectors) {
        uint8_t *reference = static_cast<uint8_t *>(IOMallocData(kBounceBytes));
        int step = reference ? 0 : -2;
        for (uint32_t done = 0; !step && done < kBounceSectors; done += kSafeSectors)
            step = transfer(firstSector + done, kSafeSectors, false, reference + done * kSector);
        uint32_t descriptors = 0, highPages = 0, highOk = 0, staged = 0;
        const char *why =
            step ? "the reference read failed" : checkAdma(reference, &descriptors, &highPages, &highOk, &staged);
        if (!why) {
            adma = true;
            maxSectors = kBounceSectors;
            maxWriteSectors = kDirectWriteSectors;
            checkBuffer = static_cast<uint8_t *>(IOMallocData(kBounceBytes));
            writeChecks = checkBuffer ? kWriteChecks : 0;
            largeWriteChecks = checkBuffer ? kLargeWriteChecks : 0;
            admaWrites = checkBuffer != nullptr;
            // Writes over 256 KiB only by ADMA2 with their read-backs; without the buffer for those, 256 KiB commands.
            writeSectors = admaWrites ? kDirectWriteSectors : kSafeSectors;
            // Without the buffer nothing above 4 GiB can be proven: it stays below.
            if (!checkBuffer && high.state == IOSwitchAdmaHigh::kProving) high.refuse();
            static const char *const states[] = {" (above 4 GiB: proven by the first reads that reach there)", "",
                                                 " (ADMA2 stays below 4 GiB)"};
            if (stagingOn)
                snprintf(stagingReason, sizeof(stagingReason), ", per-segment staging on (%u pages)", staged);
            IOLog("IOSwitchSD: ADMA2 on: 1 MiB read through %u descriptors matches SDMA, %u of %u test pages above "
                  "4 GiB right%s; commands up to %u KiB read, %u KiB write%s%s\n",
                  descriptors, highOk, highPages, states[high.state], maxSectors * kSector / 1024,
                  maxWriteSectors * kSector / 1024,
                  admaWrites ? ", the first writes read back" : ", writes through the bounce buffer", stagingReason);
            // r14-sdcq: Auto CMD23 when the card's SCR lists CMD23 and both framings read the same bytes.
            if (!haveScr || !(scr.cmdSupport & kSdCmd23)) {
                IOLog("IOSwitchSD: CMD23 not used (%s); multi-block commands stop by CMD12\n",
                      haveScr ? "the card's SCR does not list it" : "no SCR");
            } else {
                uint64_t withNs = 0, withoutNs = 0;
                if (const char *cmd23Off = checkCmd23(&withNs, &withoutNs)) {
                    IOLog("IOSwitchSD: CMD23 off (%s); multi-block commands stop by CMD12\n", cmd23Off);
                } else {
                    cmd23Reads = true;
                    cmd23Writes = admaWrites;
                    cmd23WriteChecks = cmd23Writes ? kCmd23WriteChecks : 0;
                    const uint64_t with = withNs / kCmd23Pairs, without = withoutNs / kCmd23Pairs;
                    IOLog("IOSwitchSD: CMD23 on: %u 4 KiB reads match CMD12's; %llu.%llu us per read with CMD23, "
                          "%llu.%llu us with CMD12 (%lld ns saved per command); multi-block reads%s announce their "
                          "length%s\n",
                          kCmd23Pairs, with / 1000, with / 100 % 10, without / 1000, without / 100 % 10,
                          static_cast<long long>(without) - static_cast<long long>(with),
                          cmd23Writes ? " and writes" : "", cmd23Writes ? ", the first CMD23 writes read back" : "");
                }
            }
            // r14-sdcq: gap reads need the sink and, for their proofs, checkBuffer; queue merging must be on.
            sinkMemory = checkBuffer && merge ? IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                                                    kernel_task,
                                                    static_cast<IOOptionBits>(kIODirectionInOut) |
                                                        static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
                                                    kGapSectors * kSector, 0x00000000ffffffffull & ~(kAdmaPage - 1))
                                              : nullptr;
            if (sinkMemory) {
                sinkPA = sinkMemory->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
                auto *sinkBytes = static_cast<uint8_t *>(sinkMemory->getBytesNoCopy());
                if (!sinkBytes || !sinkPA || (sinkPA & (kAdmaPage - 1)) ||
                    sinkPA + kGapSectors * kSector > (1ull << 32) || !cacheableRam(sinkPA)) {
                    sinkPA = 0;
                } else {
                    // No line of the sink is dirty in the cache from here on: the engine's writes there are never
                    // overwritten by an eviction, and nobody reads them.
                    for (uintptr_t at = (uintptr_t)sinkBytes, end = at + kGapSectors * kSector; at < end;
                         at += kAdmaLine)
                        __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
                    barrier();
                }
            }
            gapsOn = sinkPA != 0;
            gapProofs = gapsOn ? kGapProofs : 0;
            if (gapsOn)
                IOLog("IOSwitchSD: gap reads on: a read chain takes queued reads up to %u KiB beyond it (%u KiB per "
                      "command), the card reading through the gap; the first %u such chains are compared with SDMA\n",
                      kGapSectors * kSector / 1024, kGapBudget * kSector / 1024, kGapProofs);
            else
                IOLog("IOSwitchSD: gap reads off (%s)\n", !checkBuffer ? "no buffer for their proofs"
                                                          : !merge     ? "boot-arg ioswitch-sd-merge=0"
                                                                       : "no sink page below 4 GiB");
        } else {
            IOLog("IOSwitchSD: ADMA2 off (%s); every request through the bounce buffer\n", why);
        }
        if (!step && !adma) {
            for (uintptr_t at = (uintptr_t)bounce, end = at + kBounceBytes; at < end; at += 64)
                __asm__ volatile("dc civac, %0" ::"r"(at) : "memory");
            barrier();
            uint32_t status = 0;
            step = command(static_cast<uint32_t>(firstSector), kBounceSectors, false, 0, &status);
            if (step) {
                recover(true);
            } else {
                for (uintptr_t at = (uintptr_t)bounce, end = at + kBounceBytes; at < end; at += 64)
                    __asm__ volatile("dc ivac, %0" ::"r"(at) : "memory");
                barrier();
            }
            const bool same = !step && !memcmp(reference, bounce, kBounceBytes);
            if (same)
                maxSectors = sdmaReadSectors = kBounceSectors;
            else
                IOLog("IOSwitchSD: 1 MiB commands failed their check (step %d, %s); keeping 256 KiB\n", step,
                      step ? "error" : "data differs");
        }
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
