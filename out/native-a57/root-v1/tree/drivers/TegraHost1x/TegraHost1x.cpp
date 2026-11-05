// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 host1x (version 5); see TegraHost1x.h.
//
// Sources (register map and order; Linux's host1x driver is GPL-2.0-only, so this is written from it, not copied):
//   Linux drivers/gpu/host1x/dev.c host1x05_info (192 syncpoints, 14 channels, sync unit at +0x2100),
//   hw/channel_hw.c host1x_channel_init (channel n at n * 0x4000), hw/hw_host1x05_{sync,channel}.h, hw/opcodes.h,
//   hw/cdma_hw.c cdma_start/cdma_flush (DMASTART = push buffer, DMAPUT/DMAGET/DMAEND relative to it, a RESTART
//   opcode after the last slot), hw/intr_hw.c (threshold interrupt: THRESH, then ENABLE_CPU0; the handler DISABLEs
//   and clears INT_STATUS), host1x_intr_init_host_sync (IP_BUSY_TIMEOUT 0, CTXSW_TIMEOUT_CFG 0xff, USEC_CLK).
//   tegra210.dtsi host1x@50000000: interrupts SPI 65 (syncpt) and 67 (general), CAR clock and reset 28.
//   drivers/memory/tegra/tegra210.c: MC SMMU ASID of HC 0x250, client hot reset HC 0x200 bit 6.
//   hekate bdk/soc/clock.c _clock_host1x: PLLP / 2.5 = 163.2 MHz, enabled for the display (di.c waits on syncpt 9).
// The SMMU stays bypassed as for every other driver here: host1x fetches the push buffer by physical address, so it
// is contiguous, below 4 GiB (DMASTART is 32-bit on host1x05) and cleaned to the point of coherency before DMAPUT.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <string.h>
#include "TegraHost1x.h"
#include "../ArmGIC/IOSwitchInterrupts.h"

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
boolean_t PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
}

