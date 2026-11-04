// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 I2C master: packet mode with PIO FIFOs, one IOLock per bus, every completion by interrupt.
// Register layout and bring-up follow hekate bdk/soc/{i2c,clock,pinmux}.c (proven on this board); packet headers,
// FIFO triggers, the interrupt handler and error handling follow Linux i2c-tegra.c (tegra_i2c_xfer_msg,
// tegra_i2c_isr; T210 has_config_load_reg, has_per_pkt_xfer_complete_irq, supports_bus_clear).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/OSAtomic.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/types.h>
#include <string.h>
#include "TegraI2C.h"
#include "../ArmGIC/IOSwitchInterrupts.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" boolean_t preemption_enabled(void);

namespace {
constexpr vm_offset_t kI2CPA = 0x7000C000, kI2CSize = 0x2000;          // I2C1..I2C6
constexpr vm_offset_t kCarPA = 0x60006000, kCarSize = 0x1000;          // CLK_RST_CONTROLLER
constexpr vm_offset_t kPinmuxPA = 0x70003000, kPinmuxSize = 0x1000;    // PINMUX_AUX
constexpr vm_offset_t kApbMiscPA = 0x70000000, kApbMiscSize = 0x1000;  // APB_MISC: the pads' drive (GP_*_CFGPADCTRL)
constexpr unsigned kBuses = 5;
constexpr uint32_t kClkM = 19200000;  // CLK_M = 38.4 MHz OSC / 2
constexpr uint32_t kBusDivide = 48;   // (TLOW 4 + THIGH 2 + 2) * (CLK_DIVISOR std/fast 5 + 1)
constexpr uint32_t kGraceMs = 50;     // after a timed-out wait, for an interrupt action already under way
constexpr unsigned kArbRetries = 3;   // multi-master bus: repeats of a transfer that lost arbitration

// Controller registers.
enum : uint32_t {
    CNFG = 0x00,
    SL_CNFG = 0x20,
    SL_ADDR1 = 0x2c,
    SL_ADDR2 = 0x30,
    TX_FIFO = 0x50,
    RX_FIFO = 0x54,
    FIFO_CONTROL = 0x5c,
    FIFO_STATUS = 0x60,
    INT_MASK = 0x64,
    INT_STATUS = 0x68,
    CLK_DIVISOR = 0x6c,
    BUS_CLEAR_CNFG = 0x84,
    BUS_CLEAR_STATUS = 0x88,
    CONFIG_LOAD = 0x8c,
};
enum : uint32_t {
    CNFG_DEBOUNCE_4T = 2u << 12,
    CNFG_PACKET_MODE = 1u << 10,
    CNFG_NEW_MASTER_FSM = 1u << 11,
    CNFG_MULTI_MASTER = 1u << 17,
    SL_CNFG_NACK = 1u << 1,
    SL_CNFG_NEWSL = 1u << 2,
    FIFO_FLUSH = 3u << 0,      // RX_FLUSH | TX_FLUSH
    FIFO_TX_TRIG_8 = 7u << 5,  // TX request when 8 slots are free, RX request at 1 word
    INT_RX_REQ = 1u << 0,
    INT_TX_REQ = 1u << 1,
    INT_ARB_LOST = 1u << 2,
    INT_NO_ACK = 1u << 3,
    INT_PACKET_DONE = 1u << 7,
    INT_BUS_CLEAR_DONE = 1u << 11,
    MSTR_CONFIG_LOAD = 1u << 0,
    BC_ENABLE = 1u << 0,
    BC_CONFIG = (9u << 16) | (1u << 2) | (1u << 1),  // 9 pulses, STOP, terminate
    BC_STATUS = 1u << 0,
    // Packet header words (generic header 0, I2C header 1, protocol header 2).
    PKT0_I2C = (1u << 16) | (1u << 4),  // packet id 1, protocol I2C
    HDR_REPEAT_START = 1u << 16,
    HDR_IE = 1u << 17,
    HDR_READ = 1u << 19,
};
constexpr uint32_t kPinmuxI2C = 1u << 6;  // function 0 (I2Cx), input enabled, no pull
constexpr int kPending = 1;               // packet result while the transfer runs

struct BusInfo {
    uint16_t regs;                     // offset from kI2CPA
    uint16_t reset;                    // CAR RST_DEV_x_SET (CLR is +4)
    uint16_t enable;                   // CAR CLK_ENB_x_SET (CLR is +4)
    uint16_t source;                   // CAR CLK_SOURCE_I2Cx
    uint16_t resetState, enableState;  // CAR RST_DEVICES_x / CLK_OUT_ENB_x
    uint8_t bit;
    uint8_t divider;    // CLK_M divider: 4 -> 100 kHz, 1 -> 400 kHz
    uint16_t scl, sda;  // PINMUX_AUX offsets
    uint16_t spi;       // GIC SPI: Linux tegra210.dtsi:676/691/706/721/739, OLED DTB i2c@7000c000.. interrupts
    uint16_t sclDrive, sdaDrive;  // APB_MISC GP_*_I2C_*_CFGPADCTRL
    uint8_t pullDown;             // DRVDN, the pads' pull-down (sink) strength; DRVUP is 0
};
// hekate clock.c _clock_i2c[] and pinmux.h PINMUX_AUX_X_I2C_SCL/SDA. The drive: Horizon's
// (Atmosphere pinmux_initial_drive_pad_config.inc: Gen1/Gen2/Cam/Pwr I2C 0x1f, Gen3 0x07) and L4T's for the OLED
// (tegra210b01-fric-pinmux.dtsi drive_default: the same pull-down strengths, pull-up 0); offsets from
// pinmux_drive_pad_characters.inc and Linux pinctrl-tegra210.c.
constexpr BusInfo kBus[kBuses] = {
    {0x0000, 0x300, 0x320, 0x124, 0x004, 0x010, 12, 4, 0xbc, 0xc0, 38, 0x9a8, 0x9ac, 0x1f},  // I2C1 (L)
    {0x0400, 0x308, 0x328, 0x198, 0x008, 0x014, 22, 1, 0xc4, 0xc8, 84, 0x9b0, 0x9b4, 0x1f},  // I2C2 (H)
    {0x0500, 0x310, 0x330, 0x1b8, 0x00c, 0x018, 3, 1, 0xcc, 0xd0, 92, 0x9b8, 0x9bc, 0x07},   // I2C3 (U)
    {0x0700, 0x430, 0x440, 0x3c4, 0x358, 0x360, 7, 4, 0xd4, 0xd8, 120, 0x934, 0x938, 0x1f},  // I2C4 (V), CAM_I2C pads
    {0x1000, 0x308, 0x328, 0x128, 0x008, 0x014, 15, 1, 0xdc, 0xe0, 53, 0xa6c, 0xa70, 0x1f},  // I2C5 (H), PWR_I2C pads
};
constexpr uint32_t kDriveMask = (0x1fu << 20) | (0x1fu << 12);  // DRVUP 24:20, DRVDN 16:12

struct Bus {
    volatile uint8_t *regs;
    IOLock *lock;     // one transfer at a time
    IOLock *irqLock;  // hands a wait between the caller and the interrupt action
    IOInterruptEventSource *irq;
    uint32_t hz;
    bool ready;
    bool owned;        // we reset/clocked it, so error recovery may reset it again
    bool multiMaster;  // another master shares the pads: lost arbitration is retried, never reset or bus-cleared
    bool final;        // tegra_i2c_final_transfer() has the bus for the rest of the boot
    // The wait in flight (under irqLock): a packet, or the bus clear.
    const uint8_t *tx;
    uint8_t *rx;
    size_t len, done;
    bool active, busClear, overflow;
    int result;
};

IOLock *volatile gSetupLock;
volatile uint8_t *gI2C, *gCar, *gPinmux, *gApbMisc;
IOWorkLoop *gLoop;
Bus gBus[kBuses];

inline uint32_t r32(volatile uint8_t *base, uint32_t o)
{
    return *(volatile uint32_t *)(base + o);
}
inline void w32(volatile uint8_t *base, uint32_t o, uint32_t v)
{
    *(volatile uint32_t *)(base + o) = v;
}
inline uint64_t nowUs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}
inline uint32_t intid(unsigned bus)
{
    return 32 + kBus[bus - 1].spi;
}

