// Host harness for drivers/SwitchBluetooth/SwitchBluetoothLinks.h (tests/test_switch_bluetooth_links.py): the header's
// static inline functions as exported ones for ctypes, on one table of the header's own struct.
#include <stdio.h>

#include "SwitchBluetoothLinks.h"

static struct switch_bt_links table;

void bt_links_reset(void)
{
    const struct switch_bt_links empty = {0};
    table = empty;
}

int bt_link_open(uint16_t handle, uint8_t kind, const uint8_t *addr, uint8_t role)
{
    return switch_bt_link_open(&table, handle, kind, addr, role) != NULL;
}

// The closed link's rx, tx, done, peak into out[4]; 0 if the handle was not open.
int bt_link_close(uint16_t handle, uint32_t *out)
{
    struct switch_bt_link last;
    if (!switch_bt_link_close(&table, handle, &last)) return 0;
    out[0] = last.rx;
    out[1] = last.tx;
    out[2] = last.done;
    out[3] = last.peak;
    return 1;
}

void bt_link_rx(const uint8_t *header) { switch_bt_link_rx(&table, switch_bt_acl_handle(header)); }
void bt_link_tx(const uint8_t *header) { switch_bt_link_tx(&table, switch_bt_acl_handle(header)); }
int bt_links_completed(const uint8_t *params, uint32_t length)
{
    return switch_bt_links_completed(&table, params, length);
}

// One link's counters (rx, tx, done, peak) into out[4], and the stray counts into out[4..5]; 0 if not open.
int bt_link_counts(uint16_t handle, uint32_t *out)
{
    out[4] = table.strayTx;
    out[5] = table.strayDone;
    const struct switch_bt_link *link = switch_bt_link_find(&table, handle);
    if (!link) return 0;
    out[0] = link->rx;
    out[1] = link->tx;
    out[2] = link->done;
    out[3] = link->peak;
    return 1;
}

int bt_event_wanted(uint8_t code, uint8_t sub) { return switch_bt_event_wanted(code, sub); }

void bt_describe_event(char *out, size_t size, uint8_t code, const uint8_t *params, uint32_t length)
{
    switch_bt_describe_event(out, size, code, params, length);
}

void bt_describe_command(char *out, size_t size, uint16_t opcode, const uint8_t *params, uint32_t length)
{
    switch_bt_describe_command(out, size, opcode, params, length);
}

int bt_command_traced(uint16_t opcode) { return switch_bt_link_command_name(opcode) != NULL; }
