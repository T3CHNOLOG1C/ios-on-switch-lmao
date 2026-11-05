// Host harness for drivers/SwitchJoyCon/SwitchJoyConSession.h (switch_joycon_session_test.cpp): the header's static
// inline functions compiled as C and exported (switch_joycon_session_harness.h), on the header's own structs.
#include <stddef.h>

#include "../SwitchJoyConSession.h"
#include "switch_joycon_session_harness.h"

int jc_session_open(struct switch_joycon_session *session, uintptr_t client, int32_t pid, int backboardd)
{
    return (int)switch_joycon_session_open(session, client, pid, backboardd != 0);
}

int jc_session_close(struct switch_joycon_session *session, uintptr_t client)
{
    return (int)switch_joycon_session_close(session, client);
}

int jc_republish_allowed(struct switch_joycon_republish_guard *guard, int32_t pid, uint64_t now_ms)
{
    return switch_joycon_republish_allowed(guard, pid, now_ms);
}

unsigned jc_republish_gap_ms(void)
{
    return kSwitchJoyConRepublishGapMS;
}
size_t jc_session_size(void)
{
    return sizeof(struct switch_joycon_session);
}
size_t jc_guard_size(void)
{
    return sizeof(struct switch_joycon_republish_guard);
}