namespace {
constexpr vm_offset_t kHost1xPA = 0x50000000, kHost1xSize = 0x34000;
constexpr vm_offset_t kCarPA = 0x60006000, kMcPA = 0x70019000;
constexpr uint32_t kSyncOffset = 0x2100;
constexpr uint32_t kIntid = 32 + 65;  // "syncpt" (tegra210.dtsi host1x@50000000)
constexpr uint32_t kSyncpts = 192, kFirstOwn = 160;
constexpr uint32_t kPushBytes = 16384;  // 2048 two-word slots (4x Linux's), + RESTART: room for 8 NVDEC jobs
constexpr uint32_t kGraceMs = 50;

enum : uint32_t {  // clock and reset controller (Linux clk.h, clk-tegra-periph.c)
    RST_DEVICES_L = 0x004,
    CLK_OUT_ENB_L = 0x010,
    CLK_SOURCE_HOST1X = 0x180,
    RST_DEV_L_CLR = 0x304,
    CLK_ENB_L_SET = 0x320,
    L_HOST1X = 1u << 28,
    SOURCE_PLLP = 4u << 29,
    HOST1X_DIV_2_5 = 3,  // 7.1 divider: 408 MHz * 2 / (3 + 2) = 163.2 MHz
};
enum : uint32_t { MC_HOTRESET_CTRL = 0x200, MC_HOTRESET_HC = 1u << 6, MC_SMMU_HC_ASID = 0x250, SMMU_ENABLE = 1u << 31 };
enum : uint32_t {  // channel registers, channel 0 at the start of the aperture
    CH_FIFOSTAT = 0x00,
    CH_DMASTART = 0x14,
    CH_DMAPUT = 0x18,
    CH_DMAGET = 0x1c,
    CH_DMAEND = 0x20,
    CH_DMACTRL = 0x24,
    DMACTRL_STOP = 1u << 0,
    DMACTRL_GETRST = 1u << 1,
    DMACTRL_INITGET = 1u << 2,
};
enum : uint32_t {  // sync unit, relative to kSyncOffset
    SYNC_CMDPROC_STOP = 0x0ac,
    SYNC_USEC_CLK = 0x1a4,
    SYNC_CTXSW_TIMEOUT_CFG = 0x1a8,
    SYNC_IP_BUSY_TIMEOUT = 0x1bc,
    SYNC_CBREAD0 = 0xc80,
    SYNC_CBSTAT0 = 0xcc0,
    SYNC_INT_STATUS = 0xe80,
    SYNC_INT_DISABLE = 0xf00,
    SYNC_INT_ENABLE_CPU0 = 0xf20,
    SYNC_SYNCPT = 0xf80,
    SYNC_INT_THRESH = 0x1380,
};
constexpr uint32_t opRestart(uint32_t byteOffset)
{
    return (5u << 28) | (byteOffset >> 4);
}
constexpr uint32_t kOpNop = 2u << 28;  // NONINCR, offset 0, count 0

volatile uint8_t *gHost, *gCar, *gMc;
IOWorkLoop *gLoop;
IOInterruptEventSource *gIrq;
IOLock *gLock;        // syncpoint waits and the interrupt action
IOLock *gSubmitLock;  // channel 0's push buffer
IOBufferMemoryDescriptor *gPush;
uint32_t *gPushVA;
uint64_t gPushPA;
uint32_t gPut;   // byte offset of the next word
uint32_t gWake;  // wait channel; also counts interrupt actions
uint32_t gNextSyncpt = kFirstOwn;
uint32_t gArmed[kSyncpts / 32];  // threshold interrupts a waiter enabled (gLock); INT_STATUS bits are sticky for all
int gState;                      // 0 not started, 1 up, 2 suspended, < 0 failed
uint32_t gSaved[kSyncpts];       // syncpoint values across SC7, which resets the sync unit
unsigned gClockMHz;
bool gStrict;  // a client proved its engine: a lost interrupt panics from here on (tegra_host1x_set_strict)

inline uint32_t r32(volatile uint8_t *base, uint32_t o)
{
    return *(volatile uint32_t *)(base + o);
}
inline void w32(volatile uint8_t *base, uint32_t o, uint32_t v)
{
    *(volatile uint32_t *)(base + o) = v;
}
inline uint32_t sync(uint32_t o)
{
    return r32(gHost, kSyncOffset + o);
}
inline void syncWrite(uint32_t o, uint32_t v)
{
    w32(gHost, kSyncOffset + o, v);
}
inline bool reached(uint32_t value, uint32_t threshold)
{
    return (int32_t)(value - threshold) >= 0;
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

// The syncpoint line (level). Every armed status bit seen is disabled and cleared, as Linux syncpt_thresh_isr does,
// so the line is quiet on return; waiters re-read their syncpoint. Status bits of syncpoints nobody armed (another
// engine counting) are sticky but do not drive the line, and are left alone.
void interruptOccurred(OSObject *, IOInterruptEventSource *, int)
{
    IOLockLock(gLock);
    for (uint32_t i = 0; i < kSyncpts / 32; i++) {
        const uint32_t status = sync(SYNC_INT_STATUS + 4 * i) & gArmed[i];
        if (!status) continue;
        syncWrite(SYNC_INT_DISABLE + 4 * i, status);
        syncWrite(SYNC_INT_STATUS + 4 * i, status);
        gArmed[i] &= ~status;
    }
    gWake++;
    IOLockWakeup(gLock, &gWake, false);
    IOLockUnlock(gLock);
}

int fail(const char *why, uint32_t value)
{
    IOLog("TegraHost1x: H1 FAIL %s (%#x)\n", why, value);
    gState = TEGRA_HOST1X_ENODEV;
    return gState;
}

// Adopts the clock and reset the boot chain left for the display; enables them only if they are off. Never pulses
// the reset: every engine and the display controller are reached through host1x.
bool clockOn(unsigned *mhz, bool *enabled)
{
    *enabled = false;
    if (!(r32(gCar, CLK_OUT_ENB_L) & L_HOST1X)) {
        w32(gCar, CLK_SOURCE_HOST1X, SOURCE_PLLP | HOST1X_DIV_2_5);
        IODelay(2);
        w32(gCar, CLK_ENB_L_SET, L_HOST1X);
        *enabled = true;
    }
    if (r32(gCar, RST_DEVICES_L) & L_HOST1X) {
        IODelay(2);
        w32(gCar, RST_DEV_L_CLR, L_HOST1X);
        *enabled = true;
    }
    const uint32_t source = r32(gCar, CLK_SOURCE_HOST1X);
    *mhz = (source >> 29) == 4 ? 408u * 2 / ((source & 0xff) + 2) : 0;  // only PLLP is known for certain
    return (r32(gCar, CLK_OUT_ENB_L) & L_HOST1X) && !(r32(gCar, RST_DEVICES_L) & L_HOST1X);
}

// Linux cdma_start for channel 0: the push buffer is allocated once and reprogrammed after SC7.
bool channelAlloc()
{
    const IOOptionBits options = (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous |
                                 (IOOptionBits)kIOMemoryMapperNone;
    gPush =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, kPushBytes + PAGE_SIZE, 0xfffff000ULL);
    if (!gPush || gPush->prepare() != kIOReturnSuccess) return false;
    IOByteCount length = 0;
    gPushPA = gPush->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    gPushVA = (uint32_t *)gPush->getBytesNoCopy();
    return gPushPA && gPushVA && length >= kPushBytes + 4 && gPushPA + kPushBytes + 4 <= (1ULL << 32);
}

void channelProgram()
{
    memset(gPushVA, 0, kPushBytes + 4);
    gPushVA[kPushBytes / 4] = opRestart(0);
    cleanToPoC(gPushVA, kPushBytes + 4);
    syncWrite(SYNC_CMDPROC_STOP, sync(SYNC_CMDPROC_STOP) & ~1u);
    w32(gHost, CH_DMACTRL, DMACTRL_STOP);
    w32(gHost, CH_DMASTART, (uint32_t)gPushPA);
    w32(gHost, CH_DMAPUT, 0);
    w32(gHost, CH_DMAEND, kPushBytes + 4);
    w32(gHost, CH_DMACTRL, DMACTRL_STOP | DMACTRL_GETRST | DMACTRL_INITGET);
    w32(gHost, CH_DMACTRL, 0);
    gPut = 0;
}

// host1x_intr_init_host_sync: what the sync unit needs after a reset (boot or SC7). Every threshold interrupt starts
// disabled and clear; waiters arm their own.
void syncConfigure()
{
    syncWrite(SYNC_IP_BUSY_TIMEOUT, 0);
    syncWrite(SYNC_CTXSW_TIMEOUT_CFG, 0xff);
    if (gClockMHz) syncWrite(SYNC_USEC_CLK, gClockMHz);
    for (uint32_t i = 0; i < kSyncpts / 32; i++) {
        syncWrite(SYNC_INT_DISABLE + 4 * i, 0xffffffffu);
        syncWrite(SYNC_INT_STATUS + 4 * i, 0xffffffffu);
    }
}

// Before any engine behind host1x is proven, a missing interrupt is a bring-up failure (one line, nothing published),
// not a panic: host1x comes up by default for NVDEC and has never run on this board.
bool attachInterrupt()
{
    gLoop = IOWorkLoop::workLoop();
    IOService *nub = ioswitch_interrupt_nub(kIntid, false);
    gIrq = gLoop && nub ? IOInterruptEventSource::interruptEventSource(gLoop, &interruptOccurred, nub, 0) : nullptr;
    if (!gIrq || gLoop->addEventSource(gIrq) != kIOReturnSuccess) return false;
    gIrq->enable();
    return true;
}

uint64_t nowUs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

int bringUp()
{
    gHost = (volatile uint8_t *)ml_io_map(kHost1xPA, kHost1xSize);
    gCar = (volatile uint8_t *)ml_io_map(kCarPA, 0x1000);
    gMc = (volatile uint8_t *)ml_io_map(kMcPA, 0x1000);
    gLock = IOLockAlloc();
    gSubmitLock = IOLockAlloc();
    if (!gHost || !gCar || !gMc || !gLock || !gSubmitLock) return fail("no mapping", 0);
    bool enabled = false;
    if (!clockOn(&gClockMHz, &enabled))
        return fail("clock or reset stuck, CAR CLK_OUT_ENB_L", r32(gCar, CLK_OUT_ENB_L));
    if (r32(gMc, MC_SMMU_HC_ASID) & SMMU_ENABLE)
        return fail("SMMU translation is on for HC, MC 0x250", r32(gMc, MC_SMMU_HC_ASID));
    if (r32(gMc, MC_HOTRESET_CTRL) & MC_HOTRESET_HC)
        w32(gMc, MC_HOTRESET_CTRL, r32(gMc, MC_HOTRESET_CTRL) & ~MC_HOTRESET_HC);
    syncConfigure();
    if (!attachInterrupt()) return fail("cannot attach the syncpoint interrupt, INTID", kIntid);
    if (!channelAlloc()) return fail("no push buffer below 4 GiB", 0);
    channelProgram();
    gState = 1;

    uint32_t id = 0;
    tegra_host1x_syncpt_alloc(&id);
    const uint32_t before = tegra_host1x_syncpt_read(id);
    const uint32_t stream[] = {
        tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0, 0),
        tegra_host1x_op_incr_syncpt(TEGRA_HOST1X_COND_IMMEDIATE, id),
    };
    const uint64_t began = nowUs();
    int ret = tegra_host1x_submit(stream, 2);
    if (!ret) ret = tegra_host1x_syncpt_wait(id, before + 1, 100000);
    if (ret) {
        char state[160];
        tegra_host1x_describe(state, sizeof(state));
        IOLog("TegraHost1x: H1 FAIL self-test syncpoint %u at %u (%s), %s\n", id, tegra_host1x_syncpt_read(id),
              ret == TEGRA_HOST1X_ELOST ? "reached, but its interrupt never came" : "not reached", state);
        gState = TEGRA_HOST1X_ENODEV;
        return gState;
    }
    IOLog("TegraHost1x: H1 ok syncpoint %u %u->%u by interrupt in %llu us (channel 0, host1x %u MHz%s)\n", id, before,
          tegra_host1x_syncpt_read(id), nowUs() - began, gClockMHz, enabled ? ", enabled here" : "");
    return TEGRA_HOST1X_OK;
}

void probeThread(void *, wait_result_t)
{
    tegra_host1x_start();
}
}  // namespace

