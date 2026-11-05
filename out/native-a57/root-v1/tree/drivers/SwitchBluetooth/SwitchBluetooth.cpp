// SPDX-License-Identifier: BSD-2-Clause-Patent
// Switch OLED Bluetooth: the BCM4356A3's Bluetooth core on UART-D, brought up here and handed to iOS's Bluetooth
// daemons as the H4 byte stream they open on an iPad. Opt-in with boot-arg switch-bt=1; see SwitchBluetooth.h.
//
// Hardware (switchroot tegra210b01-nx-common.dtsi / tegra210b01-fric-{gpio,pinmux}.dtsi, the OLED DTB):
// serial@70006300 is "nvidia,tegra114-hsuart" with a "brcm,bcm43438-bt" child: max-speed 3000000, RTS/CTS on
// uart4_{tx,rx,rts,cts}_pi4..7 (function uartd), shutdown-gpios PH4 (BT_REG_ON, active high), device-wakeup-gpios
// PH3 (active low), PH5 the chip's host wake (input). Interrupt: GIC SPI 90, level high = INTID 122 (Linux
// tegra210.dtsi:655 `interrupts = <GIC_SPI 90 IRQ_TYPE_LEVEL_HIGH>`; OLED DTB serial@70006300 `<0x00 0x5a 0x04>`;
// MyEcoria/nxk user/bluetooth "UART-D interrupt: SPI 90").
//
// Bring-up (MyEcoria/nxk user/bluetooth/src/{main,bcm,uart}.rs, proven on a Switch; Linux hci_bcm.c / btbcm.c):
// BT_REG_ON low 50 ms, high, 200 ms; HCI Reset at 115200; Broadcom 0xFC18 to 3 Mbaud; 0xFC2E download minidriver,
// 50 ms; the .hcd's own records (0xFC4C Write_RAM ... 0xFC4E Launch_RAM); 500 ms; the patched firmware restarts
// at 115200: Reset, 0xFC18 to 3 Mbaud again, Reset; 0xFC27 sleep mode off (BT_WAKE stays asserted); 0xFC01 BD
// address. A Hardware Error event means only a power cycle helps (nxk: Erista's first bring-up after a cold boot
// gets one for seconds), so the first bring-up retries with 1-8 s pauses.
//
// Why the kernel does the firmware (path "b"), from 21H461's /usr/sbin/BlueTool and /usr/sbin/bluetoothd: on an
// iPad, bluetoothd asks BlueTool (XPC "RunBuiltinScript" {script: boot}) to power and patch the chip. BlueTool picks a
// compiled-in boot function and .hcd from a 247-entry table keyed on MobileGestalt ProductHash, the chip it derives
// from HCI Read Local Version's LMP subversion (BCM4355C0, 4350C5, 4357B1, ...; anything else is "BCM") and
// MobileGestalt WifiVendor. There is no BCM4356A3 entry, and its /etc/bluetool override scripts are read only on
// Vendor/Internal release types. So the controller must arrive ready.
//
// UART-D (Linux serial-tegra.c, T210 = tegra30_uart_chip_data: TX FIFO full in LSR bit 8, no TX FIFO reset in FIFO
// mode; hekate bdk/soc/uart.c and clock.c for the clock): a 16550 with 32-byte FIFOs and hardware RTS/CTS
// (MCR CTS_EN | RTS_EN). Clock source PLLP 408 MHz: 3 Mbaud through the UART fractional divider (8.5 -> 48 MHz,
// divisor latch 1), 115200 from undivided PLLP with divisor latch 221 (hekate clock_uart_use_src_div, uart_init;
// nxk notes Erista needed 115200 outside the 16x divider mode). Everything moves by interrupt: an
// IOFilterInterruptEventSource drains the RX FIFO into a ring and refills the TX FIFO from another (THRI at the
// 16-byte trigger) in primary context; its work loop action hands received bytes to the bring-up's HCI parser or to
// the connected client and wakes waiting writers. A full RX ring masks the RX interrupts; the FIFO then fills and
// RTS stops the chip, so nothing is lost. A wait that times out while the UART shows a pending, enabled condition
// for 50 ms more is a lost interrupt and panics with one line.
// The filter never leaves a condition it cannot clear asserted: a line held low (a core that is not driving its TX,
// so breaks), a stuck BI/FIFOE that comes straight back after the FIFO reset, an IIR condition that survives
// kFilterRounds, or a line that re-fires with IIR showing nothing (kIdleStorm; first cleared as serial-tegra clears
// latched interrupts, clearInterruptsLocked, and masked if it comes back) masks RX (maskRxFaultLocked) until the
// next flush (every bring-up step, every connect) or send (kickTx). Before r8 such a line re-fired the moment the
// filter returned, pinned CPU0 and froze the thread there for good, often holding the work-loop gate: stackshots
// stacks-2026-11-04-045010 (r6k) and -055657 (r7a, "BT Stack Stuck") show bluetoothd's connect 30 s on-core in
// ml_set_interrupts_enabled under the flush's runAction; r7b-r7d's first bring-up never ended (no line at all) and
// every connect timed out after kConnectWaitMs.
//
// Userland interface: kernel control "com.apple.uart.bluetooth" (one client), SOCK_STREAM, raw H4 both ways;
// TIOCGETA / TIOCSETA as SYSPROTO_CONTROL socket options carry a struct termios whose speed is applied to the UART
// (flow control stays on). That is what BlueTool's "device -D" opens (FUN_10001e988) and what bluetoothd's POSIX
// HCI driver opens when named (FUN_1000ddaa8); bluetoothd's unnamed default is the Skywalk variant
// "com.apple.uart.sk.bluetooth", so kern_exec.c names this one in bluetoothd's BT_HCI_TRANSPORT (switch-bt=1). Its
// chip-boot requests go to the BlueTool stand-in (BlueTool/BlueTool.c, shown at /usr/sbin/BlueTool from the Data
// overlay), which reports success: the work is done here. A client that sent anything leaves the chip in an unknown
// state (an original BlueTool would push its own baud rate and firmware), so the next connect brings the chip up
// again.
//
// BD address: the unit's own from the fuses (publishAddress), or Horizon's when deploy staged hekate's "Dump Joy-Con BT"
// (switchroot/joycon_mac.ini on the FAT, read there by tools/deploy/joycon_pairing.py and written to Data as
// kStagedAddressPath: 6 bytes, display order), as switchroot's U-Boot sets the controller to the host address in
// joycon_mac.bin so that Joy-Cons paired in Horizon reconnect without being paired again. bluetoothd must see one
// address for its whole life: it reads local-mac-address at its start (21H461 FindLocalMacAddress, "Got
// local-mac-address", before it connects) and the controller's (Read BD_ADDR) once its stack is up, and every classic
// link key it keeps carries the local address it was stored under. When its stack starts it drops each key stored
// under another one ("We have a link key for the wrong local device", "Not link key, result 150"), and a link key
// request for that device then finds none. Until r14 the staged address replaced the fuses' only at bluetoothd's
// first connect, after it had read local-mac-address: every boot's stack start dropped the Joy-Con and AirPods keys
// stored under the other address (two or three a boot in r7r's logs), and only the keys joyconpaird imported after
// that check were there for the boot. r7r 11:02:51 dropped the right Joy-Con's (imported
// just before) and kept the left's (just after); at 11:07:43-:46 the right one connected four times and dropped the
// link within 330 ms without authenticating, at :49 its link key request found no key, and after it was put back on
// the rail (re-import, 11:07:57) its key was found. So the staged address is published once Data is mounted, before
// launchd (switch_bluetooth_data_mounted, called by SwitchNVRAM's hook in load_init_program): bluetoothd reads it at
// its start, and its first connect writes it to the controller (0xFC01) before the stream is handed over. Every
// connect then hands the controller's address to the rail Joy-Con driver, which pairs rail Joy-Cons to it
// (switch_joycon_bluetooth_address, ../SwitchJoyCon/SwitchJoyCon.h).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/fcntl.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <sys/vnode.h>
#include <string.h>
#include "SwitchBluetooth.h"
#include "SwitchBluetoothLinks.h"
#include "../ArmGIC/IOSwitchInterrupts.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

// The rail Joy-Con driver's strong definition wins when it is linked (../SwitchJoyCon/SwitchJoyCon.h).
extern "C" __attribute__((weak)) void switch_joycon_bluetooth_address(const uint8_t[6]) {}

namespace {
constexpr vm_offset_t kUartPA = 0x70006000, kUartD = 0x300;  // UART-A page; UART-D at +0x300 (hekate uart.c)
constexpr vm_offset_t kCarPA = 0x60006000, kGpioPA = 0x6000D000, kPinmuxPA = 0x70003000, kFusePA = 0x7000F000;
constexpr uint32_t kIntid = 32 + 90;
constexpr uint32_t kPllpHz = 408000000;
constexpr uint32_t kInitBaud = 115200, kFastBaud = 3000000;
constexpr const char *kControlName = "com.apple.uart.bluetooth";
constexpr const char *kStagedAddressPath = "/private/var/ioswitch/bluetooth/host-address";  // see the header

// Clock and reset controller (hekate bdk/soc/clock.h, clock.c _clock_uart[3]): UARTD is bit 1 of the U registers.
enum : uint32_t {
    RST_DEVICES_U = 0x00c,
    CLK_OUT_ENB_U = 0x018,
    CLK_SOURCE_UARTD = 0x1c0,
    RST_DEV_U_SET = 0x310,
    RST_DEV_U_CLR = 0x314,
    CLK_ENB_U_SET = 0x330,
};
constexpr uint32_t kUartDBit = 1u << 1;
// The fuse block is readable only while clocked (CLK_OUT_ENB_H bit 7) and visible (MISC_CLK_ENB bit 28); an
// unclocked read stalls the bus (hekate clock_enable_fuse; SwitchWLANProbe.cpp switch_wlan_mac).
enum : uint32_t { CLK_OUT_ENB_H = 0x014, MISC_CLK_ENB = 0x048 };
constexpr uint32_t kFuseClock = 1u << 7, kFuseVisible = 1u << 28;
constexpr uint32_t kFuseLotCode0 = 0x800 + 0x208;  // FUSE_OPT_LOT_CODE_0 (FUSE_BASE 0x7000F800 + 0x208)
constexpr uint32_t kUartDivEnable = 1u << 24;      // source bits 31:29 = 0 (PLLP); divider is 7.1 fixed point minus 1

// UART registers (4 bytes apart) and bits: Linux serial_reg.h + serial-tegra.c, hekate bdk/soc/uart.h.
enum : uint32_t {
    THR = 0x00,
    IER = 0x04,
    IIR = 0x08,
    FCR = 0x08,
    LCR = 0x0c,
    MCR = 0x10,
    LSR = 0x14,
    MSR = 0x18,
    SPR = 0x1c,
    IRDA_CSR = 0x20
};
enum : uint32_t {
    IER_RDI = 1u << 0,
    IER_THRI = 1u << 1,
    IER_RLSI = 1u << 2,
    IER_RTOIE = 1u << 4,  // RTOIE: Tegra RX timeout
    IER_RX = IER_RDI | IER_RLSI | IER_RTOIE,
    IIR_NO_INT = 1u << 0,
    FCR_ENABLE = 1u << 0,
    FCR_CLEAR_RX = 1u << 1,
    FCR_CLEAR_TX = 1u << 2,
    FCR_TX_TRIG_16 = 0u << 4,  // Tegra's TX trigger runs backwards: 0 = interrupt at 16 bytes left
    FCR_RX_TRIG_8 = 2u << 6,   // RX data interrupt above 8 bytes; the timeout one takes the rest
    LCR_8N1 = 3u,
    LCR_DLAB = 1u << 7,
    MCR_RTS = 1u << 1,
    MCR_CTS_EN = 1u << 5,
    MCR_RTS_EN = 1u << 6,
    LSR_DR = 1u << 0,
    LSR_ERRORS = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4),  // OE PE FE BI
    LSR_BI = 1u << 4,
    LSR_THRE = 1u << 5,
    LSR_TEMT = 1u << 6,
    LSR_FIFOE = 1u << 7,  // an errored character in the RX FIFO
    LSR_TXFIFO_FULL = 1u << 8,
    MSR_CTS = 1u << 4,
};
enum : uint32_t { ID_MODEM = 0, ID_TX = 1, ID_RX = 2, ID_LINE = 3, ID_EORD = 4, ID_RX_TIMEOUT = 6 };  // (IIR >> 1) & 7
constexpr uint32_t kFifoBytes = 32;
constexpr uint32_t kIrqRxBudget =
    64;  // RX bytes one interrupt moves before masking RX and handing the rest to the work loop
constexpr uint32_t kFilterRounds = 32;  // IIR conditions one interrupt services; more is one it cannot clear
constexpr uint32_t kFaultRun = 2;       // break / stuck-error events with no good byte between them that mask RX
// Interrupts in a row that find nothing to do, within kIdleStormNs: the line re-fires straight away with IIR showing
// nothing (one such entry after a real interrupt is the level line's deassert latency; ~1 ms apart is normal too).
constexpr uint32_t kIdleStorm = 64, kIdleStormNs = 1000000;

// GPIO (TegraGPIO.h numbering, gpio = port * 8 + pin) and its per-port registers; the masked copies at +0x80
// (bits 15:8 select, 7:0 value) change one pin without a read-modify-write, so TegraGPIO's pins are never touched.
constexpr unsigned PH3 = 7 * 8 + 3, PH4 = 7 * 8 + 4, PH5 = 7 * 8 + 5, PI4 = 8 * 8 + 4;
enum : uint32_t { GPIO_CNF = 0x00, GPIO_OE = 0x10, GPIO_OUT = 0x20, GPIO_IN = 0x30, GPIO_MSK = 0x80 };
inline uint32_t portReg(unsigned gpio, uint32_t reg)
{
    return (gpio >> 5) * 0x100 + ((gpio >> 3) & 3) * 4 + reg;
}

// Pinmux, low 7 bits of PINMUX_AUX (function 1:0, pull 3:2, tristate 4, park 5 cleared, input 6), as nxk's
// Pads::setup with the switchroot DT values (fric-pinmux.dtsi lines 750-780 and 1149-1173).
enum : uint32_t { PM_PULL_DOWN = 1u << 2, PM_TRISTATE = 1u << 4, PM_INPUT = 1u << 6, PM_LOW = 0x7f };
struct Pad {
    uint16_t offset;
    uint8_t value;
};
constexpr Pad kPads[] = {
    {0x114, 0},                                      // uart4_tx_pi4: uartd, driven
    {0x118, PM_PULL_DOWN | PM_TRISTATE | PM_INPUT},  // uart4_rx_pi5
    {0x11c, 0},                                      // uart4_rts_pi6
    {0x120, PM_PULL_DOWN | PM_TRISTATE | PM_INPUT},  // uart4_cts_pi7
    {0x1c0, PM_PULL_DOWN | PM_INPUT},                // ap_wake_bt_ph3 (input on: GPIO_IN reads back the level driven)
    {0x1c4, PM_PULL_DOWN | PM_INPUT},                // bt_rst_ph4
    {0x1c8, PM_TRISTATE | PM_INPUT},                 // bt_wake_ap_ph5
};

