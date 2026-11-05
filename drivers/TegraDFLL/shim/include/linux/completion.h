/* SPDX-License-Identifier: GPL-2.0 */
/* Switch shim: struct completion as a type only (embedded in fuse.h's Tegra20 APBDMA state, unused on T210). */
#ifndef TDFLL_LINUX_COMPLETION_H
#define TDFLL_LINUX_COMPLETION_H
struct completion {
	unsigned int done;
};
#endif
