/*
 * The 12377 kernel's side of iOS 27's System files translated offline (bsd/vfs/vfs_switch_pretranslated.c), for every
 * XNU component that calls it: each one's -I diagnostics/switch reaches kits/ as ../../kits.
 */
#ifndef SWITCH_PRETRANSLATED_KERNEL_H
#define SWITCH_PRETRANSLATED_KERNEL_H

#include <sys/cdefs.h>
#include <stdbool.h>

struct nameidata;

__BEGIN_DECLS

extern bool switch_pretranslated_on;

void switch_pretranslated_capture(void);
void switch_pretranslated_redirect(struct nameidata *ndp);

static inline void switch_pretranslated_lookup(struct nameidata *ndp)
{
    if (__improbable(__atomic_load_n(&switch_pretranslated_on, __ATOMIC_RELAXED))) {
        switch_pretranslated_redirect(ndp);
    }
}

__END_DECLS

#endif /* SWITCH_PRETRANSLATED_KERNEL_H */
