// switch_joycon_session_harness.c's exports: SwitchJoyConSession.h's session rule and republish guard, compiled as C,
// on the header's own structs (and their sizes as C lays them out).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "../SwitchJoyConSession.h"

#ifdef __cplusplus
extern "C" {
#endif

int jc_session_open(struct switch_joycon_session *session, uintptr_t client, int32_t pid, int backboardd);
int jc_session_close(struct switch_joycon_session *session, uintptr_t client);
int jc_republish_allowed(struct switch_joycon_republish_guard *guard, int32_t pid, uint64_t now_ms);
unsigned jc_republish_gap_ms(void);
size_t jc_session_size(void);
size_t jc_guard_size(void);

#ifdef __cplusplus
}
#endif
