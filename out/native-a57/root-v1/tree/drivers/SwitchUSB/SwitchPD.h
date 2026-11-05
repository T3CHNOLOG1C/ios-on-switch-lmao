// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPD: the one owner of the BM92T36 USB-PD controller (I2C1 0x18), its ALERT line (PK4) and the port's power
// role.
//
// The ALERT line is level-low and released by reading ALERT_STATUS, which clears on read (Linux bm92txx.c
// bm92t_probe: IRQF_TRIGGER_LOW). So exactly one driver may take the pin and read that register: this one. On every
// ALERT it reads ALERT_STATUS once, then STATUS1/STATUS2, decides the power role and hands the state to its listeners
// on its own work loop: SwitchBattery (which re-reads the charger and gauge 250 ms later) and the USB role manager
// (SwitchUSB.cpp, which only follows `role` with the data role). It negotiates nothing: a contract, the input current
// limit and charging stay as the controller and the charger manage them.
//
// Power role: sink first (Try.SNK). The Switch takes power from anything that can give it and sources VBUS only to a
// partner proven to be sink-only (an OTG adapter, a USB-C flash drive or keyboard, an unpowered hub). Linux cannot
// tell a dual-role partner (a Mac's USB-C port) from such a sink: SRC_MODE + OTG_INSERT is its whole test, with the
// source switches (CONFIG1 SPDSRC) on all the time, and a dual-role partner that comes up as the sink gets our VBUS.
// Here:
//   - At start, always (host mode packaged or not): the source switches off, whatever an earlier boot left.
//   - Empty port: the source switches on only when host mode is packaged (switch_pd_allow_source), so a sink can
//     attach; VBUS itself is never driven while the port is empty (no boost, no v_vdd5v3, no VCONN).
//   - Anything attaches: the switches go off until it is classified. A partner with VBUS up (a charger, a Mac, a
//     dock) is a sink session: switches off until INSERT drops, so no later event (suspend, sleep, a swap request)
//     can make us the source. A partner that came up as our sink is offered the source role: switches off for
//     kClassifyMS. A dual-role partner then sources (sink session); a sink-only one just goes away (an observed
//     detach), and only then the source rails come up (OCP on, v_vdd5v3 on PX3, VCONN on PK5, 10 ms ramp) and the
//     switches go back on, so the controller closes its switch onto a live 5.3 V rail when the partner attaches
//     again as the sink (bm92txx.c order: rails at probe, SPDSRC12_ON after). We then source: the BQ24193 boost on
//     as Linux does (refused only if the charger sees VBUS that is not ours: STATUS1 shows neither SPDSRC switch on).
//     v_vdd5v3 is the OLED's VBUS power (SwitchPDPolicy.h). No observed detach (the switches did not move the port's
//     role) is no proof: we do not source.
//   - Sourcing ends (detach, a USB-PD contract, a source fault, host mode gone) through one path: boost off, v_vdd5v3
//     and VCONN off, switches to the empty-port state. The rails are on in Reattach and Source only.
// One kernel log line per decision, with the controller's bits. The decisions are SwitchPDPolicy.h (host-tested).
//
// Interrupt only: a pin that cannot be attached panics; a pin that keeps firing (stuck low: more than 50 ALERTs in a
// second) is masked for the boot with one line and the port stays sink-only (no storm, no panic);
// switch_pd_check_delivery() panics when the pin shows a pending interrupt that never reached the CPU. The classify
// windows are one-shot timers on the PD work loop, armed only by a new attach: nothing re-arms on its own.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum SwitchPDRole {
    SWITCH_PD_ROLE_NONE,    // nothing on the port
    SWITCH_PD_ROLE_SINK,    // something is: we take power or none (a charger, a host, a dock, or not yet classified)
    SWITCH_PD_ROLE_SOURCE,  // a sink-only partner and our VBUS on it: USB host
};

struct SwitchPDState {
    uint16_t alert;          // ALERT_STATUS bits of this event (0 for a state not caused by an ALERT)
    uint16_t status1;        // STATUS1
    uint16_t status2;        // STATUS2
    bool plugged;            // STATUS1.INSERT: a cable is in
    bool vbusPresent;        // STATUS1.VSAFE: VBUS at vSafe5V or a contract (ours when role is SOURCE)
    bool dataHost;           // STATUS1 data role DFP
    enum SwitchPDRole role;  // the power role decided (above)
};

// Adds a listener (at most 4; for the life of the boot) and starts the PD service on first use (may sleep: I2C,
// GPIO, GIC). The listener runs on the PD work loop after every ALERT and role change, and once right away with the
// current state; keep it short. Returns 0, or -1 when the controller is not there (the listener is then never called).
int switch_pd_add_listener(void (*listener)(void *context, const struct SwitchPDState *state), void *context);

// Lost-interrupt check for PK4, for a caller's existing slow timer (SwitchBattery's re-read): panics "<who>: GPIO 84
// interrupt lost: ..." when the pin's interrupt is pending at the GPIO controller and stays so for 50 ms. May sleep.
void switch_pd_check_delivery(const char *who);

// Host mode packaged and working (true) or gone for this boot (false: its bring-up failed). Without it the port is
// sink-only. Thread context; 0, or -1 without the controller.
int switch_pd_allow_source(bool allow);

// One line for the role report (SwitchUSB.cpp): the phase, STATUS1/STATUS2/CONFIG1/VENDOR_CONFIG as read now, and the
// GPIOs behind VBUS: v_vdd5v3 (PX3), VCONN (PK5) and the BQ24193's PSEL/OTG inputs (PJ5, PL0; read, never driven).
// Thread context (I2C on the PD work loop); 0, or -1 without the controller (the buffer then says so).
int switch_pd_describe(char *buffer, size_t size);

#ifdef __cplusplus
}
#endif
