/* SPDX-License-Identifier: GPL-2.0 */
/* Switch shim: notifier blocks and return codes (Linux 4.9 include/linux/notifier.h). */
#ifndef TDFLL_LINUX_NOTIFIER_H
#define TDFLL_LINUX_NOTIFIER_H
struct notifier_block;
typedef int (*notifier_fn_t)(struct notifier_block *nb, unsigned long action, void *data);
struct notifier_block {
	notifier_fn_t notifier_call;
	struct notifier_block *next;
	int priority;
};
#define NOTIFY_DONE 0x0000
#define NOTIFY_OK 0x0001
#define NOTIFY_STOP_MASK 0x8000
#define NOTIFY_BAD (NOTIFY_STOP_MASK | 0x0002)
#define NOTIFY_STOP (NOTIFY_OK | NOTIFY_STOP_MASK)
#endif
