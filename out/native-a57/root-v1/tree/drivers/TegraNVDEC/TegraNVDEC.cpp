// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 NVDEC (video decoder 2.0, a falcon behind host1x): bring-up, the job engine and sleep. tegra_nvdec_probe()
// is inert unless the boot-arg switch-nvdec is non-zero (stages in TegraNVDEC.h); it then runs on its own thread:
//   N0  host1x up (drivers/TegraHost1x: clock adopted, syncpoint interrupt, channel 0, self-test)
//   N1  power partition, clock, reset, falcon firmware loaded and started
//   N2  proof of life through host1x channel 0 in NVDEC's class: an IMMEDIATE syncpoint increment (the engine's host
//       interface answers) then an OP_DONE one (the falcon firmware answers), each completed by interrupt
//   N3  (stage 2) an embedded H.264 picture decoded and checked (TegraNVDECSelfTest.cpp)
//   N4  (stage 3) the TegraNVDEC service and its user client published (TegraNVDECUserClient.cpp)
// One line per stage ("TegraNVDEC: N1 ok ..."), or the failing stage's FAIL line and nothing after it.
//
// Jobs (tegra_nvdec_run): host1x channel 0 in NVDEC's class, each method written through the engine's THI
// METHOD0/METHOD1 pair (averne's av_nvtegra_cmdbuf_push_value; L4T nvhost), the picture setup from a kernel-only slot,
// then an OP_DONE increment of NVDEC's syncpoint: the job's fence. At most 8 jobs are queued, which bounds both the
// push buffer and the 16 setup slots.
//
// Firmware: the non-secure falcon image L4T loads as nvhost_nvdec020_ns.fw when CONFIG_NVDEC_BOOTLOADER is off
// (switchroot's tegra_linux_defconfig), here linux-firmware's nvidia/tegra210/nvdec.bin (firmware/SOURCE). The secure
// path (nvhost_nvdec_bl020_prod.fw + nvhost_nvdec020_prod.fw, WPR carveout set up by the TSEC) is not used.
//
// Sources (Linux's drivers are GPL-2.0-only, so this follows them without copying):
//   tegra210.dtsi nvdec@54480000 (CAR clock and reset 194, power domain "nvdec" = PMC partition 25), Linux
//   soc/tegra/pmc.c tegra_powergate_power_up (reset asserted, partition on, clocks, clamps off, reset released, MBIST
//   workaround), clk-tegra210.c tegra210_pg_mbist_war[NVDEC] (NVJPG clock on, LVL2_CLK_GATE_OVRE bits 9 and 31
//   pulsed), clk-tegra-periph.c (CLK_SOURCE_NVDEC 0x698, PLLP is mux code 4), memory/tegra/tegra210.c (MC SMMU NVDEC
//   ASID 0xab4, client hot reset NVDEC 0x970 bit 5), drm/tegra/falcon.c falcon_boot (scrub wait, DMATRFBASE = OS data,
//   data then code in 256-byte DMA chunks, IRQMSET/IRQDEST, ITFEN, BOOTVEC 0, STARTCPU) and nvdec.c (tegra210: no
//   stream IDs, no RISC-V, firmware nvidia/tegra210/nvdec.bin, embedded here by firmware.S). L4T nvhost t210.c,
//   nvdec.c and flcn.c agree on the order; DMA context 0 as L4T uses it on T210 (no context isolation there).
//   Clock: PLLP undivided, 408 MHz, below the lowest T210B01 NVDEC DVFS point (460.8 MHz at minimum core voltage,
//   tegra210b01-core-dvfs.c), so it is safe at any core voltage the boot chain left.
// Waits on hardware sequencing with no interrupt of its own (partition status, IMEM/DMEM scrub, the falcon's DMA
// queue) are bounded reads, as in drivers/SwitchUSB and nouveau; every completion of work is a host1x syncpoint
// interrupt (TegraHost1x), and a lost one panics there.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <string.h>
#include "../TegraHost1x/TegraHost1x.h"
#include "TegraNVDEC.h"
#include "TegraNVDECCheck.h"

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
boolean_t PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
extern const uint8_t tegra_nvdec_fw_start[] __asm__("_tegra_nvdec_fw_start");
extern const uint8_t tegra_nvdec_fw_end[] __asm__("_tegra_nvdec_fw_end");
}

