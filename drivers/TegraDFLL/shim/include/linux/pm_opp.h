/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Switch shim: operating performance points (Linux 4.9 drivers/base/power/opp) as one sorted table per device,
 * which is all cvb.c builds and clk-dfll.c searches (CPU0's DFLL table: frequency -> CVB voltage).
 */
#ifndef TDFLL_LINUX_PM_OPP_H
#define TDFLL_LINUX_PM_OPP_H
#include <linux/device.h>
struct dev_pm_opp;
int dev_pm_opp_add(struct device *dev, unsigned long freq, unsigned long u_volt);
void dev_pm_opp_remove(struct device *dev, unsigned long freq);
struct dev_pm_opp *dev_pm_opp_find_freq_ceil(struct device *dev, unsigned long *freq);
struct dev_pm_opp *dev_pm_opp_find_freq_floor(struct device *dev, unsigned long *freq);
unsigned long dev_pm_opp_get_voltage(struct dev_pm_opp *opp);
unsigned long dev_pm_opp_get_freq(struct dev_pm_opp *opp);
int dev_pm_opp_get_opp_count(struct device *dev);
#endif
