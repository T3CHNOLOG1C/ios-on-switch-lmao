/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* IOSwitch TegraBPMP shim: zeroed kernel heap (IOMalloc via shim/xnu_glue.cpp). */
#ifndef BPXNU_LINUX_SLAB_H
#define BPXNU_LINUX_SLAB_H
#include <linux/kernel.h>
#define GFP_KERNEL 0u
static inline void *kzalloc(size_t size, gfp_t flags) { return bpxnu_zalloc(size); }
static inline void *kcalloc(size_t n, size_t size, gfp_t flags)
{
	return n && size > (size_t)-1 / n ? NULL : bpxnu_zalloc(n * size);
}
static inline void kfree(const void *p) { if (p) bpxnu_free((void *)p); }
#endif