namespace {
constexpr vm_offset_t kNvdecPA = 0x54480000, kNvdecSize = 0x40000;
constexpr uint32_t kThiMethod0 = 0x40;  // THI METHOD0 (offset) then METHOD1 (data), averne nvtegra_host1x.h
constexpr vm_offset_t kCarPA = 0x60006000, kPmcPagePA = 0x7000E000, kMcPA = 0x70019000;
constexpr uint32_t kPmcOffset = 0x400, kPartition = 25;

enum : uint32_t {  // clock and reset controller
    CLK_OUT_ENB_Y = 0x298,
    CLK_ENB_Y_SET = 0x29c,
    CLK_ENB_Y_CLR = 0x2a0,
    RST_DEVICES_Y = 0x2a4,
    RST_DEV_Y_SET = 0x2a8,
    RST_DEV_Y_CLR = 0x2ac,
    LVL2_CLK_GATE_OVRE = 0x554,
    CLK_SOURCE_NVDEC = 0x698,
    Y_NVDEC = 1u << 2,
    Y_NVJPG = 1u << 3,
    OVRE_NVDEC_MBIST = (1u << 9) | (1u << 31),
    SOURCE_PLLP = 4u << 29,
    NVDEC_DIV_1 = 0,  // 7.1 divider: 408 MHz * 2 / (0 + 2) = 408 MHz
};
enum : uint32_t { PWRGATE_TOGGLE = 0x30, REMOVE_CLAMPING = 0x34, PWRGATE_STATUS = 0x38, TOGGLE_START = 1u << 8 };
enum : uint32_t {
    MC_HOTRESET_CTRL_1 = 0x970,
    MC_HOTRESET_NVDEC = 1u << 5,
    MC_SMMU_NVDEC_ASID = 0xab4,
    SMMU_ENABLE = 1u << 31,
};
enum : uint32_t {  // falcon (drm/tegra/falcon.h), relative to the engine
    FALCON_IRQSTAT = 0x1008,
    FALCON_IRQMSET = 0x1010,
    FALCON_IRQDEST = 0x101c,
    FALCON_MAILBOX0 = 0x1040,
    FALCON_ITFEN = 0x1048,
    FALCON_IDLESTATE = 0x104c,
    FALCON_DEBUGINFO = 0x1094,
    FALCON_CPUCTL = 0x1100,
    FALCON_BOOTVEC = 0x1104,
    FALCON_DMACTL = 0x110c,
    FALCON_DMATRFBASE = 0x1110,
    FALCON_DMATRFMOFFS = 0x1114,
    FALCON_DMATRFCMD = 0x1118,
    FALCON_DMATRFFBOFFS = 0x111c,
    CPUCTL_STARTCPU = 1u << 1,
    DMACTL_SCRUBBING = (1u << 1) | (1u << 2),
    DMATRFCMD_FULL = 1u << 0,
    DMATRFCMD_IDLE = 1u << 1,
    DMATRFCMD_IMEM = 1u << 4,
    DMATRFCMD_SIZE_256B = 6u << 8,
    ITFEN_CTXEN = 1u << 0,
    ITFEN_MTHDEN = 1u << 1,
    // IRQMSET: WDTMR, HALT, EXTERR, SWGEN0, SWGEN1, EXT(0xff); IRQDEST the same without WDTMR (falcon_boot).
    IRQMSET_BOOT = (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (0xffu << 8),
    IRQDEST_BOOT = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (0xffu << 8),
};

struct Image {  // falcon bin header v1 and its OS header (drm/tegra/falcon.h)
    uint32_t size, binData, binSize, code, codeSize, data, dataSize, apps;
};

volatile uint8_t *gNvdec, *gCar, *gPmc, *gMc;
IOBufferMemoryDescriptor *gFirmware;  // stays: the falcon OS loads its apps from it through DMATRFBASE
uint64_t gFirmwarePA;
Image gImage;
char gLine[256];

// The job engine (tegra_nvdec_run / tegra_nvdec_wait / sleep).
constexpr uint32_t kInFlight = 8, kSetupSlots = 16, kSetupSlotBytes = 1024, kJobTimeoutUs = 1000000;
IOLock *gEngineLock;
uint32_t gSyncpt, gBase, gIssued;  // job n's fence is gBase + n
bool gUp, gWedged, gSuspended;
TegraNVDECBlock gSetup;  // kSetupSlots picture setups, never mapped into a client

inline uint32_t r32(volatile uint8_t *base, uint32_t o)
{
    return *(volatile uint32_t *)(base + o);
}
inline void w32(volatile uint8_t *base, uint32_t o, uint32_t v)
{
    *(volatile uint32_t *)(base + o) = v;
}
inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool waitBits(volatile uint8_t *base, uint32_t o, uint32_t mask, uint32_t value, uint32_t us)
{
    for (uint32_t i = 0; i <= us; ++i) {
        if ((r32(base, o) & mask) == value) return true;
        IODelay(1);
    }
    return false;
}

uint64_t nowUs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

void cleanToPoC(const void *va, size_t size)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = (uintptr_t)4 << ((ctr >> 16) & 15);
    for (uintptr_t a = (uintptr_t)va & ~(line - 1), end = (uintptr_t)va + size; a < end; a += line)
        __asm__ volatile("dc cvac, %0" ::"r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

const char *falconState()
{
    static char text[160];
    snprintf(text, sizeof(text), "falcon CPUCTL %#x IDLESTATE %#x IRQSTAT %#x MAILBOX0 %#x DEBUGINFO %#x",
             r32(gNvdec, FALCON_CPUCTL), r32(gNvdec, FALCON_IDLESTATE), r32(gNvdec, FALCON_IRQSTAT),
             r32(gNvdec, FALCON_MAILBOX0), r32(gNvdec, FALCON_DEBUGINFO));
    return text;
}

// falcon_parse_firmware_image: magic 0x10de (or 0x10fe), version 1, the OS header and data inside the image.
bool parse(const uint8_t *fw, size_t length, Image *image)
{
    if (length < 0x100 || (le32(fw) != 0x10de && le32(fw) != 0x10fe) || le32(fw + 4) != 1) return false;
    image->size = le32(fw + 8);
    const uint32_t os = le32(fw + 12);
    image->binData = le32(fw + 16);
    image->binSize = le32(fw + 20);
    if (image->size > length || os > image->size - 20 || image->binData > image->size ||
        image->binSize > image->size - image->binData)
        return false;
    image->code = le32(fw + os);
    image->codeSize = le32(fw + os + 4);
    image->data = le32(fw + os + 8);
    image->dataSize = le32(fw + os + 12);
    image->apps = le32(fw + os + 16);
    return image->codeSize && image->code <= image->binSize && image->codeSize <= image->binSize - image->code &&
           image->data <= image->binSize && image->dataSize <= image->binSize - image->data;
}

// tegra_powergate_power_up with the T210 MBIST workaround. A partition the boot chain left on keeps its state; the
// engine is still reset and re-clocked so the falcon starts from scratch.
bool powerUp(bool *wasOn)
{
    if (r32(gMc, MC_SMMU_NVDEC_ASID) & SMMU_ENABLE) {
        snprintf(gLine, sizeof(gLine), "N1 FAIL SMMU translation is on for NVDEC (MC 0xab4 = %#x)",
                 r32(gMc, MC_SMMU_NVDEC_ASID));
        return false;
    }
    *wasOn = (r32(gPmc, kPmcOffset + PWRGATE_STATUS) >> kPartition) & 1;
    w32(gCar, RST_DEV_Y_SET, Y_NVDEC);
    IODelay(10);
    if (!*wasOn) {
        if (!waitBits(gPmc, kPmcOffset + PWRGATE_TOGGLE, TOGGLE_START, 0, 5000)) {
            snprintf(gLine, sizeof(gLine), "N1 FAIL PMC toggle busy (PWRGATE_TOGGLE %#x)",
                     r32(gPmc, kPmcOffset + PWRGATE_TOGGLE));
            return false;
        }
        w32(gPmc, kPmcOffset + PWRGATE_TOGGLE, kPartition | TOGGLE_START);
        if (!waitBits(gPmc, kPmcOffset + PWRGATE_STATUS, 1u << kPartition, 1u << kPartition, 5000)) {
            snprintf(gLine, sizeof(gLine), "N1 FAIL partition %u did not power on (PWRGATE_STATUS %#x)", kPartition,
                     r32(gPmc, kPmcOffset + PWRGATE_STATUS));
            return false;
        }
        IODelay(10);
    }
    w32(gCar, CLK_SOURCE_NVDEC, SOURCE_PLLP | NVDEC_DIV_1);
    IODelay(2);
    w32(gCar, CLK_ENB_Y_SET, Y_NVDEC);
    IODelay(10);
    w32(gPmc, kPmcOffset + REMOVE_CLAMPING, 1u << kPartition);
    IODelay(10);
    if (r32(gMc, MC_HOTRESET_CTRL_1) & MC_HOTRESET_NVDEC)
        w32(gMc, MC_HOTRESET_CTRL_1, r32(gMc, MC_HOTRESET_CTRL_1) & ~MC_HOTRESET_NVDEC);
    w32(gCar, RST_DEV_Y_CLR, Y_NVDEC);
    IODelay(10);
    // tegra210_generic_mbist_war with NVDEC's slcg clock (NVJPG's) running for the pulse.
    const bool nvjpgOn = r32(gCar, CLK_OUT_ENB_Y) & Y_NVJPG;
    if (!nvjpgOn) w32(gCar, CLK_ENB_Y_SET, Y_NVJPG);
    const uint32_t ovre = r32(gCar, LVL2_CLK_GATE_OVRE);
    w32(gCar, LVL2_CLK_GATE_OVRE, ovre | OVRE_NVDEC_MBIST);
    IODelay(1);
    w32(gCar, LVL2_CLK_GATE_OVRE, ovre);
    IODelay(1);
    if (!nvjpgOn) w32(gCar, CLK_ENB_Y_CLR, Y_NVJPG);
    if (!(r32(gCar, CLK_OUT_ENB_Y) & Y_NVDEC) || (r32(gCar, RST_DEVICES_Y) & Y_NVDEC)) {
        snprintf(gLine, sizeof(gLine), "N1 FAIL clock or reset stuck (CLK_OUT_ENB_Y %#x RST_DEVICES_Y %#x)",
                 r32(gCar, CLK_OUT_ENB_Y), r32(gCar, RST_DEVICES_Y));
        return false;
    }
    return true;
}

// The whole image goes into one contiguous buffer below 4 GiB, cleaned to DRAM: the falcon fetches it by physical
// address (SMMU bypassed), now for its OS and later for the apps.
bool loadImage()
{
    const size_t length = (size_t)(tegra_nvdec_fw_end - tegra_nvdec_fw_start);
    if (!parse(tegra_nvdec_fw_start, length, &gImage)) {
        snprintf(gLine, sizeof(gLine), "N1 FAIL embedded firmware is not a falcon v1 image (%lu bytes)",
                 (unsigned long)length);
        return false;
    }
    if (!gFirmware) {  // kept for resume: the falcon is reloaded from it after SC7
        gFirmware = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                                                                     (IOOptionBits)kIODirectionInOut |
                                                                         (IOOptionBits)kIOMemoryPhysicallyContiguous |
                                                                         (IOOptionBits)kIOMemoryMapperNone,
                                                                     gImage.size, 0xfffff000ULL);
        IOByteCount contiguous = 0;
        if (gFirmware && gFirmware->prepare() == kIOReturnSuccess)
            gFirmwarePA = gFirmware->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
        if (!gFirmwarePA || contiguous < gImage.size || gFirmwarePA + gImage.size > (1ULL << 32)) {
            snprintf(gLine, sizeof(gLine), "N1 FAIL no contiguous %u-byte firmware buffer below 4 GiB", gImage.size);
            gFirmwarePA = 0;
            return false;
        }
        memcpy(gFirmware->getBytesNoCopy(), tegra_nvdec_fw_start, gImage.size);
        cleanToPoC(gFirmware->getBytesNoCopy(), gImage.size);
    }
    w32(gNvdec, FALCON_DMATRFBASE, (uint32_t)((gFirmwarePA + gImage.binData) >> 8));
    return true;
}

bool copyChunk(uint32_t from, uint32_t to, bool code)
{
    if (!waitBits(gNvdec, FALCON_DMATRFCMD, DMATRFCMD_FULL, 0, 100000)) return false;
    w32(gNvdec, FALCON_DMATRFMOFFS, to);
    w32(gNvdec, FALCON_DMATRFFBOFFS, from);
    w32(gNvdec, FALCON_DMATRFCMD, DMATRFCMD_SIZE_256B | (code ? DMATRFCMD_IMEM : 0));
    return true;
}

// falcon_boot, minus its final idle poll: that the firmware runs is shown by N2's OP_DONE increment instead.
bool bootFalcon()
{
    if (!waitBits(gNvdec, FALCON_DMACTL, DMACTL_SCRUBBING, 0, 10000)) {
        snprintf(gLine, sizeof(gLine), "N1 FAIL IMEM/DMEM scrubbing did not end (DMACTL %#x)",
                 r32(gNvdec, FALCON_DMACTL));
        return false;
    }
    w32(gNvdec, FALCON_DMACTL, 0);
    if (!loadImage()) return false;
    bool queued = true;
    for (uint32_t offset = 0; queued && offset < gImage.dataSize; offset += 256)
        queued = copyChunk(gImage.data + offset, offset, false);
    for (uint32_t offset = 0; queued && offset < gImage.codeSize; offset += 256)
        queued = copyChunk(gImage.code + offset, offset, true);
    if (!queued || !waitBits(gNvdec, FALCON_DMATRFCMD, DMATRFCMD_IDLE, DMATRFCMD_IDLE, 100000)) {
        snprintf(gLine, sizeof(gLine), "N1 FAIL firmware DMA did not finish (DMATRFCMD %#x)",
                 r32(gNvdec, FALCON_DMATRFCMD));
        return false;
    }
    w32(gNvdec, FALCON_IRQMSET, IRQMSET_BOOT);
    w32(gNvdec, FALCON_IRQDEST, IRQDEST_BOOT);
    w32(gNvdec, FALCON_ITFEN, ITFEN_MTHDEN | ITFEN_CTXEN);
    w32(gNvdec, FALCON_BOOTVEC, 0);
    w32(gNvdec, FALCON_CPUCTL, CPUCTL_STARTCPU);
    return true;
}

// Channel 0 in NVDEC's class: an IMMEDIATE increment is done by the engine's host interface on arrival, an OP_DONE
// one only once the falcon reports the methods before it done.
bool proofOfLife(uint64_t *us, uint32_t *id, uint32_t *from)
{
    if (tegra_host1x_syncpt_alloc(id)) {
        snprintf(gLine, sizeof(gLine), "N2 FAIL no syncpoint left");
        return false;
    }
    *from = tegra_host1x_syncpt_read(*id);
    const uint32_t stream[] = {
        tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, *id),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, *id),
    };
    const uint64_t began = nowUs();
    int ret = tegra_host1x_submit(stream, 3);
    const char *stage = "submit";
    if (!ret) {
        stage = "host interface (IMMEDIATE)";
        ret = tegra_host1x_syncpt_wait(*id, *from + 1, 100000);
    }
    if (!ret) {
        stage = "falcon (OP_DONE)";
        ret = tegra_host1x_syncpt_wait(*id, *from + 2, 500000);
    }
    *us = nowUs() - began;
    if (ret) {
        char host[200];
        tegra_host1x_describe(host, sizeof(host));
        snprintf(gLine, sizeof(gLine), "N2 FAIL %s did not increment syncpoint %u (%u->%u, %d); %s; %s", stage, *id,
                 *from, tegra_host1x_syncpt_read(*id), ret, falconState(), host);
        return false;
    }
    return true;
}

