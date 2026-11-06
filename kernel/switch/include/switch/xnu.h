// SPDX-License-Identifier: GPL-2.0-only
/*
 * XNU interfaces whose shape differs between the trees we build, in one spelling for kernel/switch's code.
 */
#ifndef SWITCH_XNU_H
#define SWITCH_XNU_H

#include <kern/task.h>
#include <string.h>

static inline void switch_task_name(task_t task, char *buf, size_t size)
{
#if XNU_12377_121_6
    strlcpy(buf, task_best_name(task), size);
#else
    task_best_name(task, buf, size);
#endif
}

#endif
