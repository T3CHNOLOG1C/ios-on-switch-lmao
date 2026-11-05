// SPDX-License-Identifier: BSD-2-Clause-Patent
// The host role's register arithmetic, in plain C with no kernel dependency, so the host tests
// (tests/test_switch_usb_host.py) check this same header: which xHCI port is the USB-C port's USB 2.0 pad, how to
// write a PORTSC without touching its write-1-to-clear bits, and the XUSB pad controller values for each role.
//
// Sources: xHCI 1.2 (5.3.6 HCCPARAMS1.xECP, 7.2 Supported Protocol capability, 5.4.8 PORTSC), Linux
// drivers/usb/host/xhci.h (XHCI_PORT_RO, XHCI_PORT_RWS: xhci_port_state_to_neutral), CTCaer switch-l4t-kernel-4.9
// drivers/phy/tegra/xusb-tegra210.c (USB2_VBUS_ID id/vbus overrides, BATTERY_CHRG_OTGPAD charger detect on/off,
// USB2_OC_MAP) and drivers/usb/host/xhci-tegra.c tegra_xhci_set_host_mode (T210/T210B01 otg_set_port_power: after the
// ID override, the OTG port's PORTSC.PP is set, so the firmware sees the port powered with the pad in the host role).
#ifndef XUSB_PORTS_H
#define XUSB_PORTS_H

#include <stdbool.h>
#include <stdint.h>

// --- xHCI ---------------------------------------------------------------------------------------------------------
#define XHCI_HCCPARAMS1          0x10u
#define XHCI_XECP_SUPPORTED_PROT 2u
#define XHCI_PORTSC_BASE         0x400u  // from the operational registers, 0x10 per port, port 1 first
#define XHCI_PORTSC_CCS          (1u << 0)
#define XHCI_PORTSC_PED          (1u << 1)
#define XHCI_PORTSC_OCA          (1u << 3)
#define XHCI_PORTSC_PR           (1u << 4)
#define XHCI_PORTSC_PLS_SHIFT    5
#define XHCI_PORTSC_PP           (1u << 9)
#define XHCI_PORTSC_SPEED_SHIFT  10
#define XHCI_PORTSC_CSC          (1u << 17)
// Linux XHCI_PORT_RO (CCS, OCA, speed, device removable) and XHCI_PORT_RWS (link state, power, indicator, wake
// enables): written back as read. Everything else is a write-1 action (PED disables the port, PR resets it, LWS, WPR,
// and the seven change bits clear): written 0.
#define XHCI_PORTSC_KEEP \
    ((1u << 0) | (1u << 3) | (0xfu << 10) | (1u << 30) | (0xfu << 5) | (1u << 9) | (3u << 14) | (7u << 25))

static inline uint32_t xhci_portsc_offset(uint32_t cap_length, unsigned port)
{
    return cap_length + XHCI_PORTSC_BASE + 0x10u * (port - 1u);
}

// The value to write for PP on or off, nothing else changed.
static inline uint32_t xhci_portsc_power(uint32_t portsc, bool on)
{
    const uint32_t neutral = portsc & XHCI_PORTSC_KEEP;
    return on ? (neutral | XHCI_PORTSC_PP) : (neutral & ~XHCI_PORTSC_PP);
}

struct xhci_port_range {
    unsigned first;  // 1-based port number; 0: none found
    unsigned count;
};

// The ports of protocol `major` (2: USB 2.0, 3: USB 3.x) from the Supported Protocol capabilities in the extended
// capability list. `read` reads a 32-bit register at a byte offset from the capability base; `window` bounds the
// offsets (the register window's size). The first capability of that major wins (Tegra XUSB: one per protocol).
// A malformed list (a loop, an offset outside the window) ends the walk: at most 64 capabilities are read.
static inline struct xhci_port_range xhci_find_ports(uint32_t (*read)(void *context, uint32_t offset), void *context,
                                                     uint32_t window, unsigned major)
{
    struct xhci_port_range range = {0, 0};
    uint32_t offset = (read(context, XHCI_HCCPARAMS1) >> 16) << 2;
    for (unsigned i = 0; i < 64 && offset && offset + 12 <= window; ++i) {
        const uint32_t head = read(context, offset);
        if ((head & 0xffu) == XHCI_XECP_SUPPORTED_PROT && (head >> 24) == major) {
            const uint32_t ports = read(context, offset + 8);
            range.first = ports & 0xffu;
            range.count = (ports >> 8) & 0xffu;
            return range;
        }
        const uint32_t next = ((head >> 8) & 0xffu) << 2;
        if (!next) break;
        offset += next;
    }
    return range;
}

