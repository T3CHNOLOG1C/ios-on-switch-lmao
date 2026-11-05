/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Switch TegraBPMP shim: a device_node is the one static node TegraBPMP.c fills from the L4T DT; of_iomap maps its
 * idx'th reg entry. */
#ifndef BPXNU_LINUX_OF_ADDRESS_H
#define BPXNU_LINUX_OF_ADDRESS_H
/* The GM20B shim (same kernel) defines Linux names too: every function here is bpmp_linux_-prefixed. */
#define of_iomap(a0, a1) bpmp_linux_of_iomap(a0, a1)
#include <linux/io.h>
struct device_node {
	const char *full_name;
	struct { u64 base, size; } reg[2];
	unsigned int nreg;
};
void *of_iomap(struct device_node *np, int index);
#endif
