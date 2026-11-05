// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host harness for drivers/SwitchSuspend/SwitchWakeSources.h (tests/test_switch_wake.py drives it through ctypes).
#include <stdint.h>
#include <string.h>
#include "SwitchWakeSources.h"

static struct switch_wake_bt_edges edges;
static struct switch_wake_guard guard;
static struct switch_power_ring ring;

int wake_bt_home(const uint8_t *acl, size_t length, uint16_t *handle)
{
    return switch_wake_bt_home(acl, length, handle);
}

void wake_edges_reset(void)
{
    memset(&edges, 0, sizeof(edges));
}

int wake_bt_edge(uint16_t handle, int home)
{
    return switch_wake_bt_edge(&edges, handle, home != 0);
}

void wake_guard_reset(void)
{
    memset(&guard, 0, sizeof(guard));
}

int wake_guard_arm(unsigned source, uint64_t now, uint64_t timeout, int panelOn)
{
    return switch_wake_guard_arm(&guard, source, now, timeout, panelOn != 0);
}

void wake_guard_display_on(void)
{
    switch_wake_guard_display_on(&guard);
}

int wake_guard_check(uint64_t now, int panelOn, int onRequested)
{
    return switch_wake_guard_check(&guard, now, panelOn != 0, onRequested != 0);
}

unsigned wake_guard_source(void)
{
    return guard.source;
}

uint64_t wake_guard_deadline(void)
{
    return guard.deadline;
}

int wake_guard_armed(void)
{
    return guard.armed;
}

void ring_reset(void)
{
    memset(&ring, 0, sizeof(ring));
}

void ring_append(const char *text)
{
    switch_power_ring_append(&ring, text);
}

size_t ring_render(char *out, size_t capacity)
{
    return switch_power_ring_render(&ring, out, capacity);
}

unsigned ring_lines(void)
{
    return SWITCH_POWER_LINES;
}

unsigned ring_line_bytes(void)
{
    return SWITCH_POWER_LINE;
}

// mask[2], highSet[2], lowClear[2]
void pmc_registers(uint64_t events, uint32_t out[6])
{
    switch_wake_pmc_registers(events, out, out + 2, out + 4);
}

uint64_t pmc_armed(int usbHigh)
{
    return switch_wake_pmc_armed(usbHigh != 0);
}

const char *pmc_reason(uint32_t status0, uint32_t status1, uint64_t armed)
{
    const uint32_t status[2] = {status0, status1};
    return switch_wake_pmc_reason(status, armed);
}

const char *source_name(unsigned source)
{
    return switch_wake_source_name(source);
}

unsigned bt_peek(void)
{
    return SWITCH_WAKE_BT_PEEK;
}

unsigned guard_ms(void)
{
    return SWITCH_WAKE_GUARD_MS;
}