void engineSuspend();
void engineResume();

void wedge(uint32_t fence)
{
    if (gWedged) return;
    gWedged = true;
    char host[200];
    tegra_host1x_describe(host, sizeof(host));
    IOLog(
        "TegraNVDEC: job fence %u not reached in %u ms (syncpoint %u at %u); NVDEC refuses work until resume; %s; %s\n",
        fence, kJobTimeoutUs / 1000, gSyncpt, tegra_host1x_syncpt_read(gSyncpt), falconState(), host);
}

void bringUp(void *stageArg, wait_result_t)
{
    const uint32_t stage = (uint32_t)(uintptr_t)stageArg;
    // The arena first, while physical memory is least fragmented.
    uint32_t mib = 48;
    PE_parse_boot_argn("switch-nvdec-mib", &mib, sizeof(mib));
    if (stage >= 2 && tegra_nvdec_arena_init((uint64_t)(mib < 4 ? 4 : mib > 512 ? 512 : mib) << 20)) {
        IOLog("TegraNVDEC: N0 FAIL no contiguous %u MiB arena below 4 GiB (switch-nvdec-mib)\n", mib);
        return;
    }
    if (tegra_host1x_start()) {
        IOLog("TegraNVDEC: N0 FAIL host1x is not up (see the TegraHost1x line)\n");
        return;
    }
    gNvdec = (volatile uint8_t *)ml_io_map(kNvdecPA, kNvdecSize);
    gCar = (volatile uint8_t *)ml_io_map(kCarPA, 0x1000);
    gPmc = (volatile uint8_t *)ml_io_map(kPmcPagePA, 0x1000);
    gMc = (volatile uint8_t *)ml_io_map(kMcPA, 0x1000);
    gEngineLock = IOLockAlloc();
    if (!gNvdec || !gCar || !gPmc || !gMc || !gEngineLock) {
        IOLog("TegraNVDEC: N1 FAIL no mapping\n");
        return;
    }
    bool wasOn = false;
    gLine[0] = 0;
    if (!powerUp(&wasOn) || !bootFalcon()) {
        IOLog("TegraNVDEC: %s\n", gLine[0] ? gLine : "N1 FAIL");
        return;
    }
    IOLog("TegraNVDEC: N1 ok partition %u %s, 408 MHz, falcon started (OS code %#x+%#x data %#x+%#x, %u apps)\n",
          kPartition, wasOn ? "already on" : "powered here", gImage.code, gImage.codeSize, gImage.data, gImage.dataSize,
          gImage.apps);
    uint64_t us = 0;
    uint32_t id = 0, from = 0;
    if (!proofOfLife(&us, &id, &from)) {
        IOLog("TegraNVDEC: %s\n", gLine);
        return;
    }
    IOLog("TegraNVDEC: N2 ok host interface and falcon incremented syncpoint %u %u->%u by interrupt in %llu us; %s\n",
          id, from, tegra_host1x_syncpt_read(id), us, falconState());
    if (stage < 2) return;
    if (tegra_nvdec_arena_alloc(kSetupSlots * kSetupSlotBytes, &gSetup)) {
        IOLog("TegraNVDEC: N3 FAIL no setup slots in the arena\n");
        return;
    }
    gSyncpt = id;
    gBase = tegra_host1x_syncpt_read(id);
    gUp = true;
    tegra_host1x_set_sleep_client(engineSuspend, engineResume);
    if (tegra_nvdec_self_test() || stage < 3) return;
    tegra_nvdec_publish();
}
}  // namespace