// --- XUSB pad controller, USB2 port 0 (the USB-C port's D+/D-) ------------------------------------------------------
#define XUSB_USB2_PORT_CAP          0x008u
#define XUSB_USB2_OC_MAP            0x010u
#define XUSB_VBUS_OC_MAP            0x018u
#define XUSB_OC_DET                 0x01cu
#define XUSB_CHRG_OTGPAD0_CTL0      0x080u
#define XUSB_CHRG_OTGPAD0_CTL1      0x084u
#define XUSB_USB2_VBUS_ID           0xc60u
#define XUSB_PORT0_CAP_MASK         3u
#define XUSB_PORT0_CAP_DEVICE       2u
#define XUSB_PORT0_CAP_OTG          3u
#define XUSB_VBUS_ID_VBUS_SRC_OVR   (1u << 12)  // VBUS source: the override
#define XUSB_VBUS_ID_VBUS_ON        (1u << 14)  // the VBUS override's value
#define XUSB_VBUS_ID_ID_SRC_OVR     (1u << 16)  // ID source: the override
#define XUSB_VBUS_ID_ID_MASK        (0xfu << 18)
#define XUSB_VBUS_ID_ID_GROUNDED    (0u << 18)  // a host's ID: the XHCI owns the pad
#define XUSB_VBUS_ID_ID_FLOATING    (8u << 18)  // a device's ID: the XUDC owns it
#define XUSB_CHRG_CTL0_PD_CHG       (1u << 0)
#define XUSB_CHRG_CTL0_SRC_SINK     ((1u << 9) | (1u << 10) | (1u << 11) | (1u << 12))   // OP/ON source and sink
#define XUSB_CHRG_CTL1_RPD_RPU_OVRD ((1u << 16) | (1u << 18) | (1u << 20) | (1u << 22))  // D+/D- pull overrides
#define XUSB_OC_MAP_PORT0_MASK      0xfu
#define XUSB_OC_MAP_PORT0_DISABLED  0xfu

// Host: ID and VBUS from their overrides, VBUS override low, ID grounded (tegra210_xusb_padctl_id_override set; Linux
// drops a set VBUS override first and waits 1-2 ms, which the caller does with xusb_vbus_id_drop_vbus).
static inline uint32_t xusb_vbus_id_drop_vbus(uint32_t v)
{
    return (v | XUSB_VBUS_ID_VBUS_SRC_OVR | XUSB_VBUS_ID_ID_SRC_OVR) & ~XUSB_VBUS_ID_VBUS_ON;
}

static inline uint32_t xusb_vbus_id_host(uint32_t v)
{
    return (xusb_vbus_id_drop_vbus(v) & ~XUSB_VBUS_ID_ID_MASK) | XUSB_VBUS_ID_ID_GROUNDED;
}

// Device: ID floating (id_override clear); the VBUS override is the XUDC's to set when it connects.
static inline uint32_t xusb_vbus_id_device(uint32_t v)
{
    return ((v | XUSB_VBUS_ID_VBUS_SRC_OVR | XUSB_VBUS_ID_ID_SRC_OVR) & ~XUSB_VBUS_ID_ID_MASK) |
           XUSB_VBUS_ID_ID_FLOATING;
}

// The charger-detection leftovers a host's D+/D- must not have (a boot stage's BC1.2 detection): the pull-up/down
// overrides off (utmi_pad_charger_detect_off) so the pad's own 15 kOhm host pull-downs apply, and no D+/D- source or
// sink. PD_CHG is left as found (hekate and the XUDC clear it; the sources it powers are off).
static inline uint32_t xusb_chrg_ctl0_host(uint32_t v)
{
    return v & ~XUSB_CHRG_CTL0_SRC_SINK;
}

static inline uint32_t xusb_chrg_ctl1_host(uint32_t v)
{
    return v & ~XUSB_CHRG_CTL1_RPD_RPU_OVRD;
}

// Port 0's over-current pin: none on this board (no nvidia,oc-pin), so detection off; a mapped pin that reads
// asserted would hold PORTSC.OCA and the port unpowered.
static inline uint32_t xusb_oc_map_host(uint32_t v)
{
    return (v & ~XUSB_OC_MAP_PORT0_MASK) | XUSB_OC_MAP_PORT0_DISABLED;
}

static inline const char *xusb_id_name(uint32_t vbus_id)
{
    switch ((vbus_id & XUSB_VBUS_ID_ID_MASK) >> 18) {
    case 0: return "grounded";
    case 1: return "C";
    case 2: return "B";
    case 4: return "A";
    case 8: return "floating";
    default: return "?";
    }
}

#endif
