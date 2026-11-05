// switch_bluetooth_links_harness.c's exports: SwitchBluetoothLinks.h's functions on one link table and one request
// table, compiled as C.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void bt_links_reset(void);
int bt_link_open(uint16_t handle, uint8_t kind, const uint8_t *addr, uint8_t role);
int bt_link_close(uint16_t handle, uint32_t *out);
void bt_link_rx(const uint8_t *header);
void bt_link_tx(const uint8_t *header);
int bt_links_completed(const uint8_t *params, uint32_t length);
int bt_link_counts(uint16_t handle, uint32_t *out);
int bt_event_wanted(uint8_t code, uint8_t sub);
void bt_describe_event(char *out, size_t size, uint8_t code, const uint8_t *params, uint32_t length);
void bt_describe_command(char *out, size_t size, uint16_t opcode, const uint8_t *params, uint32_t length);
int bt_command_traced(uint16_t opcode);
void bt_requests_reset(void);
void bt_request_note(const uint8_t *addr, uint32_t cod);
int bt_accept_as_central(uint8_t *params, uint32_t length, uint32_t *cod);
int bt_cod_is_gamepad(uint32_t cod);

#ifdef __cplusplus
}
#endif