// HCI (Bluetooth Core spec; Broadcom vendor commands as Linux btbcm.c / hci_bcm.c).
enum : uint16_t {
    OP_RESET = 0x0c03,
    OP_READ_LOCAL_NAME = 0x0c14,
    OP_READ_VERSION = 0x1001,
    OP_BCM_WRITE_BDADDR = 0xfc01,
    OP_BCM_UART_BAUD = 0xfc18,
    OP_BCM_SLEEP_MODE = 0xfc27,
    OP_BCM_MINIDRIVER = 0xfc2e,
    OP_BCM_WRITE_RAM = 0xfc4c,
    OP_BCM_LAUNCH_RAM = 0xfc4e,
};
enum : uint8_t { H4_COMMAND = 1, H4_ACL = 2, H4_SCO = 3, H4_EVENT = 4 };
enum : uint8_t { EV_COMMAND_COMPLETE = 0x0e, EV_COMMAND_STATUS = 0x0f, EV_HARDWARE_ERROR = 0x10 };
enum : uint8_t { EV_NUM_COMPLETED = 0x13, EV_LE_META = 0x3e, EV_VENDOR = 0xff, LE_ADV_REPORT = 0x02 };
enum : uint8_t { LE_CONN_COMPLETE = 0x01, LE_ENH_CONN_COMPLETE = 0x0a };
// LE commands bluetoothd's own (Apple-chip) choices meet on this controller (BLE section in the client-mode comments).
enum : uint16_t {
    OP_SET_EVENT_MASK = 0x0c01,
    OP_SET_EVENT_MASK_2 = 0x0c63,
    OP_WRITE_LE_HOST_SUPPORT = 0x0c6d,
    OP_LE_SET_ADV_PARAMS = 0x2006,
    OP_LE_SET_SCAN_ENABLE = 0x200c,
    OP_BCM_LE_META = 0xfce9,           // Broadcom LE meta VSC: sub-operation first (bluetoothd _BCM_LEMeta*)
    OP_BCM_MULTI_ADV_PARAMS = 0xfd4a,  // bluetoothd _BCM_SetMultipleAdvInstanceParams
    OP_ACCEPT_CONNECTION = 0x0409,     // Accept Connection Request {address, role} (SwitchBluetoothLinks.h)
};
// 0xFCE9 sub-operations not passed on (BLE section): enable features, PCF feature selection, PCF service UUID (and
// its V2/V3 forms).
constexpr uint8_t kRefusedLeMeta[] = {0x01, 0x05, 0x07, 0x50, 0x61};
constexpr uint16_t kAdvIntervalMinNonConnectable = 0x00a0;  // 100 ms: Core 4.x, LE Set Advertising Parameters
constexpr uint8_t kHciVersion50 = 9;                        // Core 5.0 dropped that minimum
// Client-mode completion fix (frameRx): the one command that has no completion event, the LE controller OGF, the
// vendor OGF, and the status bluetoothd already copes with for vendor commands this controller lacks.
constexpr uint16_t OP_HOST_NUMBER_OF_COMPLETED_PACKETS = 0x0c35;
constexpr uint16_t kOgfLe = 0x08, kOgfVendor = 0x3f;
constexpr uint8_t kUnknownCommand = 0x01;
constexpr uint32_t kMaxPending = 16;
constexpr uint16_t kBroadcom = 0x000f;

constexpr uint32_t kPowerOffMs = 50, kPowerOffAfterErrorMs = 500, kPowerOnMs = 200;  // nxk POWER_OFF/ON_MS
constexpr uint32_t kMinidriverMs = 50, kLaunchMs = 500;                              // btbcm, nxk LAUNCH_MS
constexpr uint32_t kCommandMs = 500, kRecordMs = 1000, kGraceMs = 50;
constexpr uint32_t kWriteMs = 2000, kConnectWaitMs = 20000;
constexpr uint32_t kRetrySeconds[] = {1, 2, 4, 8};
// A bring-up attempt takes about a second and, every wait in it being bounded, ~20 s at worst; a connect less. One
// still running after this has stopped for a reason the driver does not know, and says so once (watch()).
constexpr uint32_t kStuckMs = 60000;
constexpr uint32_t kRingBytes = 16384;
constexpr uint32_t kStatsMs = 10000;  // the stats line: at most this often, and only after traffic (logStats)
constexpr uint32_t kTraceEntries = 80, kTraceParams = 24, kTraceReturn = 12;
constexpr uint32_t kLinkTraceLines = 600;  // link trace lines a boot (os_log; "Links" section)
// Traced once a boot with their completions besides the LE and vendor commands: Read Local Supported Features, Read
// Buffer Size (the ACL and SCO buffers every link shares), Read BD_ADDR (the address bluetoothd takes from the
// controller), controller-to-host flow control and Host Buffer Size, Write Simple Pairing Mode, Write Secure
// Connections Host Support, Write Class Of Device, Write Page Timeout, Write Connection Accept Timeout, Write Page
// Scan Type, Write Inquiry Scan Type.
constexpr uint16_t kTracedSetup[] = {0x1003, 0x1005, 0x1009, 0x0c31, 0x0c33, 0x0c56,
                                     0x0c7a, 0x0c24, 0x0c18, 0x0c16, 0x0c47, 0x0c43};

inline uint32_t r32(volatile uint8_t *base, uint32_t o)
{
    return *(volatile uint32_t *)(base + o);
}
inline void w32(volatile uint8_t *base, uint32_t o, uint32_t v)
{
    *(volatile uint32_t *)(base + o) = v;
}
inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

// A .hcd is HCI commands back to back (opcode, length, parameters): Write_RAM records, Launch_RAM last.
bool validPatch(const uint8_t *p, uint32_t size)
{
    uint32_t at = 0, records = 0;
    uint16_t op = 0;
    while (at + 3 <= size) {
        op = le16(p + at);
        if (op != OP_BCM_WRITE_RAM && op != OP_BCM_LAUNCH_RAM) return false;
        at += 3u + p[at + 2];
        ++records;
    }
    return records && at == size && op == OP_BCM_LAUNCH_RAM;
}

// Single producer, single consumer; indices run free and are masked on use.
struct Ring {
    uint8_t *data;
    uint32_t head, tail;
    uint32_t used() const
    {
        return __atomic_load_n(&head, __ATOMIC_ACQUIRE) - __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
    }
    uint32_t space() const { return kRingBytes - used(); }
};

// H4 framing of what the controller sends while the kernel itself talks to it.
struct H4Parser {
    uint8_t state;  // 0 packet type, 1 header, 2 payload
    uint8_t type, need, have;
    uint8_t header[4];
    uint16_t length, got;
    uint8_t payload[255];
};

enum State { kStarting, kBusy, kReady, kFailed };
enum Mode { kIdle, kKernel, kClient };  // who receives the controller's bytes
}

class SwitchBluetooth : public IOService {
    OSDeclareDefaultStructors(SwitchBluetooth)

        volatile uint8_t *uart = nullptr,
                         *car = nullptr, *gpio = nullptr, *pinmux = nullptr;
    IOWorkLoop *loop = nullptr;
    IOFilterInterruptEventSource *irq = nullptr;
    IOSimpleLock *hwLock = nullptr;  // UART registers (DLAB, IER) against the filter
    IOLock *stateLock = nullptr, *hciLock = nullptr, *txLock = nullptr;
    Ring rx = {}, tx = {};
    uint32_t ier = 0, baud = 0;
    bool chipOn = false;              // BT_REG_ON high; RX interrupts are only on while it is (hwLock)
    bool rxStalled = false;           // RX interrupts masked: ring full or an interrupt's budget spent (hwLock)
    bool rxFault = false;             // RX masked until the next flush or send: see maskRxFaultLocked (hwLock)
    uint32_t rxFaultRun = 0;          // break / stuck-error events since the last good byte (hwLock)
    volatile bool txWanted = false;   // a writer waits for ring space or an empty ring
    volatile bool actionDue = false;  // the filter asked for the action; the line stays masked until it has run
    uint32_t irqs = 0, rxBytes = 0, txBytes = 0, lineErrors = 0, dropped = 0, resyncs = 0, hardwareErrors = 0;
    uint32_t idleIrqs = 0, rxResets = 0;  // interrupts that moved no byte; RX FIFO resets for a stuck BI/FIFOE
    uint32_t rxFaults = 0;                // times RX was masked for a fault
    uint32_t idleRun = 0;                 // idle interrupts in a row (hwLock), the first at idleSince
    uint64_t idleSince = 0, idleStormAbs = 0;
    bool stormCleared = false;       // a storm got clearInterruptsLocked; the next one masks (hwLock)
    const char *lastFault = "none";  // what the last RX fault was, for describe() and a failed bring-up
    // One work loop wakeup per controller packet in client mode (filter): the H4 framing of the bytes the filter puts
    // in the ring, kept in step with frameRx's by syncFramerLocked (hwLock).
    struct {
        uint8_t state, type, have, need;  // 0 packet type, 1 header, 2 payload
        uint8_t header[4];
        uint32_t left;
        bool lost;  // a line error or dropped bytes: every interrupt wakes the work loop until the next sync
    } rxFrame = {};
    bool rxPacketDone = false;  // a packet ended in the ring since the last wakeup
    bool framePackets = false;  // client mode: wake the work loop at packet ends only
    void frameByteLocked(uint8_t byte);
    void syncFramerLocked();

    // Kernel HCI (bring-up), under hciLock.
    H4Parser h4 = {};
    struct {
        uint16_t opcode;
        bool waiting, done, hardwareError;
        uint8_t length;
        uint8_t data[255];  // return parameters, status first
    } cmd = {};

    // Userland side (stateLock), mode under the work loop gate.
    kern_ctl_ref control = nullptr;
    State state = kStarting;
    Mode mode = kIdle;
    uint32_t client = 0;
    bool clientUsed = false, chipDirty = false, clientBlocked = false;
    struct termios tio = {};

    // Client-mode HCI bookkeeping (frameRx): the commands the client has sent and not yet seen completed, from its H4
    // stream (noteClientTx; cmdLock), and the framing of the controller's stream, so that a Command Complete or Status
    // is looked at before it goes out (work loop).
    IOSimpleLock *cmdLock = nullptr;
    uint16_t sentCmds[kMaxPending] = {};
    uint32_t sentCount = 0;
    struct {
        uint8_t state, type, have, need;
        uint8_t header[4];
        uint32_t left;
    } txFrame = {};
    uint32_t rxPacketLeft = 0;  // bytes of the controller's packet being passed on still to go
    bool framingLost = false;   // either stream stopped parsing as H4: everything goes through untouched
    bool rxDrop = false;        // the packet being taken is a completion the client did not ask for: not passed on
    bool sentOverflow = false;  // a command left `sentCmds` unanswered (cmdLock): unknown completions pass as they come
    uint32_t completionFixes = 0, strayDrops = 0;
    uint16_t strayOps[8] = {};  // opcodes of the stray completions logged this boot
    uint32_t strayOpCount = 0;
    void resetClientFraming();
    size_t noteClientTx(const uint8_t *bytes, size_t length);
    void noteCommandLocked(uint16_t opcode);
    bool frameRx(uint32_t avail);
    bool takePendingLocked(uint16_t opcode);
    uint16_t takeVendorLocked();

    // BLE (client-mode comments): whole commands looked at before they go out, the answers given here, the trace and
    // the stats line.
    uint8_t hciVersion = 0;      // Read Local Version's HCI version at the last bring-up (0 unknown)
    uint8_t lastCredits = 1;     // the controller's Num_HCI_Command_Packets in its last completion (work loop)
    uint8_t answers[4][7] = {};  // Command Complete events made here, not yet passed on (cmdLock)
    uint32_t answerHead = 0, answerTail = 0;
    bool advFixLogged = false;
    struct Traced {
        uint16_t opcode;
        uint8_t sub;     // 0xFCE9's sub-operation, else 0
        uint8_t state;   // 1 sent, waiting for its completion; 2 logged
        uint8_t length;  // parameter bytes the command had
        uint8_t params[kTraceParams];
    } traced[kTraceEntries] = {};
    uint32_t tracedCount = 0;  // cmdLock
    struct Stats {
        uint32_t actions;                                // runs of the interrupt action (work loop wakeups)
        uint32_t events, acl, sco;                       // controller packets passed to the client
        uint32_t cc, cs, nocp, le, vendor, otherEvents;  // events by code
        uint32_t leAdv, leAdvReports, leConn, leOther;   // LE meta events by sub-event
        uint32_t commands, leCommands, vendorCommands;   // client commands
        uint32_t answered, advFixed, scanOn, scanOff;    // answered here, intervals raised, LE scan enables
        uint32_t central;                                // gamepads accepted as central (acceptAsCentral)
    } st = {}, stLogged = {};
    struct Failed {
        uint16_t opcode;
        uint8_t status;
        uint32_t count;
    } failed[8] = {}, failedLogged[8] = {};  // completions with a non-zero status, by opcode (work loop)
    uint32_t irqsLogged = 0, rxBytesLogged = 0, txBytesLogged = 0;
    uint64_t statsAt = 0;      // mach_absolute_time of the last stats line
    uint64_t statsWindow = 0;  // kStatsMs in absolute time units
    errno_t clientCommand(uint8_t *packet, size_t size);
    bool answerHere(uint16_t opcode, const uint8_t *params, uint8_t length);
    bool fixAdvertising(uint16_t opcode, uint8_t *params, uint8_t length);
    void traceCommand(uint16_t opcode, const uint8_t *params, uint8_t length, bool answered);
    void traceCompletion(uint16_t opcode, bool status, uint8_t result, const uint8_t *ret, uint32_t retLength);
    void countCompletion(uint16_t opcode, uint8_t result);
    bool passAnswers();
    bool clientRoom(size_t need);
    void logStats(bool force);

    // Links ("Links" section, SwitchBluetoothLinks.h): ACL packets per connection handle (cmdLock) and the link trace.
    switch_bt_links links = {};
    uint8_t roleAddr[6] = {};  // the last Role Change's address and role, for a link whose Connection Complete follows
    uint8_t role = SWITCH_BT_ROLE_UNKNOWN;
    uint32_t linkLines = 0;            // link trace lines logged this boot (atomic)
    switch_bt_requests requests = {};  // incoming Connection Requests until their Accept (cmdLock)
    bool centralLogged = false;
    bool acceptAsCentral(uint8_t *params, uint8_t length);
    // Scratch for the work loop's link event and stats line (both under the work-loop gate: action or runAction), off
    // the stack of whichever thread runs deliver() (it can be bluetoothd's, through ctlSend or ctlRcvd).
    uint8_t linkParams[255] = {};
    char linkText[200] = {};
    switch_bt_links statsLinks = {};
    char statsHeld[520] = {};
    char statsLine[1024] = {};
    void linkEvent(uint8_t code, uint32_t length);
    void linkCommand(uint16_t opcode, const uint8_t *params, uint8_t length);
    void linkCompletion(uint16_t opcode, bool status, uint8_t result);
    void linkLog(const char *direction, const char *line, const switch_bt_link *last = nullptr);

    OSData *patch = nullptr;
    uint8_t address[6] = {};
    bool haveAddress = false;
    const char *addressFrom = "ADT";
    uint8_t chipAddress[6] = {};  // what the controller was last given (Write BD_ADDR), valid with chipAddressSet
    bool chipAddressSet = false;
    bool stagedChecked = false;  // stateLock: the staged address was looked for (first connect)
    bool adoptStagedAddress();
    void publishAddressProperties();

    // A bring-up or connect that outlives kStuckMs (watch / unwatch) logs one line; a thread call, so a work loop or
    // gate that is itself stuck cannot hold it up.
    thread_call_t stuckCall = nullptr;
    static void stuck(thread_call_param_t owner, thread_call_param_t what);
    void watch(const char *what);
    void unwatch();

    // Registers and pins.
    void setPin(unsigned pin, uint32_t reg, bool on)
    {
        w32(gpio, portReg(pin, reg) + GPIO_MSK, (1u << (pin & 7)) << 8 | (on ? 1u << (pin & 7) : 0));
    }
    bool pin(unsigned p, uint32_t reg) const { return r32(gpio, portReg(p, reg)) & (1u << (p & 7)); }
    void setupPins();
    // BT_REG_ON. RX interrupts follow the core's power: an unpowered core leaves RX on its pull-down, a break that keeps
    // the line-status interrupt asserted on CPU0 (r6k: bluetoothd's re-bring-up spun 30 s in the flush after power-on).
    // The flush after power-on (gated 0) turns them back on.
    void power(bool on)
    {
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
        chipOn = on;
        if (!on) {
            ier &= ~IER_RX;
            rxStalled = false;
            w32(uart, IER, ier);
        }
        IOSimpleLockUnlockEnableInterrupt(hwLock, s);
        setPin(PH4, GPIO_OUT, on);
    }
    void resetRxFifoLocked();
    void maskRxFaultLocked(const char *what);
    bool rxFaultEventLocked(const char *what);
    void clearInterruptsLocked();
    void setupUart();
    void setBaudLocked(uint32_t rate);
    void setHostBaud(uint32_t rate);
    void describe(char *buffer, size_t size);

    // Interrupt path.
    static bool filter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void action(OSObject *owner, IOInterruptEventSource *sender, int count);
    bool drainRx(uint32_t &budget, bool stallWhenSpent);
    bool resumeRx();
    bool fillTx();
    void kickTx();
    void deliver();
    void checkDelivery(const char *what);

    // Transmit and the kernel's own HCI.
    int transmit(const uint8_t *bytes, size_t length, uint32_t timeoutMs);
    int drainTx(uint32_t timeoutMs);
    void parse(const uint8_t *bytes, uint32_t length);
    void event(uint8_t code, const uint8_t *params, uint8_t length);
    int command(uint16_t opcode, const uint8_t *params, uint8_t length, uint32_t timeoutMs, uint8_t *out = nullptr,
                uint8_t *outLength = nullptr);
    int resetAt(uint32_t rate, unsigned tries);
    bool fast();
    bool bringUp(uint32_t offMs, char *why, size_t whySize);

    // Work loop gated actions.
    static IOReturn gated(OSObject *owner, void *what, void *, void *, void *);
    void flushRx();
    void setMode(Mode m);

