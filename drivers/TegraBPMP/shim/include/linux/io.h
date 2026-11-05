/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Switch TegraBPMP shim: MMIO accessors. The BPMP channel areas are device memory, so copies use naturally aligned
 * 32-bit accesses where they can and single bytes otherwise (no unaligned or paired accesses to Device memory). */
#ifndef BPXNU_LINUX_IO_H
#define BPXNU_LINUX_IO_H
/* The GM20B shim (same kernel) defines Linux names too: every function here is bpmp_linux_-prefixed. */
#define memcpy_toio(a0, a1, a2) bpmp_linux_memcpy_toio(a0, a1, a2)
#define memcpy_fromio(a0, a1, a2) bpmp_linux_memcpy_fromio(a0, a1, a2)
#include <linux/kernel.h>
static inline u32 __raw_readl(const volatile void *a) { return *(const volatile u32 *)a; }
static inline void __raw_writel(u32 v, volatile void *a) { *(volatile u32 *)a = v; }
static inline u32 readl(const volatile void *a) { u32 v = __raw_readl(a); __asm__ volatile("dmb ld" ::: "memory"); return v; }
static inline void writel(u32 v, volatile void *a) { __asm__ volatile("dmb st" ::: "memory"); __raw_writel(v, a); }
#define writel_relaxed(v, a) __raw_writel(v, a)
void memcpy_toio(volatile void *dst, const void *src, size_t n);
void memcpy_fromio(void *dst, const volatile void *src, size_t n);
static inline void *ioremap(phys_addr_t pa, size_t size) { return (void *)bpxnu_ioremap(pa, size); }
#endif
