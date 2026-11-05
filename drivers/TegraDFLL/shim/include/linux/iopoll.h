/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Switch shim: register polls. Switch rule: no polling loops for hardware status. The DFLL monitor's DATA_NEW
 * flag (clk-dfll.c dfll_wait_monitor_data, two sample periods = 160 us at the DT's 12.5 kHz) has no interrupt line
 * (the DFLL's SPI 62 only reports output min/max limits, and L4T leaves it masked: DFLL_INTR_EN = 0), so the wait is
 * one bounded busy-delay of the full timeout followed by a single check. A miss returns -ETIMEDOUT exactly as Linux
 * does (clk-dfll.c tolerates it: dfll_get_monitor_data compares two consecutive readings) and is counted for
 * tegra_dfll_cpu_describe(). The delay is a busy-wait because the caller holds the DFLL spinlock with interrupts off.
 */
#ifndef TDFLL_LINUX_IOPOLL_H
#define TDFLL_LINUX_IOPOLL_H
#include <linux/kernel.h>
int tdfll_poll_timed_out(void);
#define readx_poll_timeout_atomic(op, addr, val, cond, delay_us, timeout_us) ({ \
	(void)(delay_us); \
	udelay(timeout_us); \
	(val) = op(addr); \
	(cond) ? 0 : tdfll_poll_timed_out(); })
#define readl_relaxed_poll_timeout_atomic(addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout_atomic(readl_relaxed, addr, val, cond, delay_us, timeout_us)
#endif
