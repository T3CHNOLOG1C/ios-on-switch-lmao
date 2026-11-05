/* SPDX-License-Identifier: GPL-2.0 */
/* IOSwitch shim: get_cpu_device(), the device that carries the CPU OPP table (platform.c: CPU0). */
#ifndef TDFLL_LINUX_CPU_H
#define TDFLL_LINUX_CPU_H
#include <linux/device.h>
struct device *get_cpu_device(unsigned cpu);
#endif
