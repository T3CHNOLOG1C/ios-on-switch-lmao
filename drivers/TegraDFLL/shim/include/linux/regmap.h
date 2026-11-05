/* SPDX-License-Identifier: GPL-2.0 */
/* Switch shim: a regmap is only a handle to its device here (regmap_get_device for dfll_fetch_i2c_params). */
#ifndef TDFLL_LINUX_REGMAP_H
#define TDFLL_LINUX_REGMAP_H
#include <linux/device.h>
struct regmap {
	struct device *dev;
};
static inline struct device *regmap_get_device(struct regmap *map) { return map ? map->dev : NULL; }
#endif
