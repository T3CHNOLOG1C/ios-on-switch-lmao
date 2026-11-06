// SPDX-License-Identifier: GPL-2.0-only
/*
 * sleh's undefined-instruction trap: what the load-time translator left (LSE where it could not give a stub) is
 * emulated (kernel/switch/translate/a64x_xnu.c). Included where arm_saved_state_t is already defined.
 */
#ifndef SWITCH_A64X_TRAP_H
#define SWITCH_A64X_TRAP_H

#if CONFIG_A64X
int a64x_emulate_user(arm_saved_state_t *state, uint32_t instr);

static inline bool switch_a64x_emulated(arm_saved_state_t *state, uint32_t instr)
{
    return PSR64_IS_USER64(get_saved_state_cpsr(state)) && a64x_emulate_user(state, instr);
}
#else
static inline bool switch_a64x_emulated(arm_saved_state_t *state, uint32_t instr)
{
    (void)state, (void)instr;
    return false;
}
#endif

#endif
