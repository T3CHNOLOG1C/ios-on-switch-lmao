/* SPDX-License-Identifier: GPL-2.0 */
/* Switch shim: no debugfs (CONFIG_DEBUG_FS is off; clk-dfll.c still removes its directory on unregister). */
#ifndef TDFLL_LINUX_DEBUGFS_H
#define TDFLL_LINUX_DEBUGFS_H
#include <linux/kernel.h>
struct dentry;
static inline void debugfs_remove_recursive(struct dentry *dentry) { (void)dentry; }
#endif
