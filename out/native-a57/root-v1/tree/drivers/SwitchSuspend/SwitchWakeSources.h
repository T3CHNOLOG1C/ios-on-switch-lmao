// SPDX-License-Identifier: BSD-2-Clause-Patent
// What wakes the Switch OLED, as plain C on byte buffers and integers so that tests/test_switch_wake.py drives it on the
// host: the wake sources, the Bluetooth controllers' Home button (an HID input report in an inbound HCI ACL packet), the
// wake guarantee's state machine, the power journal's ring, and the PMC wake events system sleep arms (SwitchSuspend).
// The kernel half is SwitchWake.cpp (SwitchWake.h).
//
// Sleep, as this port has it (r15):
//   * Display power (the lock): backboardd's render server asks for the panel off (lock button, auto-lock) and on
//     (SwitchBacklight). The system stays up: no IOPMrootDomain sleep (ADT /defaults sleep-disabled, SwitchSuspend is
//     inert unless switch-sleep=2), so every interrupt still reaches the CPU and nothing needs to be armed. A wake is
//     SpringBoard turning the panel on again because of an input event: the power key (Consumer Power), or the Home
//     button (Consumer Menu, J120's own Home button: one press wakes and goes home).
//   * The wake sources with the panel off, each of which ends in one of those two events:
//       power key        MAX77620 EN0 -> nIRQ -> INTID 118 (SwitchButtons) -> Consumer Power, as always;
//       rail Home        a seated Joy-Con (R)'s Home -> Consumer Menu on "Joy-Con System Buttons" (SwitchJoyConHID). The
//                        rails keep running with the panel off (r15: they used to pause, so a seated Joy-Con could not
//                        wake the console);
//       Bluetooth Home   a connected Joy-Con (R) or Pro Controller's Home, seen here in its HID input report on the way
//                        to bluetoothd (SwitchBluetooth) -> Consumer Menu on the console buttons (SwitchButtons). With
//                        the panel lit the report only goes to GameController, as before (its AC Home does nothing).
//       power in         a charger, dock or host plugged in (SwitchBattery: external power that is not our own boost
//                        appears) -> SpringBoard lights the screen with the charging UI itself (-[SBUIController
//                        updateBatteryState:] -> unlockUIFromSource:withOptions:, from the power-source change), as on
//                        an iPad; the wake guarantee covers it if it does not.
//   * The wake guarantee: any of them with the panel off arms a deadline (kSwitchWakeGuardMS). If the render server has
//     not asked for the panel on by then, the kernel lights it itself (one journal line, and a stackshot of every task
//     in /private/var/ioswitch/wake-<UTC>.kcdata), so a hung SpringBoard or backboardd shows as a frozen screen, not as
//     a Switch that will not wake, and the stackshot says where they are stuck. Holding POWER 6 s is the PMIC's own
//     power-off (hekate sets ONOFFCNFG1 MRT), whatever the kernel does.
//   * System sleep (SwitchSuspend, switch-sleep=2, SC7) arms the PMC wake events in switch_wake_pmc_events.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum switch_wake_source {
    SWITCH_WAKE_POWER_KEY = 0,  // MAX77620 EN0 (SwitchButtons)
    SWITCH_WAKE_RAIL_HOME = 1,  // a seated Joy-Con (R)'s Home (SwitchJoyConHID)
    SWITCH_WAKE_BT_HOME = 2,    // a Bluetooth Joy-Con (R) or Pro Controller's Home (SwitchBluetooth)
    SWITCH_WAKE_POWER_IN = 3,   // external power plugged in (SwitchBattery)
    SWITCH_WAKE_SOURCES = 4,
};

static inline const char *switch_wake_source_name(unsigned source)
{
    static const char *const names[SWITCH_WAKE_SOURCES] = {"power key", "rail Joy-Con Home", "Bluetooth Home",
                                                           "power in"};
    return source < SWITCH_WAKE_SOURCES ? names[source] : "unknown";
}

