/* SPDX-License-Identifier: GPL-2.0 */
/* IOSwitch shim: struct i2c_client, as dfll_fetch_i2c_params() reaches the PMIC's address through the regmap's device. */
#ifndef TDFLL_LINUX_I2C_H
#define TDFLL_LINUX_I2C_H
#include <linux/device.h>
struct i2c_client {
	unsigned short flags;
	unsigned short addr;
	char name[20];
	struct device dev;
};
#define to_i2c_client(d) container_of(d, struct i2c_client, dev)
#endif
