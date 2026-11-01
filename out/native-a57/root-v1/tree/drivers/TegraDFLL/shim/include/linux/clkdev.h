/* SPDX-License-Identifier: GPL-2.0 */
/* IOSwitch shim: clkdev lookups. Consumers find clocks by name in shim/linux.c's clock table instead. */
#ifndef TDFLL_LINUX_CLKDEV_H
#define TDFLL_LINUX_CLKDEV_H
#include <linux/clk.h>
#include <linux/list.h>
struct clk_hw;
struct clk_lookup {
	struct list_head node;
	const char *dev_id;
	const char *con_id;
	struct clk *clk;
	struct clk_hw *clk_hw;
};
int clk_register_clkdev(struct clk *clk, const char *con_id, const char *dev_fmt, ...);
#endif