// --- Bluetooth Home ------------------------------------------------------------------------------------------------
// An inbound HCI ACL packet from its 4-byte header (handle and flags, length; no H4 type byte), `length` bytes of it
// (it may be cut short: only the first SWITCH_WAKE_BT_PEEK bytes are looked at). A Nintendo controller's input report
// rides one unfragmented L2CAP frame on the HID interrupt channel (a dynamic CID) behind the HIDP header 0xA1 (DATA |
// INPUT): report 0x30 (full, every 15 ms), 0x21 (subcommand reply) and 0x31 (NFC/IR) carry the shared button byte at
// report offset 4 (Minus 0x01, Plus 0x02, sticks, Home 0x10, Capture 0x20), 0x3F (simple HID, on button changes) its
// second button byte at offset 2 with the same bits (dekuNukem Nintendo_Switch_Reverse_Engineering
// bluetooth_hid_notes.md; the 0x3F layout is the Joy-Con's own SDP descriptor, 16 buttons then the hat). Returns 1
// (Home down in this report), 0 (a report without Home) or -1 (not a Nintendo controller input report); *handle is set
// for 0 and 1. A Joy-Con (L) has Capture there instead and never reports Home.
#define SWITCH_WAKE_BT_PEEK 16u

static inline int switch_wake_bt_home(const uint8_t *acl, size_t length, uint16_t *handle)
{
    if (!acl || length < 4) return -1;
    const unsigned flags = (unsigned)(acl[1] >> 4) & 3u;  // packet boundary: 1 = continuation
    const unsigned aclLength = (unsigned)(acl[2] | acl[3] << 8);
    if (flags == 1u || length < 4 + 4 + 2 || aclLength < 4 + 2) return -1;
    const unsigned l2capLength = (unsigned)(acl[4] | acl[5] << 8), cid = (unsigned)(acl[6] | acl[7] << 8);
    if (cid < 0x40u || l2capLength + 4u != aclLength) return -1;  // signalling/fixed channels, or a fragmented frame
    const uint8_t *hid = acl + 8;
    const size_t have = length - 8 < l2capLength ? length - 8 : l2capLength;
    if (hid[0] != 0xa1) return -1;
    const uint8_t report = hid[1];
    size_t at;
    if (report == 0x30 || report == 0x21 || report == 0x31) {
        if (l2capLength < 1 + 13) return -1;  // HIDP header + the standard input's 13 bytes at least
        at = 1 + 4;
    } else if (report == 0x3f) {
        if (l2capLength != 1 + 12) return -1;  // HIDP header + 0x3F's 12 bytes
        at = 1 + 2;
    } else {
        return -1;
    }
    if (have <= at) return -1;
    *handle = (uint16_t)((unsigned)(acl[0] | acl[1] << 8) & 0x0fffu);
    return (hid[at] & 0x10) ? 1 : 0;
}

// Home edges per connection: true when this report has Home down and the connection's last one did not. A table full
// of other handles takes the oldest entry's slot.
#define SWITCH_WAKE_BT_LINKS 8u
struct switch_wake_bt_edges {
    uint16_t handle[SWITCH_WAKE_BT_LINKS];
    uint8_t home[SWITCH_WAKE_BT_LINKS];
    uint8_t used[SWITCH_WAKE_BT_LINKS];
    uint32_t age[SWITCH_WAKE_BT_LINKS];
    uint32_t clock;
};

static inline bool switch_wake_bt_edge(struct switch_wake_bt_edges *e, uint16_t handle, bool home)
{
    unsigned slot = SWITCH_WAKE_BT_LINKS;
    for (unsigned i = 0; i < SWITCH_WAKE_BT_LINKS && slot == SWITCH_WAKE_BT_LINKS; ++i)
        if (e->used[i] && e->handle[i] == handle) slot = i;
    if (slot == SWITCH_WAKE_BT_LINKS) {
        for (unsigned i = 0; i < SWITCH_WAKE_BT_LINKS && slot == SWITCH_WAKE_BT_LINKS; ++i)
            if (!e->used[i]) slot = i;
        if (slot == SWITCH_WAKE_BT_LINKS) {
            slot = 0;
            for (unsigned i = 1; i < SWITCH_WAKE_BT_LINKS; ++i)
                if (e->age[i] < e->age[slot]) slot = i;
        }
        e->used[slot] = 1;
        e->handle[slot] = handle;
        e->home[slot] = 0;
    }
    e->age[slot] = ++e->clock;
    const bool edge = home && !e->home[slot];
    e->home[slot] = home ? 1 : 0;
    return edge;
}