// I2C5 runs in MULTI_MASTER_MODE when the CPU goes on the DFLL (switch-cpu-mode=dfll), whose integrated I2C master
// writes VDD_CPU over the same PWR_I2C pads. L4T sets the bit on every T210 controller (i2c-tegra.c tegra_i2c_init,
// tegra210_i2c_hw.has_multi_master_mode) and answers arbitration loss with a re-init and -EAGAIN (no bus clear,
// which would cut into the other master's transfer); here the transfer is simply repeated.
bool multiMasterBus(unsigned bus)
{
    char mode[8] = "pll";
    return bus == 5 && PE_parse_boot_arg_str("switch-cpu-mode", mode, sizeof(mode)) && strcmp(mode, "dfll") == 0;
}

// Register handshakes without an interrupt of their own (FIFO flush, config load): a few microseconds.
bool waitClear(Bus &b, uint32_t reg, uint32_t bits, uint32_t us)
{
    for (uint64_t end = nowUs() + us; r32(b.regs, reg) & bits;) {
        if (nowUs() > end) return !(r32(b.regs, reg) & bits);
    }
    return true;
}

bool loadConfig(Bus &b)
{
    w32(b.regs, CONFIG_LOAD, MSTR_CONFIG_LOAD);
    return waitClear(b, CONFIG_LOAD, MSTR_CONFIG_LOAD, 2000);
}

