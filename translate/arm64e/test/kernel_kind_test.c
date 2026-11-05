/*
 * The kernel's loader decision (xnu/bsd/sys/a64x_xnu.h, compiled here as it is) against the host mirror the offline
 * translation uses (macho/macho.h a64x_kind_of), for every cputype/subtype pair that matters.
 */
#include <mach/mach_types.h>
#include <string.h>
/* the kernel header uses struct vnode, which user space headers never declare */
struct vnode;
#define XNU_KERNEL_PRIVATE 1
#define CONFIG_A64X        1
#include "../xnu/bsd/sys/a64x_xnu.h"

#include "../macho/macho.h"
#include "bazel/testing/c_check.h"

static int kernel_to_host(int kind)
{
    switch (kind) {
    case A64X_IMAGE_ARM64E: return A64X_KIND_ARM64E;
    case A64X_IMAGE_ARM64: return A64X_KIND_ARM64;
    default: return A64X_KIND_NATIVE;
    }
}

void kernel_kind_scenario(void)
{
    static const cpu_type_t types[] = {CPU_TYPE_ARM64, CPU_TYPE_ARM64_32, CPU_TYPE_X86_64, CPU_TYPE_ARM};
    for (unsigned t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        for (uint32_t sub = 0; sub < 16; sub++) {
            for (uint32_t abi = 0; abi < 4; abi++) {
                cpu_subtype_t st = (cpu_subtype_t)(sub | (abi == 0   ? 0
                                                          : abi == 1 ? 0x80000000u
                                                          : abi == 2 ? 0x81000000u
                                                                     : 0x0F000000u));
                int kernel = kernel_to_host(a64x_image_kind(types[t], st));
                int host = (int)a64x_kind_of((uint32_t)types[t], (uint32_t)st, 0);
                if (!CHECK(kernel == host))
                    FAIL_C("cputype 0x%x subtype 0x%x: kernel %d, host %d", types[t], st, kernel, host);
                int translated = a64x_subtype_translated(types[t], st);
                CHECK(translated == (host == A64X_KIND_ARM64E));
            }
        }
    }
    CHECK(!strcmp(A64X_TARGET_SECTNAME, "__a64x_target"));
}