extern "C" int tegra_host1x_start(void)
{
    static volatile int started;
    static IOLock *startLock;
    if (!startLock) {
        IOLock *fresh = IOLockAlloc(), *expected = nullptr;
        if (!__atomic_compare_exchange_n(&startLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(fresh);
    }
    IOLockLock(startLock);
    if (!started) {
        started = 1;
        bringUp();
    }
    const int state = gState;
    IOLockUnlock(startLock);
    return state == 1 ? TEGRA_HOST1X_OK : TEGRA_HOST1X_ENODEV;
}

extern "C" int tegra_host1x_syncpt_alloc(uint32_t *id)
{
    const uint32_t next = __atomic_fetch_add(&gNextSyncpt, 1, __ATOMIC_RELAXED);
    if (next >= kSyncpts) return TEGRA_HOST1X_ENOMEM;
    *id = next;
    return TEGRA_HOST1X_OK;
}

extern "C" uint32_t tegra_host1x_syncpt_read(uint32_t id)
{
    return gHost && id < kSyncpts ? sync(SYNC_SYNCPT + 4 * id) : 0;
}

// Linux host1x_intr_add_fence order: threshold, then enable; the value is read again after arming so an increment
// that landed in between is not slept on. A wait that ends with the syncpoint's status bit still set at the
// controller, after a grace period for an action under way, means the interrupt never reached the CPU: a panic once
// strict (an engine was proven and published), TEGRA_HOST1X_ELOST while its probe is still running.
extern "C" int tegra_host1x_syncpt_wait(uint32_t id, uint32_t threshold, uint32_t timeoutUs)
{
    if (gState != 1) return TEGRA_HOST1X_ENODEV;
    if (id >= kSyncpts) return TEGRA_HOST1X_EINVAL;
    const uint32_t word = id / 32, bit = 1u << (id % 32);
    IOLockLock(gLock);
    syncWrite(SYNC_INT_THRESH + 4 * id, threshold);
    syncWrite(SYNC_INT_STATUS + 4 * word, bit);
    syncWrite(SYNC_INT_ENABLE_CPU0 + 4 * word, bit);
    gArmed[word] |= bit;
    uint64_t deadline;
    clock_interval_to_deadline(timeoutUs, kMicrosecondScale, &deadline);
    int ret = TEGRA_HOST1X_OK;
    for (bool grace = false; !reached(tegra_host1x_syncpt_read(id), threshold);) {
        if (IOLockSleepDeadline(gLock, &gWake, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) continue;
        if (!reached(tegra_host1x_syncpt_read(id), threshold)) {
            ret = TEGRA_HOST1X_ETIMEDOUT;  // the engine did not get there: its failure, not the interrupt's
            break;
        }
        if (!(sync(SYNC_INT_STATUS + 4 * word) & bit)) break;
        if (grace) {
            if (!__atomic_load_n(&gStrict, __ATOMIC_ACQUIRE)) {
                ret = TEGRA_HOST1X_ELOST;
                break;
            }
            char gic[192];
            ioswitch_interrupt_describe(kIntid, gic, sizeof(gic));
            panic("TegraHost1x: syncpoint %u threshold interrupt lost (INTID %u): value %u threshold %u INT_STATUS[%u] "
                  "%#x, %s",
                  id, kIntid, tegra_host1x_syncpt_read(id), threshold, word, sync(SYNC_INT_STATUS + 4 * word), gic);
        }
        grace = true;
        clock_interval_to_deadline(kGraceMs, kMillisecondScale, &deadline);
    }
    syncWrite(SYNC_INT_DISABLE + 4 * word, bit);
    syncWrite(SYNC_INT_STATUS + 4 * word, bit);
    gArmed[word] &= ~bit;
    IOLockUnlock(gLock);
    return ret;
}

extern "C" void tegra_host1x_set_strict(void)
{
    __atomic_store_n(&gStrict, true, __ATOMIC_RELEASE);
}

// Linux push_buffer_push + cdma_flush: words go in at PUT (wrapping at the RESTART slot), are cleaned to DRAM, and
// DMAPUT moves past them. The space check uses DMAGET, which host1x keeps relative to DMASTART like PUT.
extern "C" int tegra_host1x_submit(const uint32_t *words, uint32_t count)
{
    if (gState != 1) return TEGRA_HOST1X_ENODEV;
    if (!words || !count || count >= kPushBytes / 4) return TEGRA_HOST1X_EINVAL;
    IOLockLock(gSubmitLock);
    const uint32_t get = r32(gHost, CH_DMAGET) % kPushBytes;
    const uint32_t room = (get + kPushBytes - gPut - 4) % kPushBytes;
    // A job must not straddle the RESTART slot: Linux's push buffer only holds two-word GATHERs, but these are the
    // job's own opcodes, and a RESTART landing inside an opcode's data is taken as data, so the command processor runs
    // on to DMAEND and stops (r7p5: DMAGET 0x4004, DMAPUT 0x84, fence never reached). Such a job starts at 0, the rest
    // of the buffer NOPs (host1x_opcode_nop: NONINCR, count 0) up to the RESTART.
    const uint32_t tail = kPushBytes - gPut, bytes = count * 4;
    const uint32_t pad = bytes > tail ? tail : 0;
    if (pad + bytes > room) {
        IOLockUnlock(gSubmitLock);
        return TEGRA_HOST1X_ENOMEM;
    }
    if (pad) {
        for (uint32_t at = gPut; at < kPushBytes; at += 4) gPushVA[at / 4] = kOpNop;
        cleanToPoC((uint8_t *)gPushVA + gPut, pad);
        gPut = 0;
    }
    const uint32_t from = gPut;
    for (uint32_t i = 0; i < count; i++) gPushVA[from / 4 + i] = words[i];
    cleanToPoC((uint8_t *)gPushVA + from, bytes);
    gPut = (from + bytes) % kPushBytes;  // a job that ends exactly at the RESTART slot leaves PUT at 0
    w32(gHost, CH_DMAPUT, gPut);
    IOLockUnlock(gSubmitLock);
    return TEGRA_HOST1X_OK;
}

extern "C" void tegra_host1x_describe(char *buffer, size_t size)
{
    if (!gHost) {
        snprintf(buffer, size, "host1x not mapped");
        return;
    }
    const uint32_t cbstat = sync(SYNC_CBSTAT0);
    snprintf(buffer, size,
             "ch0 DMAGET %#x DMAPUT %#x DMACTRL %#x FIFOSTAT %#x class %#x offset %#x CBREAD %#x STOP %#x",
             r32(gHost, CH_DMAGET), r32(gHost, CH_DMAPUT), r32(gHost, CH_DMACTRL), r32(gHost, CH_FIFOSTAT),
             (cbstat >> 16) & 0x3ff, cbstat & 0xffff, sync(SYNC_CBREAD0), sync(SYNC_CMDPROC_STOP));
}

// Clients have quiesced (their last fences reached) before this. SC7 powers the sync unit and channels down, so the
// syncpoint values handed out are saved (Linux host1x_syncpt_save) and channel 0 is stopped.
extern "C" void tegra_host1x_suspend(void)
{
    if (gState != 1) return;
    IOLockLock(gSubmitLock);
    for (uint32_t id = kFirstOwn; id < kSyncpts && id < gNextSyncpt; id++) gSaved[id] = sync(SYNC_SYNCPT + 4 * id);
    w32(gHost, CH_DMACTRL, DMACTRL_STOP);
    gState = 2;
    IOLockUnlock(gSubmitLock);
}

// Clock adopted (or enabled) again, the sync unit configured as at boot, the saved syncpoint values written back
// (host1x_syncpt_restore) so outstanding fence numbers stay meaningful, and channel 0 restarted on its push buffer.
extern "C" void tegra_host1x_resume(void)
{
    if (gState != 2) return;
    IOLockLock(gSubmitLock);
    // SC7 resets the CAR: put host1x back on the boot chain's PLLP / 2.5 if it came back on another source (it is
    // idle, and nothing behind it has resumed yet: the display's wake hook runs after this one).
    if ((r32(gCar, CLK_SOURCE_HOST1X) >> 29) != 4) {
        w32(gCar, CLK_SOURCE_HOST1X, SOURCE_PLLP | HOST1X_DIV_2_5);
        IODelay(2);
    }
    bool enabled = false;
    if (!clockOn(&gClockMHz, &enabled)) {
        IOLog("TegraHost1x: H1 FAIL resume: clock or reset stuck, CAR CLK_OUT_ENB_L %#x\n", r32(gCar, CLK_OUT_ENB_L));
        gState = TEGRA_HOST1X_ENODEV;
        IOLockUnlock(gSubmitLock);
        return;
    }
    IOLockLock(gLock);
    syncConfigure();
    for (uint32_t i = 0; i < kSyncpts / 32; i++) gArmed[i] = 0;
    IOLockUnlock(gLock);
    for (uint32_t id = kFirstOwn; id < kSyncpts && id < gNextSyncpt; id++) syncWrite(SYNC_SYNCPT + 4 * id, gSaved[id]);
    channelProgram();
    gState = 1;
    IOLockUnlock(gSubmitLock);
}

// The sleep agent's hooks (SwitchSuspend calls them in thread context, interrupts on). They live here, in the file
// that is always built, so they exist whether or not NVDEC is; NVDEC, NVENC and VIC register their quiesce and reload
// once they are up. Clients quiesce in the reverse order of registration and reload in order.
namespace {
constexpr uint32_t kSleepClients = 3;
struct SleepClient {
    void (*suspend)(void), (*resume)(void);
} gSleepClients[kSleepClients];
uint32_t gSleepClientCount;
}  // namespace

extern "C" void tegra_host1x_set_sleep_client(void (*suspend)(void), void (*resume)(void))
{
    if (__atomic_load_n(&gSleepClientCount, __ATOMIC_ACQUIRE) >= kSleepClients) {
        IOLog("TegraHost1x: a fourth sleep client was refused (three at most)\n");
        return;
    }
    const uint32_t i = __atomic_fetch_add(&gSleepClientCount, 1, __ATOMIC_ACQ_REL);
    gSleepClients[i] = {suspend, resume};
}

extern "C" void tegra_nvdec_suspend(void)
{
    for (uint32_t i = __atomic_load_n(&gSleepClientCount, __ATOMIC_ACQUIRE); i-- > 0;) gSleepClients[i].suspend();
    tegra_host1x_suspend();
}

extern "C" void tegra_nvdec_resume(void)
{
    tegra_host1x_resume();
    const uint32_t count = __atomic_load_n(&gSleepClientCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < count; i++) gSleepClients[i].resume();
}

extern "C" int tegra_host1x_probe(void)
{
    uint32_t on = 0;
    if (!PE_parse_boot_argn("switch-host1x", &on, sizeof(on)) || !on) return 0;
    thread_t thread;
    if (kernel_thread_start(probeThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("TegraHost1x: could not start the bring-up thread\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