// hekate clock_enable(): reset, gate, select CLK_M / divider, ungate, release reset.
void clockEnable(const BusInfo &info)
{
    const uint32_t bit = 1u << info.bit;
    w32(gCar, info.reset, bit);
    w32(gCar, info.enable + 4, bit);
    w32(gCar, info.source, (6u << 29) | (info.divider - 1u));
    w32(gCar, info.enable, bit);
    IODelay(2);
    w32(gCar, info.reset + 4, bit);
    (void)r32(gCar, info.reset);
}

// The bus's two pads: function I2Cx with the input on (hekate pinmux_config_i2c), and the pull-down strength Horizon and
// L4T give them (kBus). Without the drive setting the pads keep their reset strength, which r7p5's BH1730 on I2C2 did
// not answer through.
void padConfig(const BusInfo &info)
{
    w32(gPinmux, info.scl, kPinmuxI2C);
    w32(gPinmux, info.sda, kPinmuxI2C);
    // Only I2C2 (the ALS) for now: the other buses (touch, the PMIC, the camera bus) work at their current strength, and a
    // wrong drive on PWR_I2C would cost the boot. Widen once r7p8 shows the ALS answering with it.
    if (info.sclDrive != kBus[1].sclDrive) return;
    w32(gApbMisc, info.sclDrive, (r32(gApbMisc, info.sclDrive) & ~kDriveMask) | (uint32_t)info.pullDown << 12);
    w32(gApbMisc, info.sdaDrive, (r32(gApbMisc, info.sdaDrive) & ~kDriveMask) | (uint32_t)info.pullDown << 12);
}

bool clockRunning(const BusInfo &info)
{
    const uint32_t bit = 1u << info.bit;
    return (r32(gCar, info.enableState) & bit) && !(r32(gCar, info.resetState) & bit);
}

// Moves payload between the FIFOs and the packet: drains the RX FIFO for a read (discarding words past the end,
// which is an overflow), tops up the TX FIFO for a write.
void serviceFifo(Bus &b)
{
    const uint32_t fifo = r32(b.regs, FIFO_STATUS);
    if (b.rx) {
        for (uint32_t words = fifo & 0xf; words; --words) {
            const uint32_t word = r32(b.regs, RX_FIFO);
            if (b.done == b.len) {
                b.overflow = true;
                continue;
            }
            const size_t n = b.len - b.done < 4 ? b.len - b.done : 4;
            memcpy(b.rx + b.done, &word, n);
            b.done += n;
        }
    } else {
        for (uint32_t words = (fifo >> 4) & 0xf; words && b.done < b.len; --words) {
            uint32_t word = 0;
            const size_t n = b.len - b.done < 4 ? b.len - b.done : 4;
            memcpy(&word, b.tx + b.done, n);
            w32(b.regs, TX_FIFO, word);
            b.done += n;
        }
    }
}