// --- The wake guarantee --------------------------------------------------------------------------------------------
// Armed by a wake source while the panel is off; disarmed by a display-on request; fires when its deadline passes with
// the panel still off and no display-on asked for. Times in any monotonic unit.
#define SWITCH_WAKE_GUARD_MS 2500u
struct switch_wake_guard {
    uint64_t deadline;
    uint8_t source;
    bool armed;
};

// A wake source with the panel off (panelOn false): arms, keeping an earlier deadline. True when newly armed.
static inline bool switch_wake_guard_arm(struct switch_wake_guard *g, unsigned source, uint64_t now, uint64_t timeout,
                                         bool panelOn)
{
    if (panelOn || g->armed) return false;
    g->armed = true;
    g->source = (uint8_t)source;
    g->deadline = now + timeout;
    return true;
}

// A display-on request (the render server's, or the guard's own).
static inline void switch_wake_guard_display_on(struct switch_wake_guard *g)
{
    g->armed = false;
}

// At or after the deadline: 1 fire (the guard disarms; light the panel), 0 nothing to do (disarmed: the panel came on,
// or on was asked for), -1 not yet (still armed: look again at the deadline).
static inline int switch_wake_guard_check(struct switch_wake_guard *g, uint64_t now, bool panelOn, bool onRequested)
{
    if (!g->armed) return 0;
    if (panelOn || onRequested) {
        g->armed = false;
        return 0;
    }
    if (now < g->deadline) return -1;
    g->armed = false;
    return 1;
}

// --- The power journal's ring --------------------------------------------------------------------------------------
// The last SWITCH_POWER_LINES lines (each cut at SWITCH_POWER_LINE - 1 bytes), oldest first when rendered.
#define SWITCH_POWER_LINES 64u
#define SWITCH_POWER_LINE  192u
struct switch_power_ring {
    char line[SWITCH_POWER_LINES][SWITCH_POWER_LINE];
    uint32_t next;   // the slot the next line goes to
    uint32_t total;  // lines ever appended
};

static inline void switch_power_ring_append(struct switch_power_ring *r, const char *text)
{
    char *slot = r->line[r->next % SWITCH_POWER_LINES];
    size_t i = 0;
    for (; text[i] && text[i] != '\n' && i < SWITCH_POWER_LINE - 1; ++i) slot[i] = text[i];
    slot[i] = 0;
    r->next = (r->next + 1) % SWITCH_POWER_LINES;
    ++r->total;
}

// Every kept line, oldest first, one per '\n', into out (NUL-terminated, cut at capacity). Returns the length.
static inline size_t switch_power_ring_render(const struct switch_power_ring *r, char *out, size_t capacity)
{
    if (!capacity) return 0;
    size_t used = 0;
    const uint32_t kept = r->total < SWITCH_POWER_LINES ? r->total : SWITCH_POWER_LINES;
    const uint32_t first = (r->next + SWITCH_POWER_LINES - kept) % SWITCH_POWER_LINES;
    for (uint32_t n = 0; n < kept; ++n) {
        const char *line = r->line[(first + n) % SWITCH_POWER_LINES];
        for (size_t i = 0; line[i] && used + 1 < capacity; ++i) out[used++] = line[i];
        if (used + 1 < capacity) out[used++] = '\n';
    }
    out[used] = 0;
    return used;
}

