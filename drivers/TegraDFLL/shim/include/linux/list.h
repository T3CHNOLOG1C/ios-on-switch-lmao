/* SPDX-License-Identifier: GPL-2.0 */
/* Switch shim: struct list_head only (embedded by value in the vendored drivers/clk/tegra/clk.h). */
#ifndef TDFLL_LINUX_LIST_H
#define TDFLL_LINUX_LIST_H
struct list_head {
	struct list_head *next, *prev;
};
#endif
