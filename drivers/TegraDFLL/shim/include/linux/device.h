/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Switch shim: struct device and the device-managed allocators. Like Linux 4.9's <linux/device.h>, this also brings
 * in what the vendored files reach through it implicitly (timers, notifiers, pinctrl consumer calls, pm ops).
 * devm_* allocations are never released: the DFLL device is probed once per boot and never removed.
 */
#ifndef TDFLL_LINUX_DEVICE_H
#define TDFLL_LINUX_DEVICE_H
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/timer.h>
#include <linux/hrtimer.h>

struct device_node;
struct device;

/* Linux 4.9 include/linux/pm.h: the members SET_RUNTIME_PM_OPS / SET_LATE_SYSTEM_SLEEP_PM_OPS fill */
struct dev_pm_ops {
	int (*suspend_late)(struct device *dev);
	int (*resume_early)(struct device *dev);
	int (*freeze_late)(struct device *dev);
	int (*thaw_early)(struct device *dev);
	int (*poweroff_late)(struct device *dev);
	int (*restore_early)(struct device *dev);
	int (*runtime_suspend)(struct device *dev);
	int (*runtime_resume)(struct device *dev);
	int (*runtime_idle)(struct device *dev);
};
#define SET_RUNTIME_PM_OPS(suspend_fn, resume_fn, idle_fn) \
	.runtime_suspend = suspend_fn, .runtime_resume = resume_fn, .runtime_idle = idle_fn,
#define SET_LATE_SYSTEM_SLEEP_PM_OPS(suspend_fn, resume_fn) \
	.suspend_late = suspend_fn, .resume_early = resume_fn, .freeze_late = suspend_fn, .thaw_early = resume_fn, \
	.poweroff_late = suspend_fn, .restore_early = resume_fn,

struct device_driver {
	const char *name;
	const struct of_device_id *of_match_table;
	const struct dev_pm_ops *pm;
	bool suppress_bind_attrs;
};

struct device {
	const char *init_name;
	struct device_node *of_node;
	struct device_driver *driver;
	void *driver_data;
	/* runtime PM (shim/linux.c pm_runtime_*): usage count, enabled, and whether runtime_resume ran */
	int tdfll_rpm_usage;
	bool tdfll_rpm_enabled;
	bool tdfll_rpm_active;
};

static inline const char *dev_name(const struct device *dev) { return dev->init_name ? dev->init_name : "?"; }
static inline void *dev_get_drvdata(const struct device *dev) { return dev->driver_data; }
static inline void dev_set_drvdata(struct device *dev, void *data) { dev->driver_data = data; }

#define dev_err(dev, fmt, ...) printk("%s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...) printk("%s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_info(dev, fmt, ...) printk("%s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...) printk("%s: " fmt, dev_name(dev), ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...) ({ if (0) printk("%s: " fmt, dev_name(dev), ##__VA_ARGS__); 0; })

/* allocation (GFP flags are irrelevant here: every caller is in thread context) */
#define GFP_KERNEL 0u
#define GFP_ATOMIC 1u
void *kmalloc(size_t size, gfp_t flags);
void *kzalloc(size_t size, gfp_t flags);
void kfree(const void *p);
void *devm_kzalloc(struct device *dev, size_t size, gfp_t flags);
void *devm_kmalloc(struct device *dev, size_t size, gfp_t flags);
void devm_kfree(struct device *dev, void *p);

/* pinctrl consumer API: only the PWM-to-PMIC path of clk-dfll.c uses it; this board drives the PMIC over I2C */
struct pinctrl;
struct pinctrl_state;
struct pinctrl *devm_pinctrl_get(struct device *dev);
struct pinctrl_state *pinctrl_lookup_state(struct pinctrl *p, const char *name);
int pinctrl_select_state(struct pinctrl *p, struct pinctrl_state *s);

#endif