extern "C" bool tegra_nvdec_ready(void)
{
    return gUp && !gWedged;
}

extern "C" int tegra_nvdec_run(const TegraNVDECSubmit *s, TegraNVDECResolve resolve, void *context, uint32_t *fence,
                               const char **why)
{
    *why = nullptr;
    if (!gUp) return -19;
    if (s->entry_count > TEGRA_NVDEC_MAX_ENTRIES || s->clean_count > TEGRA_NVDEC_MAX_RANGES ||
        s->setup_bytes > kSetupSlotBytes) {
        *why = "counts";
        return -22;
    }
    TegraNVDECBlock blocks[TEGRA_NVDEC_MAX_ENTRIES];
    uint64_t avail[TEGRA_NVDEC_MAX_ENTRIES] = {};
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        if (e->kind != TEGRA_NVDEC_ENTRY_RELOC) continue;
        if (!resolve(context, e->value, &blocks[i]) || e->offset > blocks[i].size) {
            *why = "relocation outside a buffer";
            return -22;
        }
        avail[i] = blocks[i].size - e->offset;
    }
    if ((*why = tegra_nvdec_check(s, avail))) return -22;
    for (uint32_t i = 0; i < s->clean_count; i++) {
        const TegraNVDECRange *r = &s->clean[i];
        TegraNVDECBlock b;
        if (!resolve(context, r->handle, &b) || (uint64_t)r->offset + r->length > b.size) {
            *why = "clean range";
            return -22;
        }
        tegra_nvdec_cache_clean(b.va + r->offset, r->length);
    }

    // SETCLASS, (INCR METHOD0 x2: method, value) per method with the setup slot before EXECUTE, OP_DONE increment.
    uint32_t words[1 + 3 * (TEGRA_NVDEC_MAX_ENTRIES + 1) + 1], n = 0;
    words[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0);
    auto method = [&](uint32_t offset, uint32_t value) {
        words[n++] = tegra_host1x_op_incr(kThiMethod0 / 4, 2);
        words[n++] = offset / 4;
        words[n++] = value;
    };
    IOLockLock(gEngineLock);
    while (gSuspended) IOLockSleep(gEngineLock, &gSuspended, THREAD_UNINT);
    if (gWedged) {
        IOLockUnlock(gEngineLock);
        return -19;
    }
    const uint32_t job = gIssued + 1;
    if (job > kInFlight) {
        const int ret = tegra_host1x_syncpt_wait(gSyncpt, gBase + job - kInFlight, kJobTimeoutUs);
        if (ret) {
            if (ret == TEGRA_HOST1X_ETIMEDOUT) wedge(gBase + job - kInFlight);
            IOLockUnlock(gEngineLock);
            return ret;
        }
    }
    const uint32_t slot = job % kSetupSlots;
    memcpy(gSetup.va + slot * kSetupSlotBytes, s->setup, s->setup_bytes);
    tegra_nvdec_cache_clean(gSetup.va + slot * kSetupSlotBytes, s->setup_bytes);
    for (uint32_t i = 0; i < s->entry_count; i++) {
        const TegraNVDECEntry *e = &s->entries[i];
        if (i == s->entry_count - 1)  // EXECUTE, last by the check
            method(NVC5B0_SET_DRV_PIC_SETUP_OFFSET, (uint32_t)((gSetup.pa + slot * kSetupSlotBytes) >> 8));
        method(e->method, e->kind == TEGRA_NVDEC_ENTRY_RELOC ? (uint32_t)((blocks[i].pa + e->offset) >> 8) : e->value);
    }
    words[n++] = tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt);
    const int ret = tegra_host1x_submit(words, n);
    if (!ret) {
        __atomic_store_n(&gIssued, job, __ATOMIC_RELEASE);
        *fence = gBase + job;
    }
    IOLockUnlock(gEngineLock);
    return ret;
}