    // Kernel control.
    static errno_t ctlConnect(kern_ctl_ref ref, struct sockaddr_ctl *sac, void **unitinfo);
    static errno_t ctlDisconnect(kern_ctl_ref ref, u_int32_t unit, void *unitinfo);
    static errno_t ctlSend(kern_ctl_ref ref, u_int32_t unit, void *unitinfo, mbuf_t m, int flags);
    static errno_t ctlSetopt(kern_ctl_ref ref, u_int32_t unit, void *unitinfo, int opt, void *data, size_t len);
    static errno_t ctlGetopt(kern_ctl_ref ref, u_int32_t unit, void *unitinfo, int opt, void *data, size_t *len);
    static void ctlRcvd(kern_ctl_ref ref, u_int32_t unit, void *unitinfo, int flags);

public:
    bool start(IOService *provider) override;
    void run();
};

OSDefineMetaClassAndStructors(SwitchBluetooth, IOService)

    namespace
{
    SwitchBluetooth *gDriver;
    bool gAddressFromFuses;

    // The staged address (see the header, "BD address"), looked for once Data is mounted, before launchd
    // (switch_bluetooth_data_mounted): kStagedNone until then and when there is none, kStagedFound with it in gStaged.
    enum { kStagedUnread, kStagedNone, kStagedFound };
    int gStagedState = kStagedUnread;  // atomic; gStaged is written before kStagedFound is stored (release)
    uint8_t gStaged[6];

    // kStagedAddressPath, read with the kernel's context: true with a unicast address in `out`. A missing file is the
    // normal case (no dump staged); anything else is one line and no staged address.
    bool readStagedAddress(uint8_t out[6])
    {
        vfs_context_t ctx = vfs_context_kernel();
        vnode_t vp = NULLVP;
        const int opened = vnode_open(kStagedAddressPath, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
        if (opened) {
            if (opened != ENOENT) IOLog("SwitchBluetooth: %s not opened (%d)\n", kStagedAddressPath, opened);
            return false;
        }
        uint8_t staged[7] = {};
        int resid = 0;
        const int error = vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(staged), sizeof(staged), 0, UIO_SYSSPACE,
                                  IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        vnode_close(vp, FREAD, ctx);
        static const uint8_t zero[6] = {}, ones[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        // Exactly 6 bytes (one more asked for, so a longer file shows), a unicast address.
        if (error || resid != 1 || !memcmp(staged, zero, 6) || !memcmp(staged, ones, 6) || (staged[0] & 1)) {
            IOLog("SwitchBluetooth: %s is not a BD address (error %d, %d bytes); ignored\n", kStagedAddressPath, error,
                  (int)sizeof(staged) - resid);
            return false;
        }
        memcpy(out, staged, 6);
        return true;
    }

    // Where bluetoothd (/arm-io/bluetooth local-mac-address) and MobileGestalt (/chosen mac-address-bluetooth0) read
    // the address.
    void publishAddressBytes(const uint8_t bytes[6])
    {
        OSData *value = OSData::withBytes(bytes, 6);
        IORegistryEntry *node = IORegistryEntry::fromPath("/arm-io/bluetooth", gIODTPlane);
        IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
        if (value && node) node->setProperty("local-mac-address", value);
        if (value && chosen) chosen->setProperty("mac-address-bluetooth0", value);
        OSSafeReleaseNULL(chosen);
        OSSafeReleaseNULL(node);
        OSSafeReleaseNULL(value);
    }

    // The unit's own BD address, as U-Boot derives device_bt_mac (switchroot board/nintendo/switch/nintendo-switch.c
    // generate_and_set_serial): Nintendo's OUI 98:B6:E9, bits 23:16 and 15:8 of FUSE_OPT_LOT_CODE_0, its low byte + 1
    // (+2 is Wi-Fi's). It replaces the packager's shared fallback where bluetoothd (/arm-io/bluetooth
    // local-mac-address) and MobileGestalt (/chosen mac-address-bluetooth0) read it. Runs in switch_bluetooth_start,
    // ahead of launchd and of anything that can sleep, and so (the board thread's first step, seconds before the Data
    // mount) ahead of a staged address, which it never replaces. Left alone when the fuses cannot be read safely or
    // are blank.
    void publishAddress()
    {
        if (__atomic_load_n(&gStagedState, __ATOMIC_ACQUIRE) == kStagedFound) return;
        volatile uint8_t *car = reinterpret_cast<volatile uint8_t *>(ml_io_map(kCarPA, 0x1000));
        if (!car || !(r32(car, CLK_OUT_ENB_H) & kFuseClock) || !(r32(car, MISC_CLK_ENB) & kFuseVisible)) return;
        volatile uint8_t *fuse = reinterpret_cast<volatile uint8_t *>(ml_io_map(kFusePA, 0x1000));
        const uint32_t lot0 = fuse ? r32(fuse, kFuseLotCode0) : 0;
        if (!lot0) return;
        const uint8_t unit[6] = {0x98, 0xb6, 0xe9, (uint8_t)(lot0 >> 16), (uint8_t)(lot0 >> 8), (uint8_t)(lot0 + 1)};
        publishAddressBytes(unit);
        gAddressFromFuses = true;
    }
}

// ---- Setup -------------------------------------------------------------------------------------------------------

bool SwitchBluetooth::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    uart = reinterpret_cast<volatile uint8_t *>(ml_io_map(kUartPA, 0x1000));
    car = reinterpret_cast<volatile uint8_t *>(ml_io_map(kCarPA, 0x1000));
    gpio = reinterpret_cast<volatile uint8_t *>(ml_io_map(kGpioPA, 0x1000));
    pinmux = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPinmuxPA, 0x1000));
    hwLock = IOSimpleLockAlloc();
    cmdLock = IOSimpleLockAlloc();
    stateLock = IOLockAlloc();
    hciLock = IOLockAlloc();
    txLock = IOLockAlloc();
    rx.data = static_cast<uint8_t *>(IOMalloc(kRingBytes));
    tx.data = static_cast<uint8_t *>(IOMalloc(kRingBytes));
    loop = IOWorkLoop::workLoop();
    stuckCall = thread_call_allocate(&SwitchBluetooth::stuck, this);
    nanoseconds_to_absolutetime(kIdleStormNs, &idleStormAbs);
    nanoseconds_to_absolutetime((uint64_t)kStatsMs * 1000000ULL, &statsWindow);
    if (!uart || !car || !gpio || !pinmux || !hwLock || !stateLock || !hciLock || !txLock || !rx.data || !tx.data ||
        !loop || !stuckCall || !cmdLock) {
        IOLog("SwitchBluetooth: out of memory or mappings\n");
        return false;
    }
    uart += kUartD;

    // The packager's /arm-io/bluetooth node (it is also the IOService bluetoothd finds by name). The patch must be
    // whole Write_RAM records ending in Launch_RAM before the chip ever sees it.
    if (IORegistryEntry *node = IORegistryEntry::fromPath("/arm-io/bluetooth", gIODTPlane)) {
        OSData *data = OSDynamicCast(OSData, node->getProperty("ioswitch,patchram"));
        if (data && validPatch(static_cast<const uint8_t *>(data->getBytesNoCopy()), data->getLength())) {
            patch = data;
            patch->retain();
        } else if (data) {
            IOLog("SwitchBluetooth: ioswitch,patchram is not a .hcd record list (%u bytes); ignored\n",
                  data->getLength());
        }
        data = OSDynamicCast(OSData, node->getProperty("local-mac-address"));  // the unit's, see publishAddress()
        if (data && data->getLength() == 6) {
            memcpy(address, data->getBytesNoCopy(), 6);
            haveAddress = true;
            if (__atomic_load_n(&gStagedState, __ATOMIC_ACQUIRE) == kStagedFound && !memcmp(address, gStaged, 6))
                addressFrom = "staged";
            else if (gAddressFromFuses)
                addressFrom = "fuses";
        }
        node->release();
    }

    setupPins();
    setupUart();

    // UART-D's level interrupt (ioswitch_interrupt_nub sleeps until ArmCpu has the GIC up).
    IOService *nub = ioswitch_interrupt_nub(kIntid, false);
    irq = nub ? IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchBluetooth::action,
                                                                         &SwitchBluetooth::filter, nub, 0)
              : nullptr;
    if (!irq || loop->addEventSource(irq) != kIOReturnSuccess) {
        char gic[160];
        ioswitch_interrupt_describe(kIntid, gic, sizeof(gic));
        panic("SwitchBluetooth: cannot attach the UART-D interrupt (INTID %u): %s", kIntid, gic);
    }
    // RX interrupts stay off: setupPins left the core unpowered (see power()); the first flush after power-on enables them.
    irq->enable();

    tio.c_iflag = IGNPAR;
    tio.c_cflag = CS8 | CREAD | CLOCAL | CRTSCTS;
    tio.c_ispeed = tio.c_ospeed = kFastBaud;
    registerService();
    return true;
}

// UART pads to the UART (GPIO CNF 0 = special function), BT_WAKE asserted (low) and BT_REG_ON low (core off),
// host wake an input. Masked GPIO writes only; nxk Pads::setup and power() order.
void SwitchBluetooth::setupPins()
{
    for (const Pad &pad : kPads) {
        volatile uint32_t *reg = reinterpret_cast<volatile uint32_t *>(pinmux + pad.offset);
        *reg = (*reg & ~PM_LOW) | pad.value;
    }
    for (unsigned p = PI4; p < PI4 + 4; ++p) setPin(p, GPIO_CNF, false);
    setPin(PH3, GPIO_OUT, false);
    setPin(PH3, GPIO_OE, true);
    setPin(PH3, GPIO_CNF, true);
    setPin(PH4, GPIO_OUT, false);
    setPin(PH4, GPIO_OE, true);
    setPin(PH4, GPIO_CNF, true);
    setPin(PH5, GPIO_OE, false);
    setPin(PH5, GPIO_CNF, true);
    (void)r32(gpio, portReg(PH4, GPIO_OUT));
}

// Reset and clock UART-D, then hekate uart_init's order with Linux tegra_uart_hw_init's FIFO setup: 8N1, FIFOs on
// (RX trigger 8, TX trigger 16), 115200, hardware flow control. Interrupts stay off until the event source is up.
void SwitchBluetooth::setupUart()
{
    w32(car, RST_DEV_U_SET, kUartDBit);
    w32(car, CLK_ENB_U_SET, kUartDBit);
    w32(car, CLK_SOURCE_UARTD, 0);
    IODelay(10);
    w32(car, RST_DEV_U_CLR, kUartDBit);
    (void)r32(car, RST_DEVICES_U);

    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
    w32(uart, IER, 0);
    w32(uart, LCR, LCR_8N1);
    w32(uart, IRDA_CSR, 0);  // no line inversion left behind
    w32(uart, FCR, FCR_ENABLE);
    (void)r32(uart, SPR);
    IODelay(20);
    setBaudLocked(kInitBaud);
    w32(uart, FCR, FCR_ENABLE | FCR_CLEAR_RX | FCR_CLEAR_TX | FCR_RX_TRIG_8 | FCR_TX_TRIG_16);
    w32(uart, MCR, MCR_CTS_EN | MCR_RTS_EN | MCR_RTS);
    (void)r32(uart, SPR);
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
    IODelay(100);
}

// hekate clock_uart_use_src_div + uart_init. 3 M and 1 M come from PLLP through the fractional divider (8.5 ->
// 48 MHz, 25.5 -> 16 MHz, 16x oversampling, divisor latch 1); other rates from undivided PLLP and the divisor latch.
// IER and the data registers are banked behind DLAB, so the filter must not run meanwhile (hwLock held).
void SwitchBluetooth::setBaudLocked(uint32_t rate)
{
    uint32_t divisor = 1;
    if (rate == 3000000)
        w32(car, CLK_SOURCE_UARTD, kUartDivEnable | 15);
    else if (rate == 1000000)
        w32(car, CLK_SOURCE_UARTD, kUartDivEnable | 49);
    else {
        w32(car, CLK_SOURCE_UARTD, 2);
        divisor = (8 * rate + kPllpHz) / (16 * rate);
    }
    w32(uart, LCR, LCR_DLAB | LCR_8N1);
    w32(uart, THR, divisor & 0xff);
    w32(uart, IER, (divisor >> 8) & 0xff);
    w32(uart, LCR, LCR_8N1);
    (void)r32(uart, SPR);
    baud = rate;
}

// As serial-tegra's tegra_uart_set_termios: latched interrupts cleared and IER 0 while the clock and divisor change and
// for the two characters at the new rate that tegra_set_baudrate waits, then IER back. r7h's storms (kIdleStorm idle
// interrupts once per attempt, around the 3 Mbaud switch) most likely began at a rate change under live RX interrupts.
void SwitchBluetooth::setHostBaud(uint32_t rate)
{
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
    clearInterruptsLocked();
    w32(uart, IER, 0);
    setBaudLocked(rate);
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
    IODelay(2 * 10 * 1000000 / rate + 20);
    s = IOSimpleLockLockDisableInterrupt(hwLock);
    w32(uart, IER, ier);
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
}

// ---- Interrupt path ----------------------------------------------------------------------------------------------

// Primary interrupt context on CPU0 (every SPI targets it). Services the UART until IIR reports nothing pending, so
// the level line drops; returns true when the work loop has something to deliver or a writer to wake. RX is bounded
// to kIrqRxBudget bytes per interrupt: once that is spent (or the ring fills) drainRx masks the RX interrupts and
// rxStalled is set, so the level line drops and this cannot be re-entered until deliver() drains the ring and
// re-enables RX. A real producer/consumer hand-off, no re-fire storm, no polling: CPU0 is never pinned here, so the
// GPU fence interrupt it shares CPU0 with is not starved. Nor by a condition that no read clears: that masks RX (and,
// should something else be left, every source) before the filter returns, so the line drops either way.
//
// Received bytes wake the work loop once per controller packet in client mode, not at every RX FIFO trigger (8 bytes,
// ~27 us at 3 Mbaud: about 125 wakeups for one 1021-byte ACL packet of an A2DP stream): the filter frames the H4
// stream it puts in the ring (frameByteLocked) and wakes the work loop when a packet has ended there, when the line
// went idle (RX timeout: the controller paused, a packet's last bytes below the FIFO trigger come this way anyway), and
// for whatever the work loop must see at once (RX stalled, a fault, a line error, framing it could not follow). Bytes
// in the middle of a packet stay in the ring until its end, which is the earliest the client could use them: no packet
// reaches bluetoothd later than before. frameRx's framing is the reference, and deliver() puts the filter's back in step
// with it whenever the ring is empty (syncFramerLocked). The kernel's own HCI (bring-up) keeps a wakeup per interrupt.
bool SwitchBluetooth::filter(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<SwitchBluetooth *>(owner);
    bool work = false, rxWork = false, idle = false, done = false;
    IOSimpleLockLock(self->hwLock);
    ++self->irqs;
    const uint32_t rxBefore = self->rx.head, txBefore = self->tx.tail, resetsBefore = self->rxResets;
    const uint32_t faultsBefore = self->rxFaults, errorsBefore = self->lineErrors;
    uint32_t budget = kIrqRxBudget;
    for (unsigned round = 0; round < kFilterRounds && !done; ++round) {
        const uint32_t iir = r32(self->uart, IIR);
        if (iir & IIR_NO_INT) {
            done = true;
            break;
        }
        switch ((iir >> 1) & 7) {
        case ID_TX: work |= self->fillTx(); break;
        case ID_RX_TIMEOUT: idle = true; [[fallthrough]];
        case ID_RX:
        case ID_LINE:
        case ID_EORD: rxWork |= self->drainRx(budget, true); break;
        case ID_MODEM: (void)r32(self->uart, MSR); break;
        default: break;  // 5 and 7: T210's break indications (serial-tegra), receive side like the line status
        }
        // RX masked (budget spent, ring full, or a fault): the work loop drains and, but for a fault, re-enables it.
        if (self->rxStalled || self->rxFault) done = true;
    }
    // The middle of a packet: its end wakes the work loop (above).
    if (rxWork && self->framePackets && !self->rxFrame.lost && !self->rxPacketDone && !idle && !self->rxStalled &&
        !self->rxFault && self->lineErrors == errorsBefore)
        rxWork = false;
    if (rxWork) self->rxPacketDone = false;
    work |= rxWork;
    if (!done) {
        // kFilterRounds conditions and IIR still reports one. All but THRE and modem status (both cleared above) are
        // receive conditions, so RX goes off; anything left after that takes every source with it.
        self->maskRxFaultLocked("an IIR condition no read clears");
        if (!(r32(self->uart, IIR) & IIR_NO_INT)) {
            self->ier = 0;
            w32(self->uart, IER, 0);
        }
    }
    // Counted for describe(): a steady rate of these is an interrupt source the loop above does not clear. kIdleStorm
    // of them in a row within kIdleStormNs is the line re-firing with nothing to clear: RX goes off, then every source.
    if (self->rx.head == rxBefore && self->tx.tail == txBefore && self->rxResets == resetsBefore &&
        self->rxFaults == faultsBefore) {
        ++self->idleIrqs;
        const uint64_t now = mach_absolute_time();
        if (!self->idleRun++) self->idleSince = now;
        if (self->idleRun >= kIdleStorm) {
            if (now - self->idleSince < self->idleStormAbs) {
                if (!self->stormCleared) {  // serial-tegra's way to drop latched interrupts first
                    self->clearInterruptsLocked();
                    self->stormCleared = true;
                } else if (!self->rxFault) {
                    self->maskRxFaultLocked("an interrupt storm with IIR clear");
                } else {
                    self->ier = 0;
                    w32(self->uart, IER, 0);
                }
            }
            self->idleRun = 0;
        }
    } else {
        self->idleRun = 0;
        self->stormCleared = false;
    }
    work |= self->rxFaults != faultsBefore;  // the work loop runs once more; RX comes back at a flush or a send
    IOSimpleLockUnlock(self->hwLock);
    if (work) self->actionDue = true;
    return work;
}

