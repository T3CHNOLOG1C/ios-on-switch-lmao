// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shut down and restart (kernel/switch/shutdown/shutdown.c). Included by bsd/kern/kern_shutdown.c (reboot_kernel, once
 * its caller won the shutdown) and iokit/Kernel/IOPMrootDomain.cpp (halt_log_enter, which reboot_kernel and the IOKit
 * halt path call as each of their steps ends).
 */
#ifndef SWITCH_SHUTDOWN_H
#define SWITCH_SHUTDOWN_H

#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

void switch_shutdown_begin(int howto);
void switch_shutdown_step(const char *what, const void *pc, uint64_t elapsed);

__END_DECLS

#endif
