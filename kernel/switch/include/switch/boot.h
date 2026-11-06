// SPDX-License-Identifier: GPL-2.0-only
/*
 * The console's boot path through bsd_init (kernel/switch/boot/bsd_init.c). Included by bsd/kern/bsd_init.c.
 */
#ifndef SWITCH_BOOT_H
#define SWITCH_BOOT_H

#include <mach/kern_return.h>

/* After bsd_bufferinit: the original PMGR nub. */
void switch_boot_bufferinit_done(void);
/* After vfsinit: APFS, IOMobileGraphicsFamily and the accelerator stub, before anything mounts or draws. */
void switch_boot_vfs_ready(void);
/* bsd_autoconf's result, once IOBSD is published. */
kern_return_t switch_boot_iokit_bsd_ready(kern_return_t result);
/* After bsd_autoconf. */
void switch_boot_autoconf_done(void);
void switch_boot_mountroot_begin(void);
/* Panics when err is not 0: there is no other root to try. */
void switch_boot_mountroot_done(int err);
/* Whether the owner-signed policy accepts the root FSIOC_KERNEL_ROOTAUTH refused with autherr. */
int switch_boot_root_authorized(int autherr);
void switch_boot_root_accepted(int autherr, int owner_authorized);

#endif
