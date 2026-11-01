/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IOSwitch shim: the regulator consumer calls clk-dfll.c and the fcpu driver make on "vdd-cpu". They only ask about
 * the voltage map (selectors, linear step, constraints) and the vsel register, so the regulator here is a description
 * of the MAX77812 M4 output (platform.c) plus the PMIC's I2C client: nothing in this API talks to the PMIC.
 * Semantics follow Linux 4.9 drivers/regulator/core.c (_regulator_list_voltage drops selectors outside constraints).
 */
#ifndef TDFLL_LINUX_REGULATOR_CONSUMER_H
#define TDFLL_LINUX_REGULATOR_CONSUMER_H
#include <linux/device.h>
struct regulator;
struct regmap;
struct regulator *devm_regulator_get(struct device *dev, const char *id);
void devm_regulator_put(struct regulator *regulator);
struct regmap *regulator_get_regmap(struct regulator *regulator);
int regulator_get_hardware_vsel_register(struct regulator *regulator, unsigned *vsel_reg, unsigned *vsel_mask);
int regulator_list_hardware_vsel(struct regulator *regulator, unsigned selector);
int regulator_count_voltages(struct regulator *regulator);
int regulator_list_voltage(struct regulator *regulator, unsigned selector);
int regulator_get_constraint_voltages(struct regulator *regulator, int *min_uV, int *max_uV);
unsigned int regulator_get_linear_step(struct regulator *regulator);
#endif
