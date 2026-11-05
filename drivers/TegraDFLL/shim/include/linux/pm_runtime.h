/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Switch shim: runtime PM as a usage count. While enabled, 0 -> 1 runs the driver's runtime_resume
 * (tegra_dfll_runtime_resume: enable ref/soc/i2c clocks) and 1 -> 0 runs runtime_suspend, synchronously.
 */
#ifndef TDFLL_LINUX_PM_RUNTIME_H
#define TDFLL_LINUX_PM_RUNTIME_H
#include <linux/device.h>
void pm_runtime_enable(struct device *dev);
void pm_runtime_disable(struct device *dev);
int pm_runtime_get_sync(struct device *dev);
int pm_runtime_put_sync(struct device *dev);
#define pm_runtime_get(dev) pm_runtime_get_sync(dev)
#define pm_runtime_put(dev) pm_runtime_put_sync(dev)
static inline bool pm_runtime_enabled(struct device *dev) { return dev->tdfll_rpm_enabled; }
#endif
