/* a64stubdump WORD DEADMASK: print the a64x stub for an instruction word as hex (pipe into llvm-mc). */
#include "../a64x/a64x.h"
#include <stdio.h>
#include <stdlib.h>

int a64x_cmd_stubdump(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: arm64e stub WORD DEADMASK\n");
        return 2;
    }
    uint32_t w = (uint32_t)strtoul(argv[1], NULL, 16), dead = (uint32_t)strtoul(argv[2], NULL, 16), st[A64X_STUB_MAX];
    int n = a64x_stub(w, 0x100000, 0x200000, dead, st, A64X_STUB_MAX);
    for (int i = 0; i < n; i++) {
        printf("0x%02x 0x%02x 0x%02x 0x%02x\n", st[i] & 255, (st[i] >> 8) & 255, (st[i] >> 16) & 255, st[i] >> 24);
    }
    fprintf(stderr, "%d words\n", n);
    return 0;
}
