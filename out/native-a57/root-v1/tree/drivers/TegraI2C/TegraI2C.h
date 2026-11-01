// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 (T210/T210B01) I2C master for statically linked IOSwitch drivers.
//
// Buses use Tegra numbering, 1..5 (I2C1 0x7000C000 .. I2C5/PWR_I2C 0x7000D000):
//   I2C1 100 kHz  MAX17050 fuel gauge 0x36, BQ24193 charger 0x6B, BM92T36 USB-PD 0x18
//   I2C2 400 kHz  (ALS etc.)
//   I2C3 400 kHz  FTM4 touch 0x49
//   I2C4 100 kHz  CAM_I2C pads
//   I2C5 400 kHz  MAX77620 PMIC 0x3C (shared, see below)
// I2C6 (DPAUX1 pads) is not supported.
//
// What the boot chain leaves behind (hekate bdk/soc/hw_init.c, l4t.c; switch U-Boot):
//   * hekate hw_init enables + pinmuxes I2C1 and I2C5 (CLK_M source) and initializes both;
//     hw_deinit and the L4T launcher do not undo that. Touch power-off disables I2C3.
//   * switchroot U-Boot only probes I2C5 (board/nintendo/switch: MAX77620), re-clocking it
//     from PLLP at 400 kHz and leaving it in packet mode.
// tegra_i2c_init() therefore owns I2C1..I2C4 outright: CAR reset/clock-enable with CLK_M
// (19.2 MHz) as source, pinmux (function I2C, input enabled), controller config and a bus
// clear, mirroring hekate clock_enable()/pinmux_config_i2c()/i2c_init(). I2C5 is adopted
// as-is when it is already clocked: it is never reset or re-clocked because its clock and
// config are firmware-owned and CL-DVFS shares the PWR_I2C pads (hekate hw_init.c enables
// CL-DVFS "to avoid issues with I2C5 sharing"); runtime use by the T210B01 BPMP firmware is
// unverified. A PMIC driver on I2C5 must keep transfers short. With switch-cpu-mode=dfll the
// DFLL's own I2C master writes VDD_CPU on these pads, so I2C5 runs in MULTI_MASTER_MODE (as L4T
// runs every T210 controller): a transfer that loses arbitration is repeated (up to 3 times)
// after a FIFO flush, never answered with a reset or bus clear, and TEGRA_I2C_EARB reaches the
// caller only when every attempt lost. (On Hoag only, MAX77620 LDO8 also powers the I2C1 pads.) The touch panel
// needs its own supplies/reset GPIOs before it answers on I2C3.
//
// Transfers are serialized per bus with an IOLock; the API is synchronous and every completion comes by interrupt.
// tegra_i2c_init() claims the bus's SPI, I2C1 38, I2C2 84, I2C3 92, I2C4 120, I2C5 53 (Linux tegra210.dtsi:676/691/
// 706/721/739; the OLED DTB i2c@7000c000.. nodes), sleeping in ioswitch_interrupt_nub() until the GIC is up; a caller
// sleeps until the work loop action reports packet completion, NACK or arbitration loss (FIFO refills and drains
// happen there too; the bus clear completes by INT_BUS_CLEAR_DONE). There is no polled mode: an interrupt that
// cannot be attached, or a wait that times out while the controller asserts an unmasked status bit, panics with the
// controller and GIC state. I2C5 is no exception: switchroot's L4T kernel drives i2c@7000d000 by interrupt like the
// other buses (OLED DTB node: interrupts SPI 53, status okay; Linux i2c-tegra.c probe requests every non-DVC bus's
// IRQ), and nothing else uses that register block at runtime (CL-DVFS is its own initiator on the PWR_I2C pads; the
// PMC bootrom-commands on controller 4 run only in the BootROM on reset). Call only from thread context, never with
// a spinlock held or from an interrupt/filter handler. After a failed transfer an owned bus is reset and
// reinitialized (as Linux i2c-tegra does).
#ifndef TEGRA_I2C_H
#define TEGRA_I2C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEGRA_I2C_OK = 0,
    TEGRA_I2C_EINVAL = -1,     // bad bus/address/length
    TEGRA_I2C_ENOMEM = -2,     // mapping or lock allocation failed
    TEGRA_I2C_ENOTREADY = -3,  // tegra_i2c_init() not called (or failed) for this bus
    TEGRA_I2C_ENACK = -4,      // device did not acknowledge
    TEGRA_I2C_EARB = -5,       // arbitration lost / bus stuck
    TEGRA_I2C_ETIMEDOUT = -6,  // controller did not finish in time
    TEGRA_I2C_EIO = -7,        // packet completed short
};

// Longest single read or write payload, and longest register write (tegra_i2c_write).
#define TEGRA_I2C_MAX_TRANSFER  4096
#define TEGRA_I2C_MAX_REG_WRITE 63

// Maps the controller, CAR and pinmux (once), claims the bus's interrupt and brings the bus up.
// Idempotent and thread-safe; later calls for an initialized bus return TEGRA_I2C_OK immediately.
// May sleep until the GIC is up (panics after 10 s, in ioswitch_interrupt_nub).
int tegra_i2c_init(unsigned bus);

// Write tx (if txlen) then, after a repeated START, read rx (if rxlen). 7-bit address.
int tegra_i2c_transfer(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx, size_t rxlen);

// Register read: write reg, repeated START, read len bytes.
int tegra_i2c_read(unsigned bus, uint8_t addr, uint8_t reg, void *buf, size_t len);

// Register write: reg followed by len (<= TEGRA_I2C_MAX_REG_WRITE) bytes, then STOP.
int tegra_i2c_write(unsigned bus, uint8_t addr, uint8_t reg, const void *buf, size_t len);

// System sleep (drivers/SwitchSuspend), thread context, both from the same thread: suspend holds every bus (later
// transfers wait) until resume has re-clocked and reconfigured the controllers after SC7. Panics if a bus does not
// come back.
void tegra_i2c_suspend(void);
void tegra_i2c_resume(void);

#ifdef __cplusplus
}
#endif
#endif
