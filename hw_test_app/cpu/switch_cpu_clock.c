#include "switch_cpu_clock.h"

#define SWITCH_CPU_CLOCK_TEXT(x) #x
#define SWITCH_CPU_CLOCK_REPEAT(x) SWITCH_CPU_CLOCK_TEXT(x)

__attribute__((noinline)) uint64_t switch_cpu_add_chain(uint64_t blocks)
{
    uint64_t x = blocks;
    for (uint64_t i = 0; i < blocks; i++)
        __asm__ volatile(".rept " SWITCH_CPU_CLOCK_REPEAT(SWITCH_CPU_CLOCK_BLOCK) "\n add %0, %0, #1\n .endr" : "+r"(x));
    return x;
}