// The packet's outcome once INT_STATUS shows an error or completion, kPending otherwise.
int outcome(Bus &b, uint32_t status)
{
    if (status & (INT_NO_ACK | INT_ARB_LOST)) return (status & INT_NO_ACK) ? TEGRA_I2C_ENACK : TEGRA_I2C_EARB;
    if (status & INT_PACKET_DONE) return b.done == b.len && !b.overflow ? TEGRA_I2C_OK : TEGRA_I2C_EIO;
    return kPending;
}

// Work loop action for a bus's SPI (level; the GIC masks it while this runs). Mirrors tegra_i2c_isr: errors end
// the packet, FIFO requests move data, PACKET_DONE ends it; the TX request is masked once the payload is queued.
void interruptOccurred(OSObject *, IOInterruptEventSource *sender, int)
{
    for (Bus &b : gBus) {
        if (b.irq != sender) continue;
        IOLockLock(b.irqLock);
        const uint32_t status = r32(b.regs, INT_STATUS);
        if (!b.active) {
            w32(b.regs, INT_MASK, 0);  // stale: nobody waits any more
        } else {
            if (b.busClear) {
                b.result = (status & INT_BUS_CLEAR_DONE) ? TEGRA_I2C_OK : kPending;
            } else {
                serviceFifo(b);
                b.result = outcome(b, status);
            }
            if (b.result != kPending) {
                w32(b.regs, INT_MASK, 0);
                b.active = false;
                IOLockWakeup(b.irqLock, &b.active, true);
            } else if (!b.busClear && !b.rx && b.done == b.len) {
                w32(b.regs, INT_MASK, r32(b.regs, INT_MASK) & ~INT_TX_REQ);
            }
        }
        w32(b.regs, INT_STATUS, status);  // the FIFO request bits are levels and ignore this
        IOLockUnlock(b.irqLock);
    }
}