extern "C" int tegra_nvdec_wait(uint32_t fence, uint32_t timeoutUs)
{
    if (!gUp) return -19;
    // A fence no job will reach would time out and wedge the engine for everyone.
    if ((int32_t)(fence - (gBase + __atomic_load_n(&gIssued, __ATOMIC_ACQUIRE))) > 0) return -22;
    const int ret = tegra_host1x_syncpt_wait(gSyncpt, fence, timeoutUs);
    if (ret == TEGRA_HOST1X_ETIMEDOUT) {
        IOLockLock(gEngineLock);
        wedge(fence);
        IOLockUnlock(gEngineLock);
    }
    return ret;
}

// Sleep (SC7), through tegra_nvdec_suspend/resume in TegraHost1x.cpp. Suspend lets the queued jobs finish
// (interrupt waits) and parks the engine in reset before host1x saves its syncpoints; new jobs block until resume.
// Resume runs after host1x is back: the partition is powered, the falcon reloaded from the kept image and proven
// with an IMMEDIATE and an OP_DONE increment, as at N2.
namespace {
void engineSuspend()
{
    IOLockLock(gEngineLock);
    gSuspended = true;
    if (!gWedged && tegra_host1x_syncpt_wait(gSyncpt, gBase + gIssued, kJobTimeoutUs)) wedge(gBase + gIssued);
    w32(gCar, RST_DEV_Y_SET, Y_NVDEC);
    IODelay(2);
    w32(gCar, CLK_ENB_Y_CLR, Y_NVDEC);
    IOLockUnlock(gEngineLock);
}

void engineResume()
{
    IOLockLock(gEngineLock);
    bool wasOn = false;
    gLine[0] = 0;
    gWedged = true;
    if (powerUp(&wasOn) && bootFalcon()) {
        const uint32_t from = tegra_host1x_syncpt_read(gSyncpt);
        const uint32_t stream[] = {
            tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_NVDEC, 0, 0),
            tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, gSyncpt),
            tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_OP_DONE, gSyncpt),
        };
        const int ret = tegra_host1x_submit(stream, 3);
        if (!ret && !tegra_host1x_syncpt_wait(gSyncpt, from + 2, 500000))
            gWedged = false;
        else
            snprintf(gLine, sizeof(gLine), "falcon did not answer (%d, syncpoint %u %u->%u); %s", ret, gSyncpt, from,
                     tegra_host1x_syncpt_read(gSyncpt), falconState());
    }
    if (gWedged) IOLog("TegraNVDEC: resume FAIL %s; NVDEC refuses work\n", gLine[0] ? gLine : "N1");
    // Jobs lost to a wedge never incremented the syncpoint: number from where it really is.
    gBase = tegra_host1x_syncpt_read(gSyncpt);
    __atomic_store_n(&gIssued, 0, __ATOMIC_RELEASE);
    gSuspended = false;
    IOLockWakeup(gEngineLock, &gSuspended, false);
    IOLockUnlock(gEngineLock);
}
}  // namespace

extern "C" int tegra_nvdec_probe(void)
{
    static volatile int started;
    uint32_t stage = 0;
    if (!PE_parse_boot_argn("switch-nvdec", &stage, sizeof(stage)) || !stage) return 0;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_SEQ_CST)) return 0;
    thread_t thread;
    if (kernel_thread_start(bringUp, (void *)(uintptr_t)stage, &thread) != KERN_SUCCESS) {
        IOLog("TegraNVDEC: could not start the bring-up thread\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