// hwLock held. Reading LSR clears its error bits, so they are counted here; a byte with an error is still kept
// (H4 resynchronises on the next packet type). The FIFO is drained until it empties, the ring fills, or this
// interrupt's byte budget is spent. In the latter two cases the RX interrupts are masked and rxStalled is set: the
// rest stays in the FIFO, where RTS holds the chip, and deliver() drains the ring and re-enables RX. Bounding the
// batch keeps one interrupt short so CPU0 is not pinned by a sender that outruns the slow UART MMIO. With
// stallWhenSpent false (resumeRx) a spent budget just stops the drain and RX stays on: only a full ring stalls.
// A break character, or a stuck BI/FIFOE that the FIFO reset does not cure, is a fault event (rxFaultEventLocked).
bool SwitchBluetooth::drainRx(uint32_t &budget, bool stallWhenSpent)
{
    uint32_t head = rx.head;
    const uint32_t tail = __atomic_load_n(&rx.tail, __ATOMIC_ACQUIRE);
    bool any = false;
    uint32_t lsr;
    while ((lsr = r32(uart, LSR)) & LSR_DR) {
        if (lsr & LSR_ERRORS) ++lineErrors;
        if (head - tail == kRingBytes || budget == 0) {
            if (head - tail == kRingBytes || stallWhenSpent) {
                ier &= ~IER_RX;
                w32(uart, IER, ier);
                rxStalled = true;
            }
            break;
        }
        const uint8_t byte = (uint8_t)r32(uart, THR);
        rx.data[head++ & (kRingBytes - 1)] = byte;
        if (lsr & LSR_ERRORS) rxFrame.lost = true;  // the byte may not be the one sent: framing unknown till the sync
        frameByteLocked(byte);
        --budget;
        any = true;
        if (lsr & LSR_BI) {  // the line was low for a whole character: nothing drives it
            if (rxFaultEventLocked("the line held low (break)")) break;
        } else if (!(lsr & LSR_ERRORS)) {
            rxFaultRun = 0;  // a good byte: what came before was a glitch
        }
    }
    // T210 (serial-tegra tegra_uart_decode_rx_error): a break, or an errored character, can leave BI or FIFOE set with
    // the FIFO empty, and the line-status interrupt then stays asserted until the RX FIFO is reset. Reset once; the
    // same again with no good byte in between is the line itself (held low), and masks RX instead.
    if (!rxFault && !(lsr & LSR_DR) && (lsr & (LSR_BI | LSR_FIFOE)) && !rxFaultEventLocked("a stuck BI/FIFOE")) {
        resetRxFifoLocked();
        ++rxResets;
    }
    rxBytes += head - rx.head;
    __atomic_store_n(&rx.head, head, __ATOMIC_RELEASE);
    return any || rxStalled || rxFault;
}

// hwLock held. One more byte of the controller's H4 stream in the ring (filter): rxPacketDone when it ends a packet.
// Anything that is not an H4 packet type loses the framing until the next sync.
void SwitchBluetooth::frameByteLocked(uint8_t byte)
{
    auto &f = rxFrame;
    if (f.lost) return;
    if (f.state == 0) {
        f.type = byte;
        f.need = byte == H4_EVENT ? 2 : byte == H4_ACL ? 4 : byte == H4_SCO ? 3 : 0;
        f.have = 0;
        f.state = 1;
        f.lost = !f.need;
    } else if (f.state == 1) {
        f.header[f.have++] = byte;
        if (f.have < f.need) return;
        f.left = f.type == H4_EVENT ? f.header[1] : f.type == H4_ACL ? le16(f.header + 2) : f.header[2];
        f.state = f.left ? 2 : 0;
        rxPacketDone |= !f.left;
    } else if (!--f.left) {
        f.state = 0;
        rxPacketDone = true;
    }
}

// hwLock held, work loop, client mode, the ring empty: the filter's framing set to frameRx's, the reference. Between
// packets, or in the payload of one with rxPacketLeft bytes still to come; lost while frameRx's is.
void SwitchBluetooth::syncFramerLocked()
{
    rxFrame = {};
    rxPacketDone = false;
    rxFrame.lost = framingLost;
    if (rxPacketLeft) {
        rxFrame.state = 2;
        rxFrame.left = rxPacketLeft;
    }
}

// hwLock held. One break or stuck-error event; true when it masked RX: kFaultRun of them with no good byte between.
bool SwitchBluetooth::rxFaultEventLocked(const char *what)
{
    if (++rxFaultRun < kFaultRun) return false;
    maskRxFaultLocked(what);
    return true;
}

// hwLock held. The receiver reports a condition no read or FIFO reset clears: the line held low (a core that is not
// driving its TX, e.g. not yet up or gone), an IIR condition the filter cannot clear, or a line that re-fires with
// none. Left enabled, its level line re-fires the moment the filter returns and pins CPU0, and with it whatever thread
// ran there: a flush or the work loop thread, either holding the work-loop gate, so every later runAction (bring-up,
// connect) waits for good. RX stays off until the next flush (every bring-up step, every connect) or send (kickTx);
// ctlConnect brings the chip up again if it faulted since. checkDelivery sees RX disabled: no lost interrupt.
void SwitchBluetooth::maskRxFaultLocked(const char *what)
{
    ier &= ~IER_RX;
    w32(uart, IER, ier);
    if (!rxFault) ++rxFaults;
    rxFault = true;
    rxFaultRun = 0;
    lastFault = what;
}

// hwLock held. serial-tegra's tegra_uart_set_termios before it changes the configuration: "Clear all interrupts" by
// writing IER with RDI set, then 0, reading each back; IER as it was afterwards.
void SwitchBluetooth::clearInterruptsLocked()
{
    w32(uart, IER, ier | IER_RDI);
    (void)r32(uart, IER);
    w32(uart, IER, 0);
    (void)r32(uart, IER);
    w32(uart, IER, ier);
}

// hwLock held. Tops the TX FIFO up from the ring; THRI stays enabled while the ring has bytes.
bool SwitchBluetooth::fillTx()
{
    uint32_t tail = tx.tail;
    const uint32_t head = __atomic_load_n(&tx.head, __ATOMIC_ACQUIRE);
    while (tail != head && !(r32(uart, LSR) & LSR_TXFIFO_FULL)) w32(uart, THR, tx.data[tail++ & (kRingBytes - 1)]);
    txBytes += tail - tx.tail;
    __atomic_store_n(&tx.tail, tail, __ATOMIC_RELEASE);
    if (tail == head && (ier & IER_THRI)) {
        ier &= ~IER_THRI;
        w32(uart, IER, ier);
    }
    return txWanted;
}

// Thread context: starts transmission of what was just queued and arms THRI for the rest. RX masked for a fault comes
// back here too: what is sent now gets an answer, and reading it is what clears a receiver left waiting on data. The
// fault, if it is still there, masks RX again within an interrupt or two (one more fault per send at most).
void SwitchBluetooth::kickTx()
{
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
    (void)fillTx();
    uint32_t want = ier;
    if (tx.used()) want |= IER_THRI;
    if (rxFault && chipOn && !rxStalled) {
        want |= IER_RX;
        rxFault = false;
        rxFaultRun = 0;
        stormCleared = false;
    }
    if (want != ier) {
        ier = want;
        w32(uart, IER, ier);
    }
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
}

void SwitchBluetooth::action(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchBluetooth *>(owner);
    self->actionDue = false;  // IOKit unmasks the line when this returns
    ++self->st.actions;
    self->deliver();
}

// Work loop. Received bytes go to the kernel's HCI parser during a bring-up, to the client once connected
// (as much as its socket takes; the rest when it reads, ctlRcvd), and are dropped otherwise.
void SwitchBluetooth::deliver()
{
    if (txWanted) {
        IOLockLock(txLock);
        IOLockWakeup(txLock, &tx, false);
        IOLockUnlock(txLock);
    }
again:
    // Completions made here (answerHere) go out between the controller's packets.
    if (mode == kClient && !framingLost && !rxPacketLeft) (void)passAnswers();
    for (uint32_t avail; (avail = rx.used());) {
        const uint32_t tail = rx.tail, at = tail & (kRingBytes - 1);
        uint32_t chunk = avail < kRingBytes - at ? avail : kRingBytes - at;
        if (mode == kKernel) {
            parse(rx.data + at, chunk);
        } else if (mode == kClient) {
            // One controller packet at a time (frameRx); a completion waits here until it has all arrived.
            if (!framingLost && !rxPacketLeft && (!passAnswers() || !frameRx(avail))) break;
            if (!framingLost && chunk > rxPacketLeft) chunk = rxPacketLeft;
            if (!framingLost && rxDrop) {  // a completion the client did not ask for (frameRx): taken, not passed on
                rxPacketLeft -= chunk;
                rxDrop = rxPacketLeft != 0;
                __atomic_store_n(&rx.tail, tail + chunk, __ATOMIC_RELEASE);
                continue;
            }
            size_t room = 0;
            if (ctl_getenqueuespace(control, client, &room) != 0) room = chunk;  // client gone: drop below
            if (room < chunk) chunk = (uint32_t)room;
            if (!chunk) {
                if (clientRoom(1)) continue;
                break;
            }
            if (ctl_enqueuedata(control, client, rx.data + at, chunk, 0) != 0) dropped += chunk;
            if (!framingLost) rxPacketLeft -= chunk;
        } else {
            dropped += chunk;
        }
        __atomic_store_n(&rx.tail, tail + chunk, __ATOMIC_RELEASE);
    }
    if (rxStalled && rx.space() >= kRingBytes / 2 && resumeRx()) goto again;
    if (mode == kClient) {
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
        if (!rx.used()) syncFramerLocked();  // nothing in the ring: frameRx's framing is the filter's (filter)
        IOSimpleLockUnlockEnableInterrupt(hwLock, s);
        logStats(false);
    }
}

// Work loop: whether the client's socket takes `need` bytes now. If not, ctlRcvd is asked to run deliver() again once it
// reads; a read between the space query and that flag would not have seen the flag, so the space is asked once more
// after raising it, or the client and the ring would wait on each other with RX masked.
bool SwitchBluetooth::clientRoom(size_t need)
{
    size_t room = 0;
    if (ctl_getenqueuespace(control, client, &room) != 0 || room >= need) return true;  // gone: the enqueue drops
    __atomic_store_n(&clientBlocked, true, __ATOMIC_SEQ_CST);
    if (ctl_getenqueuespace(control, client, &room) == 0 && room >= need) {
        __atomic_store_n(&clientBlocked, false, __ATOMIC_SEQ_CST);
        return true;
    }
    return false;
}

// ---- Client-mode completions ---------------------------------------------------------------------------------------
//
// bluetoothd takes this BCM4356A3 for the BCM4355C0 of the J120 table and adds a peer's IRK (MagicPairing's key update
// from AirPods) with Broadcom's RPA vendor command, _BCM_LEMetaAddIrk: opcode 0xFCE9, sub-command 2, IRK, address type,
// address (24 bytes; Android's HCI_VENDOR_BLE_RPA_VSC BTM_BLE_META_ADD_IRK_ENTRY). This firmware evidently answers it
// with the Command Complete of the standard command it maps it to, LE Add Device To Resolving List (0x2027; Command
// Disallowed while LE scanning runs): with the controller's single command credit the vendor command was the only one
// outstanding when that completion came. bluetoothd has no 0x2027 outstanding, and an unknown completion of a
// non-vendor command makes it restart ("Command Complete, orphan event, opcode: 2027", then "Bluetooth error -
// restarting { reason=635 }"; r6k 04:49:39, r7j 17:27:50), every time AirPods connect. A completion of a vendor command
// it did not expect it lets pass. So here a Command Complete or Status for an LE command the client has not
// outstanding, while a vendor command is, is passed on as that vendor command's, status Unknown HCI Command: the answer
// bluetoothd already gets, and copes with, for every other vendor command this controller lacks (its Stack.VSC "status
// 701" lines). The credits in the event stay the controller's.
//
// The firmware also answers 0xFCE9 properly (Command Complete, status 0) and then, tens of ms later, sends that LE
// completion for 0x2027 anyway, with nothing of the client's outstanding (r7m 18:52:29 and :39: bluetoothd's
// startup loads a paired device's IRK, so it restarted on every start). Any Command Complete or Status for an opcode
// the client has not outstanding (and that the rule above does not take) is not passed on at all: bluetoothd sets its
// command credits from every completion it gets and counts them down as it sends, so one whose credit belongs to no
// command of its own (even as the no-op form, opcode 0) would let it send a second command past the controller's
// single slot. One line per opcode per boot. If `sentCmds` ever pushed a command off unanswered, completions it
// cannot place pass as they come, as before, until the client reconnects.
//
// BLE. bluetoothd drives this controller's LE side with the Broadcom vendor commands of J120's BCM4355C0, which this
// firmware (CYW4356A3) implements in part; r7p5 (23:08:34-:55, AirDrop's 21 s advert) and the r7p6 live log show three
// mismatches, and each command bluetoothd sends whole (clientCommand) is looked at for them before it goes out:
// - Advertising. bluetoothd's multi-advertising instances (_BCM_*MultipleAdvInstance*: 0xFD48 create, 0xFD4A set
//   parameters {instance, interval min, interval max, type, channel map, filter policy}, 0xFD4B data, 0xFD4D enable,
//   0xFD4E address) work on this firmware: the same AirDrop advert started at once (status 0) when sharingd slowed it
//   to 181.25 ms. Asked for 30 ms (interval 48, ADV_NONCONN_IND), 0xFD4A got Invalid HCI Command Parameters ("STATUS
//   718"): Core 4.x sets a 100 ms floor (0x00A0) for scannable and non-connectable advertising, which 5.0 removed
//   and bluetoothd's chips are newer than. Each refusal made bluetoothd force-stop its 4 instances twice (0xFD4D
//   enable 0 with no callback, Command Disallowed: "status 712, hlCb" the no-op default) and start again at once,
//   ~140 times a second: 23,608 of those and 2,951 refusals in 21 s, the SwitchBluetooth thread at the top of kprof.
//   So on a controller older than 5.0 (HCI version from Read Local Version), the interval of 0xFD4A and of LE Set
//   Advertising Parameters (0x2006) for those two types is raised to the floor (fixAdvertising): the fastest this
//   controller can advertise them at all, one line the first time and a count in the stats.
// - Scanning. bluetoothd filters adverts in the controller with two Broadcom LE meta (0xFCE9) features together: the
//   packet content filter (sub-operations 5 feature selection and 7 service UUID, status 0 here) holding the
//   service UUIDs some sessions scan for, and Apple's matching rules (0x32/0x33/0x1E/0x35: Unknown HCI Command here,
//   "Failed to add rule") passing the Apple-manufacturer adverts the others want, both switched on by 0xFCE9
//   sub-operation 1 (enable features, 0xA043 seen). With the rules missing, only adverts carrying the one UUID
//   asked for passed: 23 scan windows on r7p5 and every window since on r7p6 ended "0 Advertising Events" with
//   filter policy 0, a 30/300 ms window and passive scanning. A controller without those features answers Unknown
//   HCI Command, as this one already does for the matching rules, and bluetoothd then gets every advert and filters
//   in the host; so 0xFCE9 sub-operations 1, 5 and 7 (and the UUID filter's later forms 0x50, 0x61) are answered here
//   with that status and never reach the controller, which keeps its default of reporting every advert
//   (answerHere: a Command Complete with the controller's last credit count, passed on between its packets).
// - The stats line (logStats, os_log only: at most every kStatsMs and only after traffic, and when the client goes)
//   counts the controller's packets and events by type, LE meta events by sub-event with the advertising reports
//   they carry, the client's commands, those answered here, intervals raised, scan enables, and the completions
//   with a non-zero status by opcode; the first of each LE and vendor command (and the event mask commands) is
//   logged once per boot with its parameters and its completion (traceCommand, traceCompletion).

