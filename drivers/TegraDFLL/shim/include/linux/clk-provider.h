/* SPDX-License-Identifier: GPL-2.0 */
/* Switch shim: clock provider types (L4T 4.9 include/linux/clk-provider.h) for the shim's clock tree. */
#ifndef TDFLL_LINUX_CLK_PROVIDER_H
#define TDFLL_LINUX_CLK_PROVIDER_H
#include <linux/clk.h>
#include <linux/device.h>

#define CLK_SET_RATE_GATE BIT(0)
#define CLK_SET_PARENT_GATE BIT(1)
#define CLK_SET_RATE_PARENT BIT(2)
#define CLK_IGNORE_UNUSED BIT(3)
#define CLK_IS_BASIC BIT(5)
#define CLK_GET_RATE_NOCACHE BIT(6)
#define CLK_SET_RATE_NO_REPARENT BIT(7)
#define CLK_IS_CRITICAL BIT(11)
#define CLK_OPS_PARENT_ENABLE BIT(12)
#define CLK_SET_RATE_NOCACHE BIT(13)

struct clk_core;
struct clk_hw;
struct dentry;
struct of_phandle_args;

struct clk_rate_request {
	unsigned long rate;
	unsigned long min_rate;
	unsigned long max_rate;
	unsigned long best_parent_rate;
	struct clk_hw *best_parent_hw;
};

struct clk_ops {
	int (*prepare)(struct clk_hw *hw);
	void (*unprepare)(struct clk_hw *hw);
	int (*is_prepared)(struct clk_hw *hw);
	void (*unprepare_unused)(struct clk_hw *hw);
	int (*enable)(struct clk_hw *hw);
	void (*disable)(struct clk_hw *hw);
	int (*is_enabled)(struct clk_hw *hw);
	void (*disable_unused)(struct clk_hw *hw);
	unsigned long (*recalc_rate)(struct clk_hw *hw, unsigned long parent_rate);
	long (*round_rate)(struct clk_hw *hw, unsigned long rate, unsigned long *parent_rate);
	int (*determine_rate)(struct clk_hw *hw, struct clk_rate_request *req);
	int (*set_parent)(struct clk_hw *hw, u8 index);
	u8 (*get_parent)(struct clk_hw *hw);
	int (*set_rate)(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate);
	int (*set_rate_and_parent)(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate, u8 index);
	unsigned long (*recalc_accuracy)(struct clk_hw *hw, unsigned long parent_accuracy);
	int (*get_phase)(struct clk_hw *hw);
	int (*set_phase)(struct clk_hw *hw, int degrees);
	void (*init)(struct clk_hw *hw);
	int (*debug_init)(struct clk_hw *hw, struct dentry *dentry);
};

struct clk_init_data {
	const char *name;
	const struct clk_ops *ops;
	const char *const *parent_names;
	u8 num_parents;
	unsigned long flags;
};

struct clk_hw {
	struct clk_core *core;
	struct clk *clk;
	const struct clk_init_data *init;
};

struct clk_div_table {
	unsigned int val;
	unsigned int div;
};

struct clk_mux {
	struct clk_hw hw;
	void __iomem *reg;
	u32 *table;
	u32 mask;
	u8 shift;
	u8 flags;
	spinlock_t *lock;
};

struct clk *clk_register(struct device *dev, struct clk_hw *hw);
void clk_unregister(struct clk *clk);
const char *__clk_get_name(const struct clk *clk);
struct clk_hw *__clk_get_hw(struct clk *clk);
unsigned long clk_hw_get_rate(const struct clk_hw *hw);

/* clock providers: the DFLL registers itself as one; nothing here looks clocks up by phandle */
struct clk *of_clk_src_simple_get(struct of_phandle_args *clkspec, void *data);
int of_clk_add_provider(struct device_node *np, struct clk *(*clk_src_get)(struct of_phandle_args *args, void *data),
			void *data);
void of_clk_del_provider(struct device_node *np);
#endif