// --- PMC wake events (system sleep, SwitchSuspend) -----------------------------------------------------------------
// The OLED DTB's nvidia,pmc-wakeup events (switchroot tegra210b01-nx-common.dtsi / -nx-pmic.dtsi). PMC_WAKE_* hold
// events 0-31, PMC_WAKE2_* events 32-63; a LVL bit set is active high. armed: what SwitchSuspend arms for SC7. Wi-Fi
// wake (8) and Bluetooth host wake (52) are commented out in that DTB, as the controllers' host-wake features are not
// set up there either; they stay listed (not armed) so a wake status naming them is decoded.
enum { SWITCH_WAKE_LOW = 0, SWITCH_WAKE_HIGH = 1 };
struct switch_wake_pmc_event {
    uint8_t event, level, armed;
    const char *name;
};
static const struct switch_wake_pmc_event switch_wake_pmc_events[] = {
    {51, SWITCH_WAKE_LOW, 1, "pwrbtn"},   // pwr_int_n: MAX77620 nIRQ: power key EN0 (and RTC ALARM1: switch_rtc_resume)
    {14, SWITCH_WAKE_LOW, 1, "usb-c"},    // gpio_pk4: BM92T36 USB-PD ALERT (armed only while the line is high)
    {27, SWITCH_WAKE_LOW, 1, "battery"},  // gpio_py0: MAX17050 ALRT, the battery's charge-zone edges only during sleep
                                          // (switch_battery_suspend; the DTB's battery-gauge@36 nvidia,pmc-wakeup 27)
                                          // (armed only while the line is high)
    {16, SWITCH_WAKE_HIGH, 0, "soc-rtc"},  // rtc_irq: the PMC's own RTC (the MAX77620's is used)
    {35, SWITCH_WAKE_LOW, 0, "sd-cd"},     // gpio_pz1: SD card detect (both edges in the DTB)
    {8, SWITCH_WAKE_HIGH, 0, "wifi"},      // WiFi Wake Ap (PH2), commented out
    {52, SWITCH_WAKE_LOW, 0, "bt"},        // bt_wake_ap (PH5), commented out
};
#define SWITCH_WAKE_PMC_EVENTS (sizeof(switch_wake_pmc_events) / sizeof(switch_wake_pmc_events[0]))

// The registers for a set of events (bit n of `events` = event n): mask[0]/mask[1] = PMC_WAKE_MASK/PMC_WAKE2_MASK,
// highSet/lowClear[0..1] = LVL bits to set (active high) and to clear (active low).
static inline void switch_wake_pmc_registers(uint64_t events, uint32_t mask[2], uint32_t highSet[2],
                                             uint32_t lowClear[2])
{
    mask[0] = mask[1] = highSet[0] = highSet[1] = lowClear[0] = lowClear[1] = 0;
    for (size_t i = 0; i < SWITCH_WAKE_PMC_EVENTS; ++i) {
        const unsigned e = switch_wake_pmc_events[i].event;
        if (!(events >> e & 1u)) continue;
        const unsigned bank = e / 32, bit = e % 32;
        mask[bank] |= 1u << bit;
        if (switch_wake_pmc_events[i].level == SWITCH_WAKE_HIGH)
            highSet[bank] |= 1u << bit;
        else
            lowClear[bank] |= 1u << bit;
    }
}

// The armed events, as a bit set; usbHigh / gaugeHigh: the USB-PD ALERT / gauge ALRT line reads high (a level-low line
// already low would wake at once).
static inline uint64_t switch_wake_pmc_armed(bool usbHigh, bool gaugeHigh)
{
    uint64_t events = 0;
    for (size_t i = 0; i < SWITCH_WAKE_PMC_EVENTS; ++i) {
        if (!switch_wake_pmc_events[i].armed) continue;
        if (switch_wake_pmc_events[i].event == 14 && !usbHigh) continue;
        if (switch_wake_pmc_events[i].event == 27 && !gaugeHigh) continue;
        events |= (uint64_t)1 << switch_wake_pmc_events[i].event;
    }
    return events;
}

// The first listed event among those set in the wake status registers (status[0] events 0-31, status[1] 32-63) and
// armed, by table order; "unknown" if none.
static inline const char *switch_wake_pmc_reason(const uint32_t status[2], uint64_t armed)
{
    for (size_t i = 0; i < SWITCH_WAKE_PMC_EVENTS; ++i) {
        const unsigned e = switch_wake_pmc_events[i].event;
        if ((armed >> e & 1u) && (status[e / 32] >> (e % 32) & 1u)) return switch_wake_pmc_events[i].name;
    }
    return "unknown";
}

#ifdef __cplusplus
}
#endif
