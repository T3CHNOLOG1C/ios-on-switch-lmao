/*
 * The native-4K guards' blocks (xnu_guard_blocks.awk) of arm_init.c, commpage.c, mach_loader.c and vm_shared_region.c
 * in both trees, run against stubs of what they set. BUILD.bazel compiles this file twice: XNU_PAGE_SHIFTS names the
 * entry point, and SWITCH_VM_16K_PAGES is defined only for the 16k one, as the kernel toolchain does.
 */
#include <stdint.h>

#include "xnu_page_size_harness.h"

#define ARM_PGSHIFT         12
#define PAGE_MAX_SHIFT      14
#define FOURK_PAGE_SHIFT    12
#define SIXTEENK_PAGE_SHIFT 14

static int PAGE_SHIFT_CONST, page_shift_user32, vm_map_pageshift;
static const int is_64bit __attribute__((unused)) = 1;

static struct {
    uint64_t memSize;
} boot_args_model, *args = &boot_args_model;

static uint8_t commpage_rw, commpage_ro;
#define _COMM_PAGE_USER_PAGE_SHIFT_64_LEGACY ((uintptr_t)&commpage_rw)
#define _COMM_PAGE_RW_OFFSET                 0
#define _COMM_PAGE_USER_PAGE_SHIFT_64        ((uintptr_t)&commpage_ro)
#define _COMM_PAGE_RO_OFFSET                 0

static int map_model, sub_map_model;
static int *const map = &map_model, *const sub_map = &sub_map_model;

static void vm_map_set_page_shift(int *m, int shift)
{
    *m = shift;
}

#include "page_policy_10063_arm_init.h"
#include "page_policy_10063_commpage.h"
#include "page_policy_10063_mach_loader.h"
#include "page_policy_10063_shared_region.h"
#include "page_policy_12377_arm_init.h"
#include "page_policy_12377_commpage.h"
#include "page_policy_12377_mach_loader.h"
#include "page_policy_12377_shared_region.h"

void XNU_PAGE_SHIFTS(enum xnu_tree tree, uint64_t mem_size, struct xnu_page_shifts *out)
{
    args->memSize = mem_size;
    PAGE_SHIFT_CONST = page_shift_user32 = vm_map_pageshift = map_model = sub_map_model = 0;
    commpage_rw = commpage_ro = 0;
    if (tree == XNU_10063) {
        xnu10063_arm_init_0();
        xnu10063_arm_init_1();
        xnu10063_commpage_0();
        xnu10063_mach_loader_0();
        xnu10063_shared_region_0();
        *out = (struct xnu_page_shifts){PAGE_SHIFT_CONST, page_shift_user32, commpage_ro,
                                        commpage_rw,      map_model,         sub_map_model};
        return;
    }
    xnu12377_arm_init_0();
    xnu12377_arm_init_1();
    xnu12377_commpage_0();
    xnu12377_mach_loader_0();
    out->user_map = vm_map_pageshift;
    vm_map_pageshift = 0;
    xnu12377_shared_region_0();
    out->shared_region = vm_map_pageshift;
    out->kernel = PAGE_SHIFT_CONST;
    out->user32 = page_shift_user32;
    out->commpage = commpage_ro;
    out->commpage_legacy = commpage_rw;
}
