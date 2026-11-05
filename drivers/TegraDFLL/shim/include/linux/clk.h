/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IOSwitch shim: the common clock framework consumer API, implemented by a small clock tree in shim/linux.c that
 * follows L4T 4.9 drivers/clk/clk.c where the DFLL depends on it: clk_set_parent() sends PRE_PARENT_CHANGE before and
 * POST_PARENT_CHANGE after the mux write and migrates the prepare/enable counts of a prepared clock to its new parent
 * (that is what puts the DFLL into open loop before cclk_g switches to it and closes the loop after), and clk_set_rate()
 * skips a rate equal to the cached one and asks determine_rate first. Notifier action values are L4T's
 * (include/linux/clk.h: PARENT_CHANGE events are an NVIDIA addition).
 */
#ifndef TDFLL_LINUX_CLK_H
#define TDFLL_LINUX_CLK_H
#include <linux/kernel.h>
#include <linux/notifier.h>

struct device;
struct device_node;
struct clk;

#define PRE_RATE_CHANGE BIT(0)
#define POST_RATE_CHANGE BIT(1)
#define ABORT_RATE_CHANGE BIT(2)
#define PRE_SUBTREE_CHANGE BIT(3)
#define POST_SUBTREE_CHANGE BIT(4)
#define PRE_PARENT_CHANGE BIT(5)
#define POST_PARENT_CHANGE BIT(6)
#define ABORT_PARENT_CHANGE BIT(7)

struct clk_notifier_data {
	struct clk *clk;
	unsigned long old_rate;
	unsigned long new_rate;
};

int clk_notifier_register(struct clk *clk, struct notifier_block *nb);
int clk_notifier_unregister(struct clk *clk, struct notifier_block *nb);
struct clk *clk_get(struct device *dev, const char *id);
struct clk *devm_clk_get(struct device *dev, const char *id);
void clk_put(struct clk *clk);
int clk_prepare(struct clk *clk);
void clk_unprepare(struct clk *clk);
int clk_enable(struct clk *clk);
void clk_disable(struct clk *clk);
static inline int clk_prepare_enable(struct clk *clk)
{
	int ret = clk_prepare(clk);
	if (ret)
		return ret;
	ret = clk_enable(clk);
	if (ret)
		clk_unprepare(clk);
	return ret;
}
static inline void clk_disable_unprepare(struct clk *clk)
{
	clk_disable(clk);
	clk_unprepare(clk);
}
unsigned long clk_get_rate(struct clk *clk);
int clk_set_rate(struct clk *clk, unsigned long rate);
long clk_round_rate(struct clk *clk, unsigned long rate);
struct clk *clk_get_parent(struct clk *clk);
int clk_set_parent(struct clk *clk, struct clk *parent);
struct clk *of_clk_get_by_name(struct device_node *np, const char *name);
#endif
