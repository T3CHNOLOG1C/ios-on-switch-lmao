/* SPDX-License-Identifier: GPL-2.0 */
/* IOSwitch shim: struct mutex as a type only (drivers/soc/tegra/fuse/fuse.h embeds one; nothing here locks it). */
#ifndef TDFLL_LINUX_MUTEX_H
#define TDFLL_LINUX_MUTEX_H
struct mutex {
	void *tdfll_unused;
};
#endif
