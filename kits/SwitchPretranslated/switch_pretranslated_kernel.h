/*
 * The 12377 kernel's side of iOS 27's System files translated offline (bsd/vfs/vfs_switch_pretranslated.c), for every
 * XNU component that calls it: each one's -I diagnostics/switch reaches kits/ as ../../kits.
 */
#ifndef SWITCH_PRETRANSLATED_KERNEL_H
#define SWITCH_PRETRANSLATED_KERNEL_H

#include <sys/cdefs.h>
#include <stdbool.h>
#include <stdint.h>

#include "switch_pretranslated.h"

struct nameidata;
struct vnode;

__BEGIN_DECLS

extern bool switch_pretranslated_on;

void switch_pretranslated_capture(void);
void switch_pretranslated_redirect(struct nameidata *ndp);
bool switch_pretranslated_trusted(struct vnode *vp, int64_t arch_offset, bool redirected);
int switch_pretranslated_refuse(enum switch_pt_op op, struct vnode *v0, struct vnode *v1, struct vnode *v2,
                                struct vnode *v3, uint32_t intent);

static inline void switch_pretranslated_lookup(struct nameidata *ndp)
{
    if (__improbable(__atomic_load_n(&switch_pretranslated_on, __ATOMIC_RELAXED))) {
        switch_pretranslated_redirect(ndp);
    }
}

/* EPERM when op would change the pretranslated tree (vnodes in the order switch_pt_denied names them), else 0. */
static inline int switch_pretranslated_check(enum switch_pt_op op, struct vnode *v0, struct vnode *v1, struct vnode *v2,
                                             struct vnode *v3, uint32_t intent)
{
    if (__improbable(__atomic_load_n(&switch_pretranslated_on, __ATOMIC_RELAXED))) {
        return switch_pretranslated_refuse(op, v0, v1, v2, v3, intent);
    }
    return 0;
}

__END_DECLS

#endif /* SWITCH_PRETRANSLATED_KERNEL_H */
