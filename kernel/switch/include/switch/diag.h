// SPDX-License-Identifier: GPL-2.0-only
/*
 * What a stuck boot prints before its panic (kernel/switch/diag/threads.c). NativeSwitchBlock calls it by name.
 */
#ifndef SWITCH_DIAG_H
#define SWITCH_DIAG_H

#include <sys/cdefs.h>

__BEGIN_DECLS

void switch_print_blocked_kext_threads(const char *(*name_of)(const void *), unsigned max_threads);
void switch_print_thread(const void *thread, const char *who);
void switch_print_processors(const char *who);

__END_DECLS

#endif
