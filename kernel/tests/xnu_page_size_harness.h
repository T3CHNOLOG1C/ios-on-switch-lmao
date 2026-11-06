/*
 * Host harness for the page sizes the native-4K guards choose (xnu_page_size_harness.c, compiled once with
 * SWITCH_VM_16K_PAGES and once without): the kernel's, 32-bit processes', the commpage's, a 64-bit process map's and the
 * shared region's, as page shifts.
 */
#ifndef XNU_PAGE_SIZE_HARNESS_H
#define XNU_PAGE_SIZE_HARNESS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xnu_page_shifts {
    int kernel, user32, commpage, commpage_legacy, user_map, shared_region;
};

enum xnu_tree {
    XNU_10063,
    XNU_12377
};

void xnu_page_shifts_16k(enum xnu_tree tree, uint64_t mem_size, struct xnu_page_shifts *out);
void xnu_page_shifts_4k(enum xnu_tree tree, uint64_t mem_size, struct xnu_page_shifts *out);

#ifdef __cplusplus
}
#endif

#endif
