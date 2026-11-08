// SPDX-License-Identifier: MIT
// gpu.sm53_probe: NAK reads NAK_DEBUG once, when this process's first Metal device makes its compiler, and a framework
// may make one before the app's code runs. A constructor runs before any of it. Only this app's compiles are affected;
// release images carry neither the app nor the flag (NAK compiles it out).
#include <stdlib.h>

__attribute__((constructor)) static void switch_hwtest_sm53_probe(void)
{
    setenv("NAK_DEBUG", "sm53_probe", 0);
}
