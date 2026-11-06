// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM pmap hooks. Included by osfmk/arm/pmap/pmap.c after its own headers (pt_entry_t, ARM_PTE_*).
 */
#ifndef SWITCH_PMAP_H
#define SWITCH_PMAP_H

#include <stdbool.h>

/*
 * arm_clear_fast_fault: whether rewriting a live PTE needs a TLB invalidate. The software writeable bit is invisible to
 * the walker, and so is a change that only sets the Access flag: the A57 has no hardware Access flag (ARMv8.0, no
 * FEAT_HAFDBS), and the Arm ARM (DDI 0487, TLB maintenance requirements) never lets a TLB hold an entry that generates
 * an Access flag fault, so no TLB holds the old entry. Such a write needs only pmap.c's FLUSH_PTE, as its comment says
 * of PTEs "that had the AF bit cleared". Every reference-bit trap on a page skips 4 TLBIs and their DSB (PAGE_RATIO 4).
 */
static inline bool switch_pte_needs_tlbi(pt_entry_t old_pte, pt_entry_t new_pte)
{
    const pt_entry_t changed = (old_pte ^ new_pte) & ~ARM_PTE_WRITEABLE;

    return changed != 0 && (changed != ARM_PTE_AF || (old_pte & ARM_PTE_AF) != 0);
}

#endif