void SwitchBluetooth::resetClientFraming()
{
    rxPacketLeft = 0;
    framingLost = false;
    rxDrop = false;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    txFrame = {};
    sentCount = 0;
    sentOverflow = false;
    answerHead = answerTail = 0;
    links = {};  // a new client: the controller has no links (a used one was brought up again)
    requests = {};
    role = SWITCH_BT_ROLE_UNKNOWN;
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
}

// Client thread, before the bytes go out: the commands in the client's H4 stream join `sentCmds`. ACL and SCO
// payloads are skipped by their lengths. Anything that is not H4 turns the bookkeeping off for the connection. Returns
// how many of `bytes` were taken: up to the end of the first packet that ends in them, so that ctlSend can look at a
// whole command that starts next (clientCommand), else all of them.
size_t SwitchBluetooth::noteClientTx(const uint8_t *bytes, size_t length)
{
    if (framingLost) return length;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    auto &f = txFrame;
    size_t i = 0;
    while (i < length && !framingLost) {
        if (f.state == 0) {
            f.type = bytes[i++];
            f.need = f.type == H4_COMMAND ? 3 : f.type == H4_ACL ? 4 : f.type == H4_SCO ? 3 : 0;
            f.have = 0;
            if (f.need)
                f.state = 1;
            else
                framingLost = true;
        } else if (f.state == 1) {
            f.header[f.have++] = bytes[i++];
            if (f.have < f.need) continue;
            f.left = f.type == H4_ACL ? (uint32_t)(f.header[2] | f.header[3] << 8) : f.header[2];
            if (f.type == H4_COMMAND) noteCommandLocked((uint16_t)(f.header[0] | f.header[1] << 8));
            if (f.type == H4_ACL) switch_bt_link_tx(&links, switch_bt_acl_handle(f.header));
            f.state = f.left ? 2 : 0;
            if (!f.state) break;
        } else {
            const size_t skip = length - i < f.left ? length - i : f.left;
            i += skip;
            f.left -= (uint32_t)skip;
            if (!f.left) {
                f.state = 0;
                break;
            }
        }
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    return framingLost ? length : i;
}

// cmdLock held. A command on its way to the controller joins `sentCmds`.
void SwitchBluetooth::noteCommandLocked(uint16_t opcode)
{
    if (opcode == OP_HOST_NUMBER_OF_COMPLETED_PACKETS) return;
    if (sentCount == kMaxPending) {  // a command that was never answered: the oldest goes
        memmove(sentCmds, sentCmds + 1, (kMaxPending - 1) * sizeof(sentCmds[0]));
        --sentCount;
        sentOverflow = true;
    }
    sentCmds[sentCount++] = opcode;
}

// cmdLock held. The oldest sent `opcode`, taken off the list; false if there is none.
bool SwitchBluetooth::takePendingLocked(uint16_t opcode)
{
    for (uint32_t i = 0; i < sentCount; ++i) {
        if (sentCmds[i] != opcode) continue;
        memmove(sentCmds + i, sentCmds + i + 1, (sentCount - i - 1) * sizeof(sentCmds[0]));
        --sentCount;
        return true;
    }
    return false;
}

// cmdLock held. The oldest pending vendor command, taken off the list; 0 if there is none.
uint16_t SwitchBluetooth::takeVendorLocked()
{
    for (uint32_t i = 0; i < sentCount; ++i) {
        if ((sentCmds[i] >> 10) != kOgfVendor) continue;
        const uint16_t opcode = sentCmds[i];
        (void)takePendingLocked(opcode);
        return opcode;
    }
    return 0;
}

// Work loop, client mode, at a packet boundary of the controller's stream: the next packet's length into
// rxPacketLeft. A Command Complete or Status waits until it is all in the ring and is matched against `sentCmds` first
// (top of section). False while what is needed has not arrived; the next interrupt's action comes back here.
bool SwitchBluetooth::frameRx(uint32_t avail)
{
    const uint32_t tail = rx.tail;
    auto at = [this, tail](uint32_t i) -> uint8_t & { return rx.data[(tail + i) & (kRingBytes - 1)]; };
    uint32_t total = 0;
    switch (at(0)) {
    case H4_EVENT: {
        if (avail < 3) return false;
        total = 3u + at(2);
        const uint8_t code = at(1);
        const bool status = code == EV_COMMAND_STATUS;  // Status: status, credits, opcode; Complete: credits, opcode
        const bool completion = status ? at(2) >= 4 : code == EV_COMMAND_COMPLETE && at(2) >= 3;
        // A completion is looked at whole; an LE meta event once its sub-event and report count (bytes 3, 4) are in,
        // and whole if it is a connection complete; a link event ("Links") whole.
        if (code == EV_LE_META && avail < (total < 5 ? total : 5)) return false;
        const bool linked = !completion && switch_bt_event_wanted(code, code == EV_LE_META && at(2) ? at(3) : 0);
        if ((completion || linked) && avail < total) return false;
        if (linked) linkEvent(code, at(2));
        ++st.events;
        switch (code) {
        case EV_COMMAND_COMPLETE: ++st.cc; break;
        case EV_COMMAND_STATUS: ++st.cs; break;
        case EV_NUM_COMPLETED: ++st.nocp; break;
        case EV_VENDOR: ++st.vendor; break;
        case EV_LE_META: {
            ++st.le;
            const uint8_t sub = at(2) >= 1 ? at(3) : 0;
            if (sub == LE_ADV_REPORT) {
                ++st.leAdv;
                st.leAdvReports += at(2) >= 2 ? at(4) : 0;
            } else if (sub == LE_CONN_COMPLETE || sub == LE_ENH_CONN_COMPLETE) {
                ++st.leConn;
            } else {
                ++st.leOther;
            }
            break;
        }
        default: ++st.otherEvents; break;
        }
        if (!completion) break;
        const uint8_t credits = status ? at(4) : at(3);
        if (credits) lastCredits = credits;
        const uint32_t op = status ? 5 : 4;
        const uint16_t opcode = (uint16_t)(at(op) | at(op + 1) << 8);
        if (!opcode) break;  // credits only
        uint16_t vendor = 0;
        bool stray = false;
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
        if (!takePendingLocked(opcode)) {
            if ((opcode >> 10) == kOgfLe) vendor = takeVendorLocked();
            stray = !vendor && !sentOverflow;
        }
        IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
        uint8_t *const statusByte = status ? &at(3) : at(2) >= 4 ? &at(6) : nullptr;
        const unsigned was = statusByte ? *statusByte : 0;
        if (!stray) {
            uint8_t ret[kTraceReturn];
            const uint32_t retLength =
                !status && at(2) > 4 ? (at(2) - 4u < kTraceReturn ? at(2) - 4u : kTraceReturn) : 0;
            for (uint32_t i = 0; i < retLength; ++i) ret[i] = at(7 + i);
            countCompletion(opcode, (uint8_t)was);
            traceCompletion(opcode, status, (uint8_t)was, ret, retLength);
            linkCompletion(opcode, status, (uint8_t)was);
        }
        if (stray) {  // not passed on (top of section)
            rxDrop = true;
            ++strayDrops;
            bool logged = false;
            for (uint32_t i = 0; i < strayOpCount && !logged; ++i) logged = strayOps[i] == opcode;
            if (!logged && strayOpCount < sizeof(strayOps) / sizeof(strayOps[0])) {
                strayOps[strayOpCount++] = opcode;
                IOLog("SwitchBluetooth: the controller sent a Command %s for %#06x (status 0x%02x) with no such "
                      "command of the client's outstanding; not passed on\n",
                      status ? "Status" : "Complete", opcode, was);
            }
            break;
        }
        if (!vendor) break;
        at(op) = (uint8_t)vendor;
        at(op + 1) = (uint8_t)(vendor >> 8);
        if (statusByte) *statusByte = kUnknownCommand;
        if (!completionFixes++) {
            IOLog("SwitchBluetooth: the controller answered vendor command %#06x with a Command %s for %#06x (status "
                  "%#04x); passed on as Unknown HCI Command for %#06x\n",
                  vendor, status ? "Status" : "Complete", opcode, was, vendor);
        }
        break;
    }
    case H4_ACL: {
        if (avail < 5) return false;
        total = 5u + (uint32_t)(at(3) | at(4) << 8);
        ++st.acl;
        const uint8_t header[2] = {at(1), at(2)};
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
        switch_bt_link_rx(&links, switch_bt_acl_handle(header));
        IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
        break;
    }
    case H4_SCO:
        if (avail < 4) return false;
        total = 4u + at(3);
        ++st.sco;
        break;
    default: framingLost = true; return true;  // not H4: pass everything on as it comes
    }
    rxPacketLeft = total;
    return true;
}

// Client thread (ctlSend): one whole command of the client's, `size` bytes from its H4 type byte, in a buffer of ours.
// Answered here (answerHere), or sent after fixAdvertising; traced and counted either way (BLE section).
errno_t SwitchBluetooth::clientCommand(uint8_t *packet, size_t size)
{
    const uint16_t opcode = le16(packet + 1);
    uint8_t *const params = packet + 4;
    const uint8_t length = packet[3];
    __atomic_fetch_add(&st.commands, 1, __ATOMIC_RELAXED);
    if ((opcode >> 10) == kOgfLe) __atomic_fetch_add(&st.leCommands, 1, __ATOMIC_RELAXED);
    if ((opcode >> 10) == kOgfVendor) __atomic_fetch_add(&st.vendorCommands, 1, __ATOMIC_RELAXED);
    if (opcode == OP_LE_SET_SCAN_ENABLE && length)
        __atomic_fetch_add(params[0] ? &st.scanOn : &st.scanOff, 1, __ATOMIC_RELAXED);
    if (answerHere(opcode, params, length)) {
        traceCommand(opcode, params, length, true);
        loop->runAction(&SwitchBluetooth::gated, this, reinterpret_cast<void *>(1));  // deliver(): the answer goes out
        return 0;
    }
    if (fixAdvertising(opcode, params, length)) __atomic_fetch_add(&st.advFixed, 1, __ATOMIC_RELAXED);
    if (opcode == OP_ACCEPT_CONNECTION && acceptAsCentral(params, length))
        __atomic_fetch_add(&st.central, 1, __ATOMIC_RELAXED);
    traceCommand(opcode, params, length, false);
    linkCommand(opcode, params, length);
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    noteCommandLocked(opcode);
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    return transmit(packet, size, kWriteMs);
}

// Client thread: true when the command is answered here, not sent (BLE section): 0xFCE9 with a sub-operation in
// kRefusedLeMeta. Its Command Complete, status Unknown HCI Command, waits in `answers` for deliver() to pass it on
// between the controller's packets. Not while the controller's stream is not H4 (no packet boundary to put it at), nor
// with the queue full (bluetoothd has one command out at a time): the controller answers those.
bool SwitchBluetooth::answerHere(uint16_t opcode, const uint8_t *params, uint8_t length)
{
    if (opcode != OP_BCM_LE_META || !length || __atomic_load_n(&framingLost, __ATOMIC_RELAXED)) return false;
    bool refused = false;
    for (const uint8_t sub : kRefusedLeMeta) refused |= params[0] == sub;
    if (!refused) return false;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    const bool room = answerHead - answerTail < sizeof(answers) / sizeof(answers[0]);
    if (room) {
        uint8_t *const e = answers[answerHead++ % (sizeof(answers) / sizeof(answers[0]))];
        e[0] = H4_EVENT;
        e[1] = EV_COMMAND_COMPLETE;
        e[2] = 4;  // credits, opcode, status
        e[3] = lastCredits ? lastCredits : 1;
        e[4] = (uint8_t)opcode;
        e[5] = (uint8_t)(opcode >> 8);
        e[6] = kUnknownCommand;
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (room) __atomic_fetch_add(&st.answered, 1, __ATOMIC_RELAXED);
    return room;
}

// Work loop, client mode, at a packet boundary of the controller's stream: the completions answerHere made, passed on.
// False while the client's socket has no room for the next (ctlRcvd brings deliver() back).
bool SwitchBluetooth::passAnswers()
{
    constexpr uint32_t kSlots = sizeof(answers) / sizeof(answers[0]);
    for (;;) {
        uint8_t e[sizeof(answers[0])];
        IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
        const bool any = answerHead != answerTail;
        if (any) memcpy(e, answers[answerTail % kSlots], sizeof(e));
        IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
        if (!any) return true;
        if (!clientRoom(sizeof(e))) return false;
        if (ctl_enqueuedata(control, client, e, sizeof(e), 0) != 0) dropped += sizeof(e);
        s = IOSimpleLockLockDisableInterrupt(cmdLock);
        ++answerTail;
        IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    }
}

// Client thread: a scannable or non-connectable advertising interval below what a controller older than Core 5.0
// takes, raised to it (BLE section); 0xFD4A {instance, min, max, type, ...} and 0x2006 {min, max, type, ...}. True if
// it changed the command.
bool SwitchBluetooth::fixAdvertising(uint16_t opcode, uint8_t *params, uint8_t length)
{
    uint8_t *interval = nullptr, type = 0;
    if (opcode == OP_BCM_MULTI_ADV_PARAMS && length >= 6) {
        interval = params + 1;
        type = params[5];
    } else if (opcode == OP_LE_SET_ADV_PARAMS && length >= 5) {
        interval = params;
        type = params[4];
    } else {
        return false;
    }
    const uint8_t version = __atomic_load_n(&hciVersion, __ATOMIC_RELAXED);
    if (!version || version >= kHciVersion50 || (type != 2 && type != 3))
        return false;  // ADV_SCAN_IND, ADV_NONCONN_IND
    const uint16_t min = le16(interval), max = le16(interval + 2);
    if (min >= kAdvIntervalMinNonConnectable) return false;
    const uint16_t newMax = max < kAdvIntervalMinNonConnectable ? kAdvIntervalMinNonConnectable : max;
    interval[0] = (uint8_t)kAdvIntervalMinNonConnectable;
    interval[1] = (uint8_t)(kAdvIntervalMinNonConnectable >> 8);
    interval[2] = (uint8_t)newMax;
    interval[3] = (uint8_t)(newMax >> 8);
    if (!__atomic_exchange_n(&advFixLogged, true, __ATOMIC_RELAXED)) {
        IOLog("SwitchBluetooth: %s advertising interval %u-%u (%u ms) raised to %u-%u (%#06x): this controller (HCI "
              "version %u, older than 5.0) refuses it below 100 ms\n",
              type == 3 ? "non-connectable" : "scannable", min, max, min * 5 / 8, kAdvIntervalMinNonConnectable, newMax,
              opcode, version);
    }
    return true;
}

namespace {
void hex(char *out, size_t size, const uint8_t *bytes, uint32_t count)
{
    size_t at = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < count && at + 3 < size; ++i) at += (size_t)snprintf(out + at, size - at, "%02x", bytes[i]);
}
}

// Client thread: the first LE and vendor command of each opcode (and 0xFCE9 sub-operation) and the event mask
// commands, kept for traceCompletion; one answered here is logged at once. os_log only, once per boot each.
void SwitchBluetooth::traceCommand(uint16_t opcode, const uint8_t *params, uint8_t length, bool answered)
{
    const uint16_t ogf = opcode >> 10;
    bool setup = false;  // the controller settings that shape every link (Links), and what it reports of itself
    for (const uint16_t op : kTracedSetup) setup |= opcode == op;
    if (ogf != kOgfLe && ogf != kOgfVendor && opcode != OP_SET_EVENT_MASK && opcode != OP_SET_EVENT_MASK_2 &&
        opcode != OP_WRITE_LE_HOST_SUPPORT && !setup)
        return;
    const uint8_t sub = opcode == OP_BCM_LE_META && length ? params[0] : 0;
    Traced entry = {};
    bool log = false;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    uint32_t i = 0;
    while (i < tracedCount && !(traced[i].opcode == opcode && traced[i].sub == sub)) ++i;
    if (i == tracedCount && tracedCount < kTraceEntries) {
        Traced &t = traced[tracedCount++];
        t.opcode = opcode;
        t.sub = sub;
        t.length = length;
        memcpy(t.params, params, length < kTraceParams ? length : kTraceParams);
        t.state = answered ? 2 : 1;
        entry = t;
        log = answered;
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (!log) return;
    char p[2 * kTraceParams + 1];
    hex(p, sizeof(p), entry.params, entry.length < kTraceParams ? entry.length : kTraceParams);
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: HCI %04x/%02x %u bytes [%s] -> answered here: Unknown HCI Command", opcode,
           sub, entry.length, p);
}

// Work loop: the completion of a command traceCommand kept, logged with its parameters (once per boot each).
void SwitchBluetooth::traceCompletion(uint16_t opcode, bool status, uint8_t result, const uint8_t *ret,
                                      uint32_t retLength)
{
    Traced entry = {};
    bool log = false;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    for (uint32_t i = 0; i < tracedCount && !log; ++i) {
        if (traced[i].opcode != opcode || traced[i].state != 1) continue;
        traced[i].state = 2;
        entry = traced[i];
        log = true;
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (!log) return;
    char p[2 * kTraceParams + 1], r[2 * kTraceReturn + 1];
    hex(p, sizeof(p), entry.params, entry.length < kTraceParams ? entry.length : kTraceParams);
    hex(r, sizeof(r), ret, retLength);
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: HCI %04x/%02x %u bytes [%s] -> Command %s, status %#04x [%s]", opcode,
           entry.sub, entry.length, p, status ? "Status" : "Complete", result, r);
}

// ---- Links -------------------------------------------------------------------------------------------------------
//
// What every connection does, for a controller that holds several at once (two Joy-Cons and AirPods: two classic HID
// links in sniff and an A2DP one). The table (SwitchBluetoothLinks.h, cmdLock) follows each connection handle from its
// Connection Complete to its Disconnection Complete: the ACL packets each way, the controller's completions for them
// (the credit accounting bluetoothd does across all links from Read Buffer Size's total), the most ever outstanding, the
// role and sniff mode; the stats line carries it. The link trace (os_log, kLinkTraceLines a boot) logs every command
// that makes, accepts, secures, shapes or ends a link, with its Command Status or Complete, and every link event:
// connection request and complete, role and mode changes, link key and IO capability requests, authentication,
// encryption, disconnection (with the link's counts), max slots, supervision timeout, Hardware Error and Data Buffer
// Overflow. A Link Key Request Reply is logged without its key.

// Any thread: one link trace line ("->" to the controller, "<-" from it), with a closed link's counts; the last line of
// the budget says the rest are not logged.
void SwitchBluetooth::linkLog(const char *direction, const char *line, const switch_bt_link *last)
{
    const uint32_t n = __atomic_fetch_add(&linkLines, 1, __ATOMIC_RELAXED);
    if (n > kLinkTraceLines) return;
    if (n == kLinkTraceLines) {
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: link trace: %u lines this boot, no more", kLinkTraceLines);
        return;
    }
    if (last)
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: link %s %s: rx %u tx %u done %u outstanding %u peak %u", direction,
               line, last->rx, last->tx, last->done, last->tx - last->done, last->peak);
    else
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: link %s %s", direction, line);
}