// Caller holds irqLock and has unmasked the interrupts it waits for. Sleeps until the action ends the wait or `us`
// pass. Timing out while the controller still asserts an unmasked status bit means its interrupt is not reaching
// us: after a grace period for an action already under way, panic with the controller's and the GIC's state.
// Otherwise the device simply did not finish (clock stretching, a stuck bus): ETIMEDOUT.
int sleepForAction(unsigned bus, Bus &b, uint64_t us)
{
    uint64_t deadline;
    clock_interval_to_deadline((uint32_t)us, kMicrosecondScale, &deadline);
    for (bool grace = false;; grace = true) {
        while (b.active && IOLockSleepDeadline(b.irqLock, &b.active, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
        if (!b.active) return b.result;
        const uint32_t status = r32(b.regs, INT_STATUS), mask = r32(b.regs, INT_MASK);
        if (!(status & mask)) break;
        if (grace) {
            char gic[192];
            ioswitch_interrupt_describe(intid(bus), gic, sizeof(gic));
            panic("TegraI2C: I2C%u interrupt lost (INTID %u): INT_STATUS %#x INT_MASK %#x FIFO_STATUS %#x, %s", bus,
                  intid(bus), status, mask, r32(b.regs, FIFO_STATUS), gic);
        }
        clock_interval_to_deadline(kGraceMs, kMillisecondScale, &deadline);
    }
    w32(b.regs, INT_MASK, 0);
    b.active = false;
    return TEGRA_I2C_ETIMEDOUT;
}

// hekate i2c_init() plus Linux tegra_i2c_init(): master FSM in packet mode, interrupts masked, std/fast divisor 5,
// never answer as a slave, then a bus clear (completion by INT_BUS_CLEAR_DONE) so a device left holding SDA by an
// interrupted transfer releases it.
int configure(unsigned bus, Bus &b)
{
    w32(b.regs, CNFG,
        CNFG_NEW_MASTER_FSM | CNFG_PACKET_MODE | CNFG_DEBOUNCE_4T | (b.multiMaster ? CNFG_MULTI_MASTER : 0));
    w32(b.regs, INT_MASK, 0);
    w32(b.regs, CLK_DIVISOR, (5u << 16) | 1u);
    w32(b.regs, SL_CNFG, r32(b.regs, SL_CNFG) | SL_CNFG_NACK | SL_CNFG_NEWSL);
    w32(b.regs, SL_ADDR1, 0xfc);
    w32(b.regs, SL_ADDR2, 0);
    w32(b.regs, FIFO_CONTROL, FIFO_FLUSH);
    if (!waitClear(b, FIFO_CONTROL, FIFO_FLUSH, 2000) || !loadConfig(b)) {
        IOLog("TegraI2C: I2C%u config load timed out\n", bus);
        return TEGRA_I2C_ETIMEDOUT;
    }
    w32(b.regs, BUS_CLEAR_CNFG, BC_CONFIG);
    if (!loadConfig(b)) return TEGRA_I2C_ETIMEDOUT;
    IOLockLock(b.irqLock);
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    b.busClear = true;
    b.result = kPending;
    b.active = true;
    w32(b.regs, INT_MASK, INT_BUS_CLEAR_DONE);
    w32(b.regs, BUS_CLEAR_CNFG, BC_CONFIG | BC_ENABLE);
    const int cleared = sleepForAction(bus, b, 10000);
    b.busClear = false;
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    IOLockUnlock(b.irqLock);
    if (cleared != TEGRA_I2C_OK || !(r32(b.regs, BUS_CLEAR_STATUS) & BC_STATUS)) {
        IOLog("TegraI2C: I2C%u bus clear did not complete\n", bus);
    }
    return TEGRA_I2C_OK;
}

// The bus's SPI on the shared work loop; ioswitch_interrupt_nub() sleeps until the GIC is up. Only called once the
// controller is clocked, so the action never reads an unclocked block.
void attachInterrupt(unsigned bus, Bus &b)
{
    if (b.irq) return;
    w32(b.regs, INT_MASK, 0);
    IOService *nub = ioswitch_interrupt_nub(intid(bus), false);
    b.irq = IOInterruptEventSource::interruptEventSource(gLoop, &interruptOccurred, nub, 0);
    if (!b.irq || gLoop->addEventSource(b.irq) != kIOReturnSuccess) {
        panic("TegraI2C: cannot attach the I2C%u interrupt (INTID %u)", bus, intid(bus));
    }
    b.irq->enable();
}

int bringUp(unsigned bus, Bus &b)
{
    const BusInfo &info = kBus[bus - 1];
    b.regs = gI2C + info.regs;
    b.multiMaster = multiMasterBus(bus);
    if (bus == 5 && clockRunning(info)) {
        // PWR_I2C left clocked by U-Boot (PLLP, 400 kHz): adopt its clock and config, never reset or re-clock it.
        b.owned = false;
        b.hz = 400000;
        const uint32_t cnfg = r32(b.regs, CNFG);
        const uint32_t want = cnfg | CNFG_PACKET_MODE | CNFG_NEW_MASTER_FSM | (b.multiMaster ? CNFG_MULTI_MASTER : 0);
        if (cnfg != want) {
            w32(b.regs, CNFG, want);
            if (!loadConfig(b)) return TEGRA_I2C_ETIMEDOUT;
        }
        attachInterrupt(bus, b);
        return TEGRA_I2C_OK;
    }
    b.owned = true;
    b.hz = kClkM / info.divider / kBusDivide;
    clockEnable(info);
    padConfig(info);
    if (bus == 3 || bus == 4) {
        // hekate pinmux_config_i2c(): detach the CAM_I2C (I2C4) pads from the I2C3 function.
        w32(gPinmux, kBus[3].scl, r32(gPinmux, kBus[3].scl) | 1u);
        w32(gPinmux, kBus[3].sda, r32(gPinmux, kBus[3].sda) | 1u);
    }
    attachInterrupt(bus, b);
    return configure(bus, b);
}

// Flush the FIFOs, then queue one packet: header and, for a write, as much payload as fits.
int startPacket(Bus &b, uint8_t addr, const uint8_t *out, uint8_t *in, size_t len, bool repeatStart)
{
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    w32(b.regs, FIFO_CONTROL, FIFO_TX_TRIG_8 | FIFO_FLUSH);
    if (!waitClear(b, FIFO_CONTROL, FIFO_FLUSH, 2000)) return TEGRA_I2C_ETIMEDOUT;
    b.tx = out;
    b.rx = in;
    b.len = len;
    b.done = 0;
    b.overflow = false;
    w32(b.regs, TX_FIFO, PKT0_I2C);
    w32(b.regs, TX_FIFO, (uint32_t)len - 1);
    w32(b.regs, TX_FIFO, HDR_IE | (repeatStart ? HDR_REPEAT_START : 0) | (in ? HDR_READ : 0) | ((uint32_t)addr << 1));
    if (out) serviceFifo(b);
    return TEGRA_I2C_OK;
}

// One packet: queue it, unmask the controller's interrupts (tegra_i2c_xfer_msg: errors and packet completion
// always, the FIFO request for the direction in use) and sleep until the action reports the outcome. The budget is
// 9 bits per byte plus START/address, twice over, and 5 ms for clock stretching.
int packet(unsigned bus, Bus &b, uint8_t addr, const uint8_t *out, uint8_t *in, size_t len, bool repeatStart)
{
    IOLockLock(b.irqLock);
    int result = startPacket(b, addr, out, in, len, repeatStart);
    if (result == TEGRA_I2C_OK) {
        b.result = kPending;
        b.active = true;
        w32(b.regs, INT_MASK,
            INT_NO_ACK | INT_ARB_LOST | INT_PACKET_DONE | (in ? INT_RX_REQ : (b.done < b.len ? INT_TX_REQ : 0)));
        result = sleepForAction(bus, b, 5000 + (uint64_t)(len + 2) * 18 * 1000000 / b.hz);
        w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    }
    b.tx = nullptr;
    b.rx = nullptr;
    IOLockUnlock(b.irqLock);
    return result;
}

// tegra_i2c_final_transfer()'s packet: the one polled transfer in this driver (TegraI2C.h). The controller's interrupts
// stay masked; INT_STATUS is read until an error or PACKET_DONE ends the packet, moving FIFO words on every pass, within
// the same budget packet() gives the interrupt. The caller holds irqLock when it could take it.
int pollPacket(Bus &b, uint8_t addr, const uint8_t *out, uint8_t *in, size_t len, bool repeatStart)
{
    int result = startPacket(b, addr, out, in, len, repeatStart);
    const uint64_t end = nowUs() + 5000 + (uint64_t)(len + 2) * 18 * 1000000 / b.hz;
    while (result == TEGRA_I2C_OK) {
        const uint32_t status = r32(b.regs, INT_STATUS);
        serviceFifo(b);
        const int done = outcome(b, status);
        if (done != kPending) {
            result = done;
            break;
        }
        if (nowUs() > end) result = TEGRA_I2C_ETIMEDOUT;
    }
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    b.tx = nullptr;
    b.rx = nullptr;
    return result;
}

// Reset after a failed transfer. irqLock is held while the clock is gated so no action reads an unclocked block.
// A multi-master bus only gets its FIFOs flushed: a reset's bus clear would cut into the other master's transfer.
void recover(unsigned bus, Bus &b)
{
    IOLockLock(b.irqLock);
    if (b.owned && !b.multiMaster) {
        clockEnable(kBus[bus - 1]);
        IOLockUnlock(b.irqLock);
        (void)configure(bus, b);
        return;
    }
    w32(b.regs, INT_MASK, 0);
    w32(b.regs, FIFO_CONTROL, r32(b.regs, FIFO_CONTROL) | FIFO_FLUSH);
    (void)waitClear(b, FIFO_CONTROL, FIFO_FLUSH, 2000);
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    IOLockUnlock(b.irqLock);
}

IOLock *setupLock()
{
    if (!gSetupLock) {
        IOLock *lock = IOLockAlloc();
        if (lock && !OSCompareAndSwapPtr(nullptr, lock, &gSetupLock)) IOLockFree(lock);
    }
    return gSetupLock;
}
}

extern "C" int tegra_i2c_init(unsigned bus)
{
    if (bus < 1 || bus > kBuses) return TEGRA_I2C_EINVAL;
    IOLock *setup = setupLock();
    if (!setup) return TEGRA_I2C_ENOMEM;
    IOLockLock(setup);
    int result = TEGRA_I2C_OK;
    if (!gI2C) {
        gCar = (volatile uint8_t *)ml_io_map(kCarPA, kCarSize);
        gPinmux = (volatile uint8_t *)ml_io_map(kPinmuxPA, kPinmuxSize);
        gApbMisc = (volatile uint8_t *)ml_io_map(kApbMiscPA, kApbMiscSize);
        gI2C = (gCar && gPinmux && gApbMisc) ? (volatile uint8_t *)ml_io_map(kI2CPA, kI2CSize) : nullptr;
    }
    if (!gLoop) gLoop = IOWorkLoop::workLoop();
    Bus &b = gBus[bus - 1];
    if (!gI2C || !gLoop) {
        result = TEGRA_I2C_ENOMEM;
    } else if (!b.ready) {
        if (!b.lock) b.lock = IOLockAlloc();
        if (!b.irqLock) b.irqLock = IOLockAlloc();
        if (!b.lock || !b.irqLock) {
            result = TEGRA_I2C_ENOMEM;
        } else {
            IOLockLock(b.lock);
            result = bringUp(bus, b);  // claims the bus's interrupt (sleeps until the GIC is up)
            b.ready = result == TEGRA_I2C_OK;
            IOLockUnlock(b.lock);
        }
    }
    IOLockUnlock(setup);
    if (result != TEGRA_I2C_OK) IOLog("TegraI2C: I2C%u init failed (%d)\n", bus, result);
    return result;
}

extern "C" int tegra_i2c_transfer(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx, size_t rxlen)
{
    if (bus < 1 || bus > kBuses || addr > 0x7f || (!txlen && !rxlen) || txlen > TEGRA_I2C_MAX_TRANSFER ||
        rxlen > TEGRA_I2C_MAX_TRANSFER || (txlen && !tx) || (rxlen && !rx)) {
        return TEGRA_I2C_EINVAL;
    }
    Bus &b = gBus[bus - 1];
    if (!b.lock) return TEGRA_I2C_ENOTREADY;
    IOLockLock(b.lock);
    int result = b.ready ? TEGRA_I2C_OK : TEGRA_I2C_ENOTREADY;
    for (unsigned attempt = 0; result == TEGRA_I2C_OK; ++attempt) {
        if (txlen) result = packet(bus, b, addr, (const uint8_t *)tx, nullptr, txlen, rxlen != 0);
        if (result == TEGRA_I2C_OK && rxlen) result = packet(bus, b, addr, nullptr, (uint8_t *)rx, rxlen, false);
        if (result == TEGRA_I2C_OK) break;
        recover(bus, b);
        if (result != TEGRA_I2C_EARB || !b.multiMaster || attempt == kArbRetries) break;
        IODelay(100);  // the other master's transfer (a few bytes at 400 kHz) finishes
        result = TEGRA_I2C_OK;
    }
    IOLockUnlock(b.lock);
    return result;
}

// System sleep (drivers/SwitchSuspend's sleep and wake actions; both on the same thread, IOCPUSleepKernel's). Suspend
// takes every ready bus's lock and keeps it, so a transfer started afterwards waits for resume instead of reaching a
// controller that SC7 is about to reset; that thread must not transfer again until it has called resume. Resume clocks
// and configures each bus again (after SC7 every controller and its CAR clock are back at reset, PWR_I2C included:
// U-Boot's setup is gone unless the BPMP firmware's SC7 exit re-clocked it, in which case it is adopted as at boot)
// and then lets the waiting transfers through.
extern "C" void tegra_i2c_suspend(void)
{
    for (unsigned bus = 1; bus <= kBuses; ++bus) {
        Bus &b = gBus[bus - 1];
        if (!b.ready) continue;
        IOLockLock(b.lock);
        if (b.irq) b.irq->disable();
        IOLockLock(b.irqLock);
        w32(b.regs, INT_MASK, 0);
        IOLockUnlock(b.irqLock);
    }
}

extern "C" void tegra_i2c_resume(void)
{
    for (unsigned bus = 1; bus <= kBuses; ++bus) {
        Bus &b = gBus[bus - 1];
        if (!b.ready) continue;
        const BusInfo &info = kBus[bus - 1];
        const bool adopted = bus == 5 && clockRunning(info);
        if (!adopted) {
            clockEnable(info);
            padConfig(info);
            if (bus == 5) b.hz = kClkM / info.divider / kBusDivide;
        }
        if (b.irq) b.irq->enable();
        const int result = adopted ? TEGRA_I2C_OK : configure(bus, b);
        if (adopted) {
            w32(b.regs, CNFG,
                r32(b.regs, CNFG) | CNFG_PACKET_MODE | CNFG_NEW_MASTER_FSM | (b.multiMaster ? CNFG_MULTI_MASTER : 0));
            (void)loadConfig(b);
        }
        if (result != TEGRA_I2C_OK) panic("TegraI2C: I2C%u did not come back after sleep (%d)", bus, result);
        IOLockUnlock(b.lock);
    }
}

// Shut Down / Restart only (TegraI2C.h). Polling is acceptable here and nowhere else: these are the boot's last
// transfers, nothing is left to interrupt, and the caller may have interrupts off. A failed packet only has the FIFOs
// flushed (no reset, no bus clear): the caller has nothing left to retry with but its fallback.
extern "C" int tegra_i2c_final_transfer(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx,
                                        size_t rxlen)
{
    if (bus < 1 || bus > kBuses || addr > 0x7f || (!txlen && !rxlen) || txlen > TEGRA_I2C_MAX_TRANSFER ||
        rxlen > TEGRA_I2C_MAX_TRANSFER || (txlen && !tx) || (rxlen && !rx)) {
        return TEGRA_I2C_EINVAL;
    }
    Bus &b = gBus[bus - 1];
    if (!b.lock || !b.irqLock || !b.ready) return TEGRA_I2C_ENOTREADY;
    const bool canSleep = preemption_enabled();
    if (!b.final) {
        // Keep the bus from here on: a transfer in flight finishes first (by its interrupt) when this thread may
        // sleep; with interrupts off a busy bus is taken as it is, since nothing else runs here to finish it.
        if (canSleep) {
            IOLockLock(b.lock);
        } else {
            (void)IOLockTryLock(b.lock);
        }
        b.final = true;
    }
    const bool irqHeld = canSleep ? (IOLockLock(b.irqLock), true) : IOLockTryLock(b.irqLock);
    b.active = false;  // a late action for this bus only masks and clears
    w32(b.regs, INT_MASK, 0);
    int result = TEGRA_I2C_OK;
    for (unsigned attempt = 0;; ++attempt) {
        if (txlen) result = pollPacket(b, addr, (const uint8_t *)tx, nullptr, txlen, rxlen != 0);
        if (result == TEGRA_I2C_OK && rxlen) result = pollPacket(b, addr, nullptr, (uint8_t *)rx, rxlen, false);
        if (result == TEGRA_I2C_OK) break;
        w32(b.regs, FIFO_CONTROL, r32(b.regs, FIFO_CONTROL) | FIFO_FLUSH);
        (void)waitClear(b, FIFO_CONTROL, FIFO_FLUSH, 2000);
        w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
        if (result != TEGRA_I2C_EARB || !b.multiMaster || attempt == kArbRetries) break;
        IODelay(100);  // the other master's transfer (a few bytes at 400 kHz) finishes
        result = TEGRA_I2C_OK;
    }
    if (irqHeld) IOLockUnlock(b.irqLock);
    return result;
}

extern "C" int tegra_i2c_read(unsigned bus, uint8_t addr, uint8_t reg, void *buf, size_t len)
{
    if (!len) return TEGRA_I2C_EINVAL;
    return tegra_i2c_transfer(bus, addr, &reg, 1, buf, len);
}

extern "C" int tegra_i2c_write(unsigned bus, uint8_t addr, uint8_t reg, const void *buf, size_t len)
{
    if (len > TEGRA_I2C_MAX_REG_WRITE || (len && !buf)) return TEGRA_I2C_EINVAL;
    uint8_t data[TEGRA_I2C_MAX_REG_WRITE + 1];
    data[0] = reg;
    if (len) memcpy(data + 1, buf, len);
    return tegra_i2c_transfer(bus, addr, data, len + 1, nullptr, 0);
}
