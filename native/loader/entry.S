// Share the measured EL2 -> EL1 sequence, vectors and private stacks.
// The standalone handoff diagnostic retains its original callback.
#define UINT64_C(value) value
#include "layout.h"
#if defined(LOAD_FAST_COPY) && LOAD_FAST_COPY
#define PROBE_STACK_BYTES 0xc000
#endif
#define probe_after_transition loader_after_transition
#include "../handoff/entry.S"

.section .text, "ax"
.global loader_enter_kernel
loader_enter_kernel:
    // x0 = physical boot_args, x1 = physical entry. Never return to firmware.
    mov x16, x1
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    mov x30, xzr
    dsb sy
    ic iallu
    dsb sy
    isb
    br x16
