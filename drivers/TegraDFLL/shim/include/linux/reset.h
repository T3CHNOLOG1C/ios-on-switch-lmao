/* SPDX-License-Identifier: GPL-2.0 */
/* IOSwitch shim: reset controls by name, backed by platform.c (the DFLL's "dvco" = CAR RST_DFLL_DVCO bit 0). */
#ifndef TDFLL_LINUX_RESET_H
#define TDFLL_LINUX_RESET_H
#include <linux/device.h>
struct reset_control;
struct reset_control *devm_reset_control_get(struct device *dev, const char *id);
int reset_control_assert(struct reset_control *rstc);
int reset_control_deassert(struct reset_control *rstc);
int reset_control_reset(struct reset_control *rstc);
#endif