// Client thread (clientCommand): a link command on its way to the controller.
void SwitchBluetooth::linkCommand(uint16_t opcode, const uint8_t *params, uint8_t length)
{
    if (!switch_bt_link_command_name(opcode)) return;
    char line[160];
    switch_bt_describe_command(line, sizeof(line), opcode, params, length);
    linkLog("->", line);
}

// Work loop (frameRx): the Command Status or Complete of a link command.
void SwitchBluetooth::linkCompletion(uint16_t opcode, bool status, uint8_t result)
{
    const char *name = switch_bt_link_command_name(opcode);
    if (!name) return;
    char line[96];
    snprintf(line, sizeof(line), "Command %s for %s: 0x%02x", status ? "Status" : "Complete", name, result);
    linkLog("<-", line);
}

// Work loop (frameRx): a whole link event at the ring's tail (`length` parameter bytes after its 3-byte H4 header),
// into the table and the trace. Number Of Completed Packets is only counted.
void SwitchBluetooth::linkEvent(uint8_t code, uint32_t length)
{
    uint8_t *const p = linkParams;
    const uint32_t tail = rx.tail;
    for (uint32_t i = 0; i < length; ++i) p[i] = rx.data[(tail + 3 + i) & (kRingBytes - 1)];
    switch_bt_link closed = {};
    bool wasOpen = false;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    switch (code) {
    case SWITCH_BT_EV_NUM_COMPLETED: (void)switch_bt_links_completed(&links, p, length); break;
    case SWITCH_BT_EV_CONNECTION_REQUEST:
        if (length >= 10) switch_bt_request_note(&requests, p, (uint32_t)(p[6] | p[7] << 8 | p[8] << 16));
        break;
    case SWITCH_BT_EV_CONNECTION_COMPLETE:
        if (length >= 11 && !p[0])
            (void)switch_bt_link_open(&links, switch_bt_le16(p + 1), SWITCH_BT_LINK_ACL, p + 3,
                                      memcmp(p + 3, roleAddr, 6) ? (uint8_t)SWITCH_BT_ROLE_UNKNOWN : role);
        break;
    case SWITCH_BT_EV_LE_META:
        if (length >= 19 && !p[1])
            (void)switch_bt_link_open(&links, (uint16_t)(switch_bt_le16(p + 2) & 0x0fffu), SWITCH_BT_LINK_LE, p + 6,
                                      p[4]);
        break;
    case SWITCH_BT_EV_DISCONNECTION_COMPLETE:
        if (length >= 4 && !p[0]) wasOpen = switch_bt_link_close(&links, switch_bt_le16(p + 1), &closed);
        break;
    case SWITCH_BT_EV_ROLE_CHANGE:
        if (length >= 8 && !p[0]) {
            memcpy(roleAddr, p + 1, 6);
            role = p[7];
            for (switch_bt_link &l : links.link)
                if (l.kind != SWITCH_BT_LINK_FREE && !memcmp(l.addr, p + 1, 6)) l.role = p[7];
        }
        break;
    case SWITCH_BT_EV_MODE_CHANGE: {
        switch_bt_link *l = length >= 6 && !p[0] ? switch_bt_link_find(&links, switch_bt_le16(p + 1)) : nullptr;
        if (l) {
            l->mode = p[3];
            l->interval = switch_bt_le16(p + 4);
        }
        break;
    }
    case SWITCH_BT_EV_ENCRYPTION_CHANGE: {
        switch_bt_link *l = length >= 4 && !p[0] ? switch_bt_link_find(&links, switch_bt_le16(p + 1)) : nullptr;
        if (l) l->encrypted = p[3];
        break;
    }
    default: break;
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (code == SWITCH_BT_EV_NUM_COMPLETED) return;
    switch_bt_describe_event(linkText, sizeof(linkText), code, p, length);
    linkLog("<-", linkText, wasOpen ? &closed : nullptr);
}

// Client thread (clientCommand): bluetoothd's Accept Connection Request for a gamepad that paged us, Role 1 (stay
// peripheral) made 0 (become central; SwitchBluetoothLinks.h). One line the first time.
bool SwitchBluetooth::acceptAsCentral(uint8_t *params, uint8_t length)
{
    uint32_t cod = 0;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    const bool changed = switch_bt_accept_as_central(&requests, params, length, &cod);
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (changed && !__atomic_exchange_n(&centralLogged, true, __ATOMIC_RELAXED)) {
        char a[18];
        switch_bt_format_addr(a, params);
        IOLog(
            "SwitchBluetooth: gamepad %s (class %#08x) accepted as central, so the controller switches roles while it "
            "connects instead of the gamepad after it\n",
            a, cod);
    }
    return changed;
}

// Work loop: a completion with a non-zero status, counted by opcode and status for the stats line.
void SwitchBluetooth::countCompletion(uint16_t opcode, uint8_t result)
{
    if (!result) return;
    Failed *slot = nullptr;
    for (Failed &f : failed) {
        if (f.count && f.opcode == opcode && f.status == result) {
            slot = &f;
            break;
        }
        if (!f.count && !slot) slot = &f;
    }
    if (!slot) return;  // eight kinds already; the stats line shows those
    if (!slot->count) {
        slot->opcode = opcode;
        slot->status = result;
    }
    ++slot->count;
}

// Work loop, client mode: one line for what changed since the last, at most every kStatsMs and only when something
// did (force: whatever the time, as the client goes). os_log only, never the console.
void SwitchBluetooth::logStats(bool force)
{
    const uint64_t now = mach_absolute_time();
    if (!force && now - statsAt < statsWindow) return;
    Stats n = st;
    n.commands = __atomic_load_n(&st.commands, __ATOMIC_RELAXED);
    n.leCommands = __atomic_load_n(&st.leCommands, __ATOMIC_RELAXED);
    n.vendorCommands = __atomic_load_n(&st.vendorCommands, __ATOMIC_RELAXED);
    n.answered = __atomic_load_n(&st.answered, __ATOMIC_RELAXED);
    n.advFixed = __atomic_load_n(&st.advFixed, __ATOMIC_RELAXED);
    n.scanOn = __atomic_load_n(&st.scanOn, __ATOMIC_RELAXED);
    n.scanOff = __atomic_load_n(&st.scanOff, __ATOMIC_RELAXED);
    n.central = __atomic_load_n(&st.central, __ATOMIC_RELAXED);
    const uint32_t irqsNow = irqs, rxNow = rxBytes, txNow = txBytes;
    const Stats &o = stLogged;
    if (n.events == o.events && n.acl == o.acl && n.sco == o.sco && n.commands == o.commands &&
        n.answered == o.answered) {
        statsAt = now;
        return;
    }
    char fails[200] = "";
    size_t at = 0;
    for (uint32_t i = 0; i < sizeof(failed) / sizeof(failed[0]) && at + 24 < sizeof(fails); ++i) {
        if (failed[i].count == failedLogged[i].count) continue;
        at += (size_t)snprintf(fails + at, sizeof(fails) - at, " %04x=%#04x x%u", failed[i].opcode, failed[i].status,
                               failed[i].count - failedLogged[i].count);
    }
    // The links as they are now (totals since each came up; "Links" section): tx - done is what each holds of the
    // controller's ACL buffers.
    switch_bt_links &snapshot = statsLinks;
    IOInterruptState locked = IOSimpleLockLockDisableInterrupt(cmdLock);
    snapshot = links;
    IOSimpleLockUnlockEnableInterrupt(cmdLock, locked);
    char *const held = statsHeld;
    constexpr size_t kHeld = sizeof(statsHeld);
    held[0] = '\0';
    size_t used = 0;  // what is in `held`; an entry that would not fit ends it (snprintf returns what it wanted)
    for (const switch_bt_link &l : snapshot.link) {
        if (l.kind == SWITCH_BT_LINK_FREE) continue;
        char a[18];
        switch_bt_format_addr(a, l.addr);
        const int w = snprintf(held + used, kHeld - used,
                               " %04x %s %s role %s %s %u, rx %u tx %u done %u outstanding %u peak %u;", l.handle, a,
                               l.kind == SWITCH_BT_LINK_LE ? "LE" : "ACL",
                               l.role == 0   ? "central"
                               : l.role == 1 ? "peripheral"
                                             : "?",
                               l.mode == 2 ? "sniff" : "active", l.interval, l.rx, l.tx, l.done, l.tx - l.done, l.peak);
        if (w < 0 || used + (size_t)w >= kHeld) {
            held[used] = '\0';
            break;
        }
        used += (size_t)w;
    }
    if (snapshot.strayTx || snapshot.strayDone) {
        const int w = snprintf(held + used, kHeld - used, " unknown handles: tx %u done %u", snapshot.strayTx,
                               snapshot.strayDone);
        if (w < 0 || used + (size_t)w >= kHeld) held[used] = '\0';
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(now - statsAt, &ns);
    char *const line = statsLine;
    snprintf(
        line, sizeof(statsLine),
        "SwitchBluetooth: stats %llu ms: rx %u B, tx %u B, %u interrupts, %u wakeups; from the controller %u events "
        "(complete %u, status %u, completed packets %u, vendor %u, other %u), LE meta %u (advertising reports "
        "%u in %u events, connections %u, other %u), ACL %u, SCO %u; to it %u commands (LE %u, vendor %u), "
        "answered here %u, advertising intervals raised %u, scan on %u off %u, gamepads accepted as central %u; "
        "failed:%s; "
        "links:%s",
        (unsigned long long)(ns / 1000000), rxNow - rxBytesLogged, txNow - txBytesLogged, irqsNow - irqsLogged,
        n.actions - o.actions, n.events - o.events, n.cc - o.cc, n.cs - o.cs, n.nocp - o.nocp, n.vendor - o.vendor,
        n.otherEvents - o.otherEvents, n.le - o.le, n.leAdvReports - o.leAdvReports, n.leAdv - o.leAdv,
        n.leConn - o.leConn, n.leOther - o.leOther, n.acl - o.acl, n.sco - o.sco, n.commands - o.commands,
        n.leCommands - o.leCommands, n.vendorCommands - o.vendorCommands, n.answered - o.answered,
        n.advFixed - o.advFixed, n.scanOn - o.scanOn, n.scanOff - o.scanOff, n.central - o.central,
        at ? fails : " none", held[0] ? held : " none");
    os_log(OS_LOG_DEFAULT, "%s", line);
    stLogged = n;
    memcpy(failedLogged, failed, sizeof(failed));
    irqsLogged = irqsNow;
    rxBytesLogged = rxNow;
    txBytesLogged = txNow;
    statsAt = now;
}

// Work loop, RX masked (rxStalled) and room in the ring again: RX interrupts back on, then what the FIFO already
// holds is taken here, bounded like one interrupt. A residue below the RX trigger (8) whose character timeout ran
// out while RX was masked raises neither RDI nor the RX timeout once unmasked: the 7 bytes left of a 71-byte
// Command Complete (Read Local Supported Commands, 64 taken by the budget) sat in the FIFO and bluetoothd's stack
// waited on them for good. At most one FIFO's worth is taken, without masking RX again: that is every byte that was in
// the FIFO when RX came back on; later ones raise RDI or the RX timeout themselves. So deliver() passes these on and
// returns. (It used to loop here while each drain spent a 64-byte budget, holding the work-loop gate for as long as
// the controller kept sending; a bring-up's runAction then waited on it for good.) True when bytes were taken.
bool SwitchBluetooth::resumeRx()
{
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
    rxStalled = false;
    bool took = false;
    if (chipOn && !rxFault) {  // power() and a fault mask RX until a flush (the one after power-on) enables it
        ier |= IER_RX;
        w32(uart, IER, ier);
        const uint32_t before = rx.head;
        uint32_t budget = kFifoBytes;
        (void)drainRx(budget, false);
        took = rx.head != before;
    }
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
    return took;
}

// After a wait timed out: if the UART shows a condition the filter would have serviced within microseconds
// (received data with RX interrupts on, or an empty TX FIFO with bytes queued and THRI on) and still does 50 ms
// later with no interrupt taken and no action outstanding, the interrupt is lost. A chip holding CTS keeps THRE
// clear, so a silent or stuck chip is a timeout, not a panic.
void SwitchBluetooth::checkDelivery(const char *what)
{
    uint32_t before = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
        const uint32_t lsr = r32(uart, LSR), enabled = ier, seen = irqs;
        const bool pending = !actionDue && (((lsr & LSR_DR) && (enabled & IER_RDI) && !rxStalled) ||
                                            (tx.used() && (enabled & IER_THRI) && (lsr & LSR_THRE)));
        IOSimpleLockUnlockEnableInterrupt(hwLock, s);
        if (!pending || (pass && seen != before)) return;
        if (pass) {
            char line[400];
            describe(line, sizeof(line));
            panic("SwitchBluetooth: UART-D interrupt lost during %s: %s", what, line);
        }
        before = seen;
        IOSleep(kGraceMs);
    }
}

// One line for a panic or a failed bring-up: UART, pins, clock, counters and the GIC's view of INTID 122.
void SwitchBluetooth::describe(char *buffer, size_t size)
{
    char gic[160];
    ioswitch_interrupt_describe(kIntid, gic, sizeof(gic));
    const uint32_t port = portReg(PH4, 0);
    auto mux = [this](uint32_t o) { return r32(pinmux, o) & PM_LOW; };
    snprintf(buffer, size,
             "lsr %#x msr %#x (cts %u) ier %#x mcr %#x lcr %#x baud %u; rx %u tx %u queued %u irqs %u (idle %u) "
             "errors %u rx-resets %u rx-faults %u (last %s)%s dropped %u resyncs %u hw-errors %u fixes %u strays %u; "
             "gpio H cnf/oe/out/in "
             "%02x/%02x/%02x/%02x; pinmux %x/%x/%x/%x %x/%x/%x; car clk %u rst %u src %#x; %s",
             r32(uart, LSR), r32(uart, MSR), !!(r32(uart, MSR) & MSR_CTS), ier, r32(uart, MCR), r32(uart, LCR), baud,
             rxBytes, txBytes, tx.used(), irqs, idleIrqs, lineErrors, rxResets, rxFaults, lastFault,
             rxFault ? " RX off" : "", dropped, resyncs, hardwareErrors, completionFixes, strayDrops,
             r32(gpio, port + GPIO_CNF) & 0xff, r32(gpio, port + GPIO_OE) & 0xff, r32(gpio, port + GPIO_OUT) & 0xff,
             r32(gpio, port + GPIO_IN) & 0xff, mux(0x114), mux(0x118), mux(0x11c), mux(0x120), mux(0x1c0), mux(0x1c4),
             mux(0x1c8), !!(r32(car, CLK_OUT_ENB_U) & kUartDBit), !!(r32(car, RST_DEVICES_U) & kUartDBit),
             r32(car, CLK_SOURCE_UARTD), gic);
}

// Every wait in a bring-up or a connect is bounded, so one that is still running after kStuckMs has stopped for a
// reason the driver does not know: one line, once per boot, so that Bluetooth never fails silently.
void SwitchBluetooth::watch(const char *what)
{
    uint64_t deadline;
    clock_interval_to_deadline(kStuckMs, kMillisecondScale, &deadline);
    thread_call_enter1_delayed(stuckCall, const_cast<char *>(what), deadline);
}
void SwitchBluetooth::unwatch()
{
    thread_call_cancel(stuckCall);
}
void SwitchBluetooth::stuck(thread_call_param_t owner, thread_call_param_t what)
{
    static bool said;
    if (__atomic_exchange_n(&said, true, __ATOMIC_RELAXED)) return;
    char line[400];
    static_cast<SwitchBluetooth *>(owner)->describe(line, sizeof(line));
    IOLog("SwitchBluetooth: %s still running after %u s: [%s]\n", static_cast<const char *>(what), kStuckMs / 1000,
          line);
}

// ---- Transmit and the kernel's HCI -------------------------------------------------------------------------------

// Queues all of `bytes`, waiting (by interrupt) for ring space. One writer at a time.
int SwitchBluetooth::transmit(const uint8_t *bytes, size_t length, uint32_t timeoutMs)
{
    IOLockLock(txLock);
    uint64_t deadline;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    while (length) {
        // txWanted goes up before the check, so the interrupt that frees the space also wakes us.
        __atomic_store_n(&txWanted, true, __ATOMIC_SEQ_CST);
        uint32_t room = tx.space();
        if (!room) {
            const int r = IOLockSleepDeadline(txLock, &tx, deadline, THREAD_UNINT);
            if (r == THREAD_TIMED_OUT && !tx.space()) {
                txWanted = false;
                IOLockUnlock(txLock);
                checkDelivery("transmit");
                return ETIMEDOUT;
            }
            continue;
        }
        txWanted = false;
        uint32_t head = tx.head;
        for (; room && length; --room, --length) tx.data[head++ & (kRingBytes - 1)] = *bytes++;
        __atomic_store_n(&tx.head, head, __ATOMIC_RELEASE);
        kickTx();
    }
    IOLockUnlock(txLock);
    return 0;
}

// Waits until everything queued has left the ring, then for the FIFO and shift register at the current rate
// (TEMT has no interrupt; 33 characters is the worst case).
int SwitchBluetooth::drainTx(uint32_t timeoutMs)
{
    IOLockLock(txLock);
    uint64_t deadline;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    __atomic_store_n(&txWanted, true, __ATOMIC_SEQ_CST);
    while (tx.used()) {
        const int r = IOLockSleepDeadline(txLock, &tx, deadline, THREAD_UNINT);
        if (r == THREAD_TIMED_OUT && tx.used()) {
            txWanted = false;
            IOLockUnlock(txLock);
            checkDelivery("transmit drain");
            return ETIMEDOUT;
        }
    }
    txWanted = false;
    IOLockUnlock(txLock);
    IODelay((kFifoBytes + 1) * 10 * 1000000 / baud + 1);
    return 0;
}

// Work loop, kernel mode only: H4 framing; events go to event(), ACL/SCO and garbage are skipped.
void SwitchBluetooth::parse(const uint8_t *bytes, uint32_t length)
{
    H4Parser &p = h4;
    for (uint32_t i = 0; i < length; ++i) {
        const uint8_t b = bytes[i];
        switch (p.state) {
        case 0:
            p.type = b;
            p.have = 0;
            p.need = b == H4_EVENT ? 2 : b == H4_ACL ? 4 : b == H4_SCO ? 3 : 0;
            if (p.need)
                p.state = 1;
            else
                ++resyncs;
            break;
        case 1:
            p.header[p.have++] = b;
            if (p.have < p.need) break;
            p.length = p.type == H4_EVENT ? p.header[1] : p.type == H4_ACL ? le16(p.header + 2) : p.header[2];
            p.got = 0;
            p.state = p.length ? 2 : 0;
            if (!p.length && p.type == H4_EVENT) event(p.header[0], p.payload, 0);
            break;
        default:
            if (p.type == H4_EVENT) p.payload[p.got] = b;
            if (++p.got < p.length) break;
            if (p.type == H4_EVENT) event(p.header[0], p.payload, (uint8_t)p.length);
            p.state = 0;
            break;
        }
    }
}

// Completes the command in flight: Command Complete for its opcode, a failed Command Status, or a Hardware Error.
void SwitchBluetooth::event(uint8_t code, const uint8_t *params, uint8_t length)
{
    IOLockLock(hciLock);
    bool done = false;
    if (code == EV_COMMAND_COMPLETE && length >= 4 && cmd.waiting && le16(params + 1) == cmd.opcode) {
        cmd.length = (uint8_t)(length - 3);
        memcpy(cmd.data, params + 3, cmd.length);
        done = true;
    } else if (code == EV_COMMAND_STATUS && length >= 4 && cmd.waiting && le16(params + 2) == cmd.opcode && params[0]) {
        cmd.length = 1;
        cmd.data[0] = params[0];
        done = true;
    } else if (code == EV_HARDWARE_ERROR) {
        ++hardwareErrors;
        cmd.hardwareError = true;
        done = cmd.waiting;
    }
    if (done) {
        cmd.done = true;
        IOLockWakeup(hciLock, &cmd, false);
    }
    IOLockUnlock(hciLock);
}

// One HCI command and its completion. Returns 0, the controller's status as EIO, EPROTO for a Hardware Error, or
// ETIMEDOUT (after the lost-interrupt check).
int SwitchBluetooth::command(uint16_t opcode, const uint8_t *params, uint8_t length, uint32_t timeoutMs, uint8_t *out,
                             uint8_t *outLength)
{
    uint8_t packet[4 + 255] = {H4_COMMAND, (uint8_t)opcode, (uint8_t)(opcode >> 8), length};
    if (length) memcpy(packet + 4, params, length);
    IOLockLock(hciLock);
    cmd.opcode = opcode;
    cmd.waiting = true;
    cmd.done = cmd.hardwareError = false;
    IOLockUnlock(hciLock);
    int error = transmit(packet, 4u + length, kWriteMs);
    IOLockLock(hciLock);
    if (!error) {
        uint64_t deadline;
        clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
        while (!cmd.done && IOLockSleepDeadline(hciLock, &cmd, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
    }
    const bool done = cmd.done, hardwareError = cmd.hardwareError;
    cmd.waiting = false;
    if (done && !hardwareError && out) {
        memcpy(out, cmd.data, cmd.length);
        if (outLength) *outLength = cmd.length;
    }
    const uint8_t status = cmd.length ? cmd.data[0] : 0;
    IOLockUnlock(hciLock);
    if (error) return error;
    if (hardwareError) return EPROTO;
    if (!done) {
        checkDelivery("an HCI command");
        return ETIMEDOUT;
    }
    return status ? EIO : 0;
}

// Host UART at `rate`, stale bytes dropped, then HCI Reset up to `tries` times. EPROTO (Hardware Error) ends it:
// the bytes framed correctly, so the rate is right and only a power cycle helps (nxk bcm.rs reset()).
int SwitchBluetooth::resetAt(uint32_t rate, unsigned tries)
{
    if (rate != baud) setHostBaud(rate);
    int error = ETIMEDOUT;
    for (unsigned i = 0; i < tries && error != EPROTO; ++i) {
        flushRx();
        error = command(OP_RESET, nullptr, 0, kCommandMs);
        if (!error) break;
    }
    return error;
}

// Controller and host to 3 Mbaud (Broadcom 0xFC18: two zero bytes, the rate little-endian); false leaves both at
// the old rate (nxk go_fast).
bool SwitchBluetooth::fast()
{
    const uint32_t old = baud;
    const uint8_t p[6] = {
        0, 0, (uint8_t)kFastBaud, (uint8_t)(kFastBaud >> 8), (uint8_t)(kFastBaud >> 16), (uint8_t)(kFastBaud >> 24)};
    if (command(OP_BCM_UART_BAUD, p, sizeof(p), kCommandMs)) return false;
    setHostBaud(kFastBaud);
    IOSleep(10);
    flushRx();
    if (!command(OP_READ_VERSION, nullptr, 0, kCommandMs)) return true;
    setHostBaud(old);
    flushRx();
    return false;
}

// One power cycle and bring-up (header comment). On failure the core is left off and `why` says where it stopped.
bool SwitchBluetooth::bringUp(uint32_t offMs, char *why, size_t whySize)
{
    watch("bring-up");
    const uint32_t faultsBefore = rxFaults;
    setMode(kKernel);
    power(false);
    chipAddressSet = false;  // a power cycle takes the controller back to its own address
    setPin(PH3, GPIO_OUT, false);
    IOSleep(offMs);
    setHostBaud(kInitBaud);
    power(true);
    IOSleep(kPowerOnMs);

    uint8_t v[255], n = 0;
    int error = resetAt(kInitBaud, 6);
    if (error == ETIMEDOUT) error = resetAt(kFastBaud, 2);  // a chip that kept another rate across the cycle
    if (error) {
        snprintf(why, whySize, "no HCI Reset answer (error %d)", error);
        goto fail;
    }
    if ((error = command(OP_READ_VERSION, nullptr, 0, kCommandMs, v, &n)) || n < 9) {
        snprintf(why, whySize, "Read Local Version failed (error %d)", error);
        goto fail;
    }
    {
        const uint16_t romSubversion = le16(v + 7);
        // A failed switch leaves the host at 115200 for the download; the chip may have switched all the same.
        const bool fastOk = baud == kFastBaud || fast();

        // Patch RAM: the minidriver, then the file's own records, the last launching it (btbcm_patchram).
        uint32_t records = 0;
        if (patch) {
            const uint8_t *p = static_cast<const uint8_t *>(patch->getBytesNoCopy());
            const uint32_t size = patch->getLength();
            if ((error = command(OP_BCM_MINIDRIVER, nullptr, 0, kRecordMs))) {
                snprintf(why, whySize, "download minidriver failed (error %d)%s", error,
                         fastOk ? "" : " after the 3 Mbaud switch failed");
                goto fail;
            }
            IOSleep(kMinidriverMs);
            for (uint32_t at = 0; at + 3 <= size; ++records) {
                const uint16_t op = le16(p + at);
                const uint8_t len = p[at + 2];
                if ((error = command(op, p + at + 3, len, kRecordMs))) {
                    snprintf(why, whySize, "patch record %u (opcode %#06x) failed (error %d)", records, op, error);
                    goto fail;
                }
                at += 3u + len;
            }
            IOSleep(kLaunchMs);
            // The patched firmware restarts the UART at its default rate; nxk also saw it want BT_WAKE high once.
            error = resetAt(kInitBaud, 2);
            if (error == ETIMEDOUT) error = resetAt(kFastBaud, 2);
            if (error == ETIMEDOUT) {
                setPin(PH3, GPIO_OUT, true);
                IOSleep(20);
                error = resetAt(kInitBaud, 2);
                if (error == ETIMEDOUT) error = resetAt(kFastBaud, 2);
                setPin(PH3, GPIO_OUT, false);
            }
            if (error) {
                snprintf(why, whySize, "no answer after the patch (%u records, error %d)", records, error);
                goto fail;
            }
            if (baud != kFastBaud && !fast()) {
                snprintf(why, whySize, "patched firmware refused 3 Mbaud");
                goto fail;
            }
        } else if (baud != kFastBaud) {
            snprintf(why, whySize, "ROM firmware refused 3 Mbaud");
            goto fail;
        }
        if ((error = command(OP_READ_VERSION, nullptr, 0, kCommandMs, v, &n)) || n < 9) {
            snprintf(why, whySize, "Read Local Version after the patch failed (error %d)", error);
            goto fail;
        }
        const uint16_t manufacturer = le16(v + 5), subversion = le16(v + 7);
        __atomic_store_n(&hciVersion, v[1], __ATOMIC_RELAXED);  // status, HCI version, revision, LMP version, ...
        uint8_t name[255] = {};
        if (command(OP_READ_LOCAL_NAME, nullptr, 0, kCommandMs, name, &n) || n < 2)
            strlcpy((char *)name + 1, "unnamed", 16);
        name[sizeof(name) - 1] = 0;

        // Sleep mode off (12 zero bytes, nxk): BT_WAKE stays asserted and host wake is unused. bluetoothd may
        // configure its own later.
        const uint8_t sleep[12] = {};
        (void)command(OP_BCM_SLEEP_MODE, sleep, sizeof(sleep), kCommandMs);
        if (haveAddress) {
            const uint8_t le[6] = {address[5], address[4], address[3], address[2], address[1], address[0]};
            if ((error = command(OP_BCM_WRITE_BDADDR, le, sizeof(le), kCommandMs))) {
                snprintf(why, whySize, "Write BD_ADDR failed (error %d)", error);
                goto fail;
            }
            memcpy(chipAddress, address, sizeof(chipAddress));
            chipAddressSet = true;
        }
        setMode(kIdle);
        IOLog("SwitchBluetooth: %s up at %u baud, manufacturer %#06x, LMP subversion %#06x (ROM %#06x), %s %u records, "
              "BD %02x:%02x:%02x:%02x:%02x:%02x (%s), HCI version %u\n",
              (const char *)name + 1, baud, manufacturer, subversion, romSubversion,
              patch ? "patch RAM" : "ROM firmware, patch", records, address[0], address[1], address[2], address[3],
              address[4], address[5], haveAddress ? addressFrom : "none: chip default", v[1]);
        if (manufacturer != kBroadcom) IOLog("SwitchBluetooth: manufacturer %#06x is not Broadcom\n", manufacturer);
        setProperty("local-name", (const char *)name + 1);
        setProperty("lmp-subversion", subversion, 16);
        setProperty("hci-version", v[1], 8);
        setProperty("baud", baud, 32);
        unwatch();
        return true;
    }
fail:
    // RX masked for a fault during the attempt: which one (r7h said "the line held low" for what was a storm).
    if (rxFaults != faultsBefore) {
        char masked[80];
        snprintf(masked, sizeof(masked), ", RX masked for %s", lastFault);
        strlcat(why, masked, whySize);
    }
    setMode(kIdle);
    power(false);
    unwatch();
    return false;
}

// ---- Work loop gated actions -------------------------------------------------------------------------------------

IOReturn SwitchBluetooth::gated(OSObject *owner, void *what, void *, void *, void *)
{
    auto *self = static_cast<SwitchBluetooth *>(owner);
    const uintptr_t op = reinterpret_cast<uintptr_t>(what);
    if (op == 0) {
        // Flush: the RX FIFO, then the ring and the H4 parser. Under hwLock the filter cannot produce meanwhile.
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(self->hwLock);
        self->resetRxFifoLocked();
        __atomic_store_n(&self->rx.tail, __atomic_load_n(&self->rx.head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        // The ring and FIFO a stall waited on are gone, so nothing else would unmask RX; and this is where RX comes
        // back on after power-on (power() keeps it off for an unpowered core) and after a fault (maskRxFaultLocked).
        self->rxStalled = false;
        self->rxFault = false;
        self->rxFaultRun = 0;
        self->stormCleared = false;
        self->rxFrame = {};  // the controller's next byte starts a packet
        self->rxPacketDone = false;
        uint32_t ier = self->chipOn ? self->ier | IER_RX : self->ier & ~IER_RX;
        if (self->tx.used()) ier |= IER_THRI;  // a fault that took every source left bytes queued
        if (ier != self->ier) {
            self->ier = ier;
            w32(self->uart, IER, ier);
        }
        IOSimpleLockUnlockEnableInterrupt(self->hwLock, s);
        self->h4.state = 0;
    } else if (op == 1) {
        self->deliver();
    } else {
        const Mode was = self->mode;
        self->mode = static_cast<Mode>(op - 2);
        if (was == kClient && self->mode != kClient) self->logStats(true);  // the client's last stats line
        if (self->mode == kClient && was != kClient) self->statsAt = mach_absolute_time();
        if (self->mode == kClient) self->resetClientFraming();
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(self->hwLock);
        self->framePackets = self->mode == kClient;
        if (self->framePackets && !self->rx.used()) self->syncFramerLocked();
        IOSimpleLockUnlockEnableInterrupt(self->hwLock, s);
        __atomic_store_n(&self->clientBlocked, false, __ATOMIC_SEQ_CST);
        self->deliver();  // what the ring holds goes by the new mode, and a stall held for the old client resumes
    }
    return kIOReturnSuccess;
}

// hwLock held. The RX FIFO as Linux tegra_uart_fifo_reset does on T210, which cannot clear it in FIFO mode: FIFO mode
// off, clear, on (the TX FIFO empties with it), then 32 UART clocks for the clear to propagate.
void SwitchBluetooth::resetRxFifoLocked()
{
    rxFrame.lost = true;  // whatever the FIFO held is gone (a flush starts the framing afresh after this)
    const uint32_t fcr = FCR_RX_TRIG_8 | FCR_TX_TRIG_16;
    w32(uart, FCR, fcr);
    IODelay(60);
    w32(uart, FCR, fcr | FCR_CLEAR_RX);
    w32(uart, FCR, fcr | FCR_ENABLE);
    (void)r32(uart, SPR);
    IODelay(20);  // 32 UART clocks (serial-tegra), generous at any rate here
}

void SwitchBluetooth::flushRx()
{
    loop->runAction(&SwitchBluetooth::gated, this, reinterpret_cast<void *>(0));
}
void SwitchBluetooth::setMode(Mode m)
{
    loop->runAction(&SwitchBluetooth::gated, this, reinterpret_cast<void *>(uintptr_t(m) + 2));
}

// ---- BD address --------------------------------------------------------------------------------------------------

// The address where bluetoothd (/arm-io/bluetooth local-mac-address) and MobileGestalt (/chosen
// mac-address-bluetooth0) read it, as publishAddress() puts the fuses' one.
void SwitchBluetooth::publishAddressProperties()
{
    publishAddressBytes(address);
}

// bluetoothd's first connect (see the header): the staged address as `address`, if there is one and it is not that
// already. Normally switch_bluetooth_data_mounted found it before launchd and published it, so bluetoothd has read it
// as local-mac-address from its start and this only takes it over for the controller. A kernel that never called that
// hook (no Data mount of its own) reads the file here, as before: this runs on bluetoothd's thread, so with the
// kernel's context (bluetoothd's sandbox would refuse the path). Returns whether `address` changed.
bool SwitchBluetooth::adoptStagedAddress()
{
    uint8_t staged[6];
    const int found = __atomic_load_n(&gStagedState, __ATOMIC_ACQUIRE);
    if (found == kStagedFound)
        memcpy(staged, gStaged, sizeof(staged));
    else if (found == kStagedNone || !readStagedAddress(staged))
        return false;
    if (haveAddress && !memcmp(staged, address, 6)) return false;
    IOLog("SwitchBluetooth: BD %02x:%02x:%02x:%02x:%02x:%02x from the staged Joy-Con pairing (Horizon's), was "
          "%02x:%02x:%02x:%02x:%02x:%02x (%s)%s\n",
          staged[0], staged[1], staged[2], staged[3], staged[4], staged[5], address[0], address[1], address[2],
          address[3], address[4], address[5], haveAddress ? addressFrom : "chip default",
          found == kStagedFound ? "" : "; bluetoothd read the old one at its start");
    memcpy(address, staged, 6);
    haveAddress = true;
    addressFrom = "staged";
    publishAddressProperties();
    return true;
}

// ---- Kernel control ----------------------------------------------------------------------------------------------

// Waits for the first bring-up, brings the chip up again if the last client used it, the last attempt failed or the
// line faulted since (maskRxFaultLocked: the core stopped driving it or the UART wedged), then hands the stream over
// with nothing stale in it. One client at a time.
errno_t SwitchBluetooth::ctlConnect(kern_ctl_ref, struct sockaddr_ctl *sac, void **unitinfo)
{
    SwitchBluetooth *self = gDriver;
    IOLockLock(self->stateLock);
    uint64_t deadline;
    clock_interval_to_deadline(kConnectWaitMs, kMillisecondScale, &deadline);
    while (self->state <= kBusy &&
           IOLockSleepDeadline(self->stateLock, &self->state, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {}
    errno_t error = 0;
    // The first connect (see the header) takes the staged address over; the controller, brought up at boot before
    // Data was mounted, still has the fuses' one. Bring-up writes `address`; a controller that needs none gets it
    // with one command.
    if (self->state > kBusy && !self->client && !self->stagedChecked) {
        self->stagedChecked = true;
        IOLockUnlock(self->stateLock);
        (void)self->adoptStagedAddress();
        IOLockLock(self->stateLock);
    }
    const bool readdress =
        self->haveAddress && (!self->chipAddressSet || memcmp(self->chipAddress, self->address, sizeof(self->address)));
    if (readdress && self->state == kReady && !self->client && !self->chipDirty &&
        !__atomic_load_n(&self->rxFault, __ATOMIC_RELAXED)) {
        self->state = kBusy;
        IOLockUnlock(self->stateLock);
        self->setMode(kKernel);
        const uint8_t le[6] = {self->address[5], self->address[4], self->address[3],
                               self->address[2], self->address[1], self->address[0]};
        const int written = self->command(OP_BCM_WRITE_BDADDR, le, sizeof(le), kCommandMs);
        self->setMode(kIdle);
        if (written) IOLog("SwitchBluetooth: Write BD_ADDR for the staged address failed (error %d)\n", written);
        if (!written) {
            memcpy(self->chipAddress, self->address, sizeof(self->chipAddress));
            self->chipAddressSet = true;
        }
        IOLockLock(self->stateLock);
        self->state = kReady;
        self->chipDirty |= written != 0;  // the bring-up below writes it
        IOLockWakeup(self->stateLock, &self->state, false);
    }
    if (self->state <= kBusy)
        error = ETIMEDOUT;
    else if (self->client)
        error = EBUSY;
    else if (self->state == kFailed || self->chipDirty || __atomic_load_n(&self->rxFault, __ATOMIC_RELAXED)) {
        self->state = kBusy;
        IOLockUnlock(self->stateLock);
        char why[128];
        const bool ok = self->bringUp(kPowerOffMs, why, sizeof(why));
        static bool said;  // bluetoothd retries every few seconds: one line is enough
        if (!ok && !said) {
            said = true;
            char line[400];
            self->describe(line, sizeof(line));
            IOLog("SwitchBluetooth: bring-up for a client failed: %s [%s]\n", why, line);
        }
        IOLockLock(self->stateLock);
        self->state = ok ? kReady : kFailed;
        self->chipDirty = false;
        IOLockWakeup(self->stateLock, &self->state, false);
        if (!ok) error = ECONNREFUSED;
    }
    if (!error) {
        self->client = sac->sc_unit;
        self->clientUsed = false;
    }
    IOLockUnlock(self->stateLock);
    if (error) return error;
    self->watch("connect");
    self->flushRx();
    self->setMode(kClient);
    self->unwatch();
    *unitinfo = self;
    char line[400];
    self->describe(line, sizeof(line));
    IOLog("SwitchBluetooth: client %u connected: %s\n", sac->sc_unit, line);
    if (self->haveAddress) switch_joycon_bluetooth_address(self->address);
    return 0;
}

errno_t SwitchBluetooth::ctlDisconnect(kern_ctl_ref, u_int32_t unit, void *)
{
    SwitchBluetooth *self = gDriver;
    IOLockLock(self->stateLock);
    const bool mine = self->client == unit;
    if (mine) {
        self->client = 0;
        self->chipDirty |= self->clientUsed;
        // A core the client used goes off now (under stateLock, so a connect's bring-up cannot be cut short): the
        // next connect brings it up again anyway, and until then no one would take what it keeps sending (r6k:
        // bluetoothd exited mid-connection to AirPods with an LE scan and inquiry running).
        if (self->chipDirty) self->power(false);
    }
    IOLockUnlock(self->stateLock);
    if (mine) {
        self->setMode(kIdle);
        char line[400];
        self->describe(line, sizeof(line));
        IOLog("SwitchBluetooth: client %u gone%s: %s\n", unit, self->rxStalled ? " (RX was stalled)" : "", line);
    }
    return 0;
}

// The client's bytes, in order. A command that starts at a packet boundary and is whole in this send is taken out and
// looked at (clientCommand: answered here, or sent, possibly changed); the rest go out as they are, noted
// (noteClientTx) before the controller can see, let alone answer, a command.
errno_t SwitchBluetooth::ctlSend(kern_ctl_ref, u_int32_t unit, void *, mbuf_t m, int)
{
    SwitchBluetooth *self = gDriver;
    errno_t error = self->client == unit ? 0 : ENOTCONN;
    if (!error) self->clientUsed = true;
    size_t total = 0;
    for (mbuf_t n = m; n; n = mbuf_next(n)) total += mbuf_len(n);
    mbuf_t n = m;
    size_t base = 0, off = 0;  // n starts at byte `base` of the send; the first `off` bytes are done
    while (!error && off < total) {
        while (base + mbuf_len(n) <= off) {
            base += mbuf_len(n);
            n = mbuf_next(n);
        }
        const uint8_t *bytes = static_cast<const uint8_t *>(mbuf_data(n)) + (off - base);
        if (!self->txFrame.state && !self->framingLost && bytes[0] == H4_COMMAND && total - off >= 4) {
            uint8_t packet[4 + 255];
            if (!mbuf_copydata(m, off, 4, packet) && total - off >= 4u + packet[3] &&
                !mbuf_copydata(m, off, 4u + packet[3], packet)) {
                const size_t size = 4u + packet[3];
                error = self->clientCommand(packet, size);
                off += size;
                continue;
            }
        }
        const size_t taken = self->noteClientTx(bytes, base + mbuf_len(n) - off);
        error = self->transmit(bytes, taken, kWriteMs);
        off += taken;
    }
    mbuf_freem(m);
    return error;
}

// struct termios in, as a tty: the output speed becomes the UART rate once queued bytes are out. Hardware flow
// control stays on whatever c_cflag says (H4 has no other). TIOCSETAW/F behave like TIOCSETA.
errno_t SwitchBluetooth::ctlSetopt(kern_ctl_ref, u_int32_t unit, void *, int opt, void *data, size_t len)
{
    SwitchBluetooth *self = gDriver;
    const uint32_t name = (uint32_t)opt;
    if (name != TIOCSETA && name != TIOCSETAW && name != TIOCSETAF) return ENOPROTOOPT;
    if (self->client != unit) return ENOTCONN;
    if (!data || len < sizeof(struct termios)) return EINVAL;
    struct termios t;
    memcpy(&t, data, sizeof(t));
    const uint32_t rate = (uint32_t)(t.c_ospeed ? t.c_ospeed : t.c_ispeed);
    if (rate && rate != self->baud) {
        if (rate != 3000000 && rate != 1000000 && (rate < 9600 || rate > 921600)) return EINVAL;
        if (self->drainTx(kWriteMs)) return EIO;
        self->setHostBaud(rate);
        self->clientUsed = true;
    }
    self->tio = t;
    self->tio.c_ispeed = self->tio.c_ospeed = self->baud;
    return 0;
}

errno_t SwitchBluetooth::ctlGetopt(kern_ctl_ref, u_int32_t, void *, int opt, void *data, size_t *len)
{
    SwitchBluetooth *self = gDriver;
    if ((uint32_t)opt != TIOCGETA) return ENOPROTOOPT;
    if (!data) {
        *len = sizeof(struct termios);
        return 0;
    }
    if (*len < sizeof(struct termios)) return EINVAL;
    self->tio.c_ispeed = self->tio.c_ospeed = self->baud;
    memcpy(data, &self->tio, sizeof(struct termios));
    *len = sizeof(struct termios);
    return 0;
}

// The client read: room again in its socket for what deliver() had to leave in the ring.
void SwitchBluetooth::ctlRcvd(kern_ctl_ref, u_int32_t, void *, int)
{
    SwitchBluetooth *self = gDriver;
    if (__atomic_exchange_n(&self->clientBlocked, false, __ATOMIC_SEQ_CST))
        self->loop->runAction(&SwitchBluetooth::gated, self, reinterpret_cast<void *>(1));
}

// ---- Start -------------------------------------------------------------------------------------------------------

// Kernel thread: the control first (so a client's CTLIOCGINFO finds it and its connect waits), then the first
// bring-up, power cycled and tried again after 1, 2, 4 and 8 s when it fails. nxk retries a Hardware Error so
// (Erista's first bring-up after a cold boot gets one for seconds); a core that does not drive its TX at all after a
// cold power-on (RX held low, maskRxFaultLocked) or stops answering is retried the same way. Only a failure takes
// longer.
void SwitchBluetooth::run()
{
    struct kern_ctl_reg reg = {};
    strlcpy(reg.ctl_name, kControlName, sizeof(reg.ctl_name));
    reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM | CTL_FLAG_REG_EXTENDED;  // bluetoothd runs as mobile: not privileged
    reg.ctl_sendsize = 64 * 1024;
    reg.ctl_recvsize = 64 * 1024;
    reg.ctl_connect = &SwitchBluetooth::ctlConnect;
    reg.ctl_disconnect = &SwitchBluetooth::ctlDisconnect;
    reg.ctl_send = &SwitchBluetooth::ctlSend;
    reg.ctl_setopt = &SwitchBluetooth::ctlSetopt;
    reg.ctl_getopt = &SwitchBluetooth::ctlGetopt;
    reg.ctl_rcvd = &SwitchBluetooth::ctlRcvd;
    const errno_t registered = ctl_register(&reg, &control);
    if (registered) IOLog("SwitchBluetooth: kernel control %s not registered (%d)\n", kControlName, registered);

    if (!patch) IOLog("SwitchBluetooth: no /arm-io/bluetooth ioswitch,patchram; trying the ROM firmware\n");
    char why[128] = "";
    bool ok = false;
    uint32_t offMs = kPowerOffMs;
    unsigned attempt = 0;
    IOLockLock(stateLock);
    state = kBusy;
    IOLockUnlock(stateLock);
    for (;; ++attempt) {
        ok = bringUp(offMs, why, sizeof(why));
        if (ok || attempt == sizeof(kRetrySeconds) / sizeof(kRetrySeconds[0])) break;
        IOSleep(kRetrySeconds[attempt] * 1000);
        offMs = kPowerOffAfterErrorMs;
    }
    if (!ok) {
        char line[400];
        describe(line, sizeof(line));
        IOLog("SwitchBluetooth: bring-up failed (%u attempts): %s [%s]\n", attempt + 1, why, line);
        setProperty("error", why);
    } else if (attempt) {
        IOLog("SwitchBluetooth: up at attempt %u; the one before failed: %s\n", attempt + 1, why);
    }
    IOLockLock(stateLock);
    state = ok ? kReady : kFailed;
    IOLockWakeup(stateLock, &state, false);
    IOLockUnlock(stateLock);
}

namespace {
void startThread(void *, wait_result_t)
{
    SwitchBluetooth *driver = OSTypeAlloc(SwitchBluetooth);
    IOService *resources = IOService::getResourceService();
    if (!driver || !driver->init() || !resources || !driver->attach(resources)) {
        IOLog("SwitchBluetooth: attach failed\n");
        OSSafeReleaseNULL(driver);
        return;
    }
    if (!driver->start(resources)) {
        driver->detach(resources);
        driver->release();
        return;
    }
    gDriver = driver;
    driver->release();  // the attachment keeps it
    gDriver->run();
}
}

extern "C" int switch_bluetooth_start(void)
{
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("switch-bt", &enabled, sizeof(enabled)) || !enabled) return 0;
    static volatile bool started;
    if (__atomic_exchange_n(&started, true, __ATOMIC_ACQ_REL)) return 0;
    publishAddress();
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(&startThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("SwitchBluetooth: thread start failed\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}

// kern_exec.c load_init_program, through SwitchNVRAM's ioswitch_nvram_data_mounted: Data is mounted and launchd not yet
// started. A staged address is published now, so that bluetoothd reads it as local-mac-address when it starts and
// never sees another one (see the header, "BD address"); the controller gets it at bluetoothd's first connect.
extern "C" void switch_bluetooth_data_mounted(void)
{
    uint32_t enabled = 0;
    if (!PE_parse_boot_argn("switch-bt", &enabled, sizeof(enabled)) || !enabled) return;
    uint8_t staged[6];
    if (!readStagedAddress(staged)) {
        __atomic_store_n(&gStagedState, kStagedNone, __ATOMIC_RELEASE);
        return;
    }
    memcpy(gStaged, staged, sizeof(gStaged));
    __atomic_store_n(&gStagedState, kStagedFound, __ATOMIC_RELEASE);
    publishAddressBytes(staged);
    IOLog("SwitchBluetooth: BD %02x:%02x:%02x:%02x:%02x:%02x from the staged Joy-Con pairing (Horizon's), in place "
          "before launchd\n",
          staged[0], staged[1], staged[2], staged[3], staged[4], staged[5]);
}
