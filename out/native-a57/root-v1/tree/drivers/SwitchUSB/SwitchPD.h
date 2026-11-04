// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPD: the one owner of the BM92T36 USB-PD controller (I2C1 0x18) and its ALERT line (PK4).
//
// The ALERT line is level-low and released by reading ALERT_STATUS, which clears on read (Linux bm92txx.c
// bm92t_probe: IRQF_TRIGGER_LOW). So exactly one driver may take the pin and read that register: this one. On every
// ALERT it reads ALERT_STATUS once, then STATUS1/STATUS2, and hands the decoded state to its listeners on its own
// work loop: SwitchBattery (which re-reads the charger and gauge 250 ms later, as it did when it owned the pin) and
// the USB role manager (SwitchUSB.cpp). It sends no PD command and negotiates nothing: the contract, input current
// limit and charging stay exactly as the controller and the charger manage them.
//
// With host mode packaged (the ADT has /arm-io/usb-xhci), switch_pd_enable_source() readies the controller to source
// VBUS to a sink, as Linux bm92t_probe/bm92t_extcon_cable_set_init_state do: the v_vdd5v3 rail (PX3) and VCONN (PK5)
// on, over-current protection on, both source switches enabled (CONFIG1 SPDSRC). Without it the controller is only
// read.
//
// Interrupt only: a pin that cannot be attached panics; a pin that keeps firing (stuck low) panics with its state;
// switch_pd_check_delivery() panics when the pin shows a pending interrupt that never reached the CPU.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct SwitchPDState {
    uint16_t alert;    // ALERT_STATUS bits of this event (0 for the first, synthetic event)
    uint16_t status1;  // STATUS1
    uint16_t status2;  // STATUS2
    bool plugged;      // STATUS1.INSERT: a cable is in
    bool sourcing;     // STATUS1.SRC_MODE: the port is the power source
    bool otgDevice;    // SRC_MODE and STATUS2.OTG_INSERT: a sink (OTG adapter, keyboard, flash drive, unpowered hub)
    bool dataHost;     // STATUS1 data role DFP: we are the USB host on this cable
    bool vbusPresent;  // STATUS1.VSAFE: VBUS at vSafe5V or a contract
};

// Adds a listener (at most 4; for the life of the boot) and starts the PD service on first use (may sleep: I2C,
// GPIO, GIC). The listener runs on the PD work loop after every ALERT, and once right away with the current state;
// keep it short. Returns 0, or -1 when the controller is not there (the listener is then never called).
int switch_pd_add_listener(void (*listener)(void *context, const struct SwitchPDState *state), void *context);

// Lost-interrupt check for PK4, for a caller's existing slow timer (SwitchBattery's re-read): panics "<who>: GPIO 84
// interrupt lost: ..." when the pin's interrupt is pending at the GPIO controller and stays so for 50 ms. May sleep.
void switch_pd_check_delivery(const char *who);

// Ready the controller to source VBUS (see above). Idempotent; 0 on success. Each change it makes is logged once.
int switch_pd_enable_source(void);

#ifdef __cplusplus
}
#endif
