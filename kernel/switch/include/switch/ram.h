// SPDX-License-Identifier: GPL-2.0-only
/*
 * The console's RAM in XNU's arm64 VM bootstrap (kernel/switch/ram/ram.c): the bootloader's ledger of banks and live
 * reservations (boot/loader/ram.h), a dense vm_pages array over the holes between them, and the GPU pool, which the VM
 * never gets and the physical aperture maps write-combined.
 *
 * Included by osfmk/arm64/arm_vm_init.c, osfmk/arm/pmap/pmap.c, osfmk/arm/pmap/pmap_data.h and osfmk/vm/vm_resident.c.
 */
#ifndef SWITCH_RAM_H
#define SWITCH_RAM_H

#include "../../../../boot/loader/ram.h"

struct boot_args;
struct vm_page;

boolean_t pmap_switch_ram_enabled(void);
boolean_t pmap_switch_gpu_pool(pmap_paddr_t *start, pmap_paddr_t *end);
boolean_t pmap_switch_pa_valid(pmap_paddr_t pa);
boolean_t pmap_switch_ram_reserved(ppnum_t pnum);
void pmap_switch_bootstrap_check(pmap_paddr_t start, pmap_paddr_t end);
unsigned int pmap_switch_dense_page_count(void);
boolean_t pmap_switch_begin_dense_pages(void);
boolean_t pmap_switch_page_reserved(ppnum_t pnum);

/* Set once the ledger is committed (the dense span exists), and once vm_pages owns the span. */
extern bool switch_ram_committed, switch_dense_pages;

/* arm_vm_init */
void pmap_switch_ram_init(struct boot_args *args);
void switch_ram_check_memory_size(uint64_t requested, uint64_t present);
void switch_ram_account(uint64_t present);
void switch_ram_bootstrap_done(void);

/* pmap.c */
boolean_t switch_ram_virtual_region(unsigned int region_select, vm_map_offset_t *startp, vm_map_size_t *size,
                                    boolean_t *ret);
void switch_ram_count_pages(unsigned int *avail_page_count, pmap_paddr_t end);
void switch_ram_skip_reserved_slow(void);

/* pmap_next_page, before it hands out first_avail: the span's holes are skipped until vm_pages owns them. */
static inline void switch_ram_skip_reserved(void)
{
    if (__improbable(switch_ram_committed && !switch_dense_pages)) {
        switch_ram_skip_reserved_slow();
    }
}

/* pmap_next_page: whether the page it hands out is one of avail_page_count's (a hole is not). */
static inline bool switch_ram_page_counted(ppnum_t pnum)
{
    return !__improbable(switch_dense_pages) || !pmap_switch_page_reserved(pnum);
}

/* vm_resident.c pmap_startup */
void switch_ram_dense_begin(void);
boolean_t switch_ram_hole_init_slow(struct vm_page *page, ppnum_t phys_page);
unsigned int switch_ram_dense_done(unsigned int count);

/* vm_pages covers the whole span, holes included: PPNs come from array indices. */
static inline unsigned int switch_ram_vm_pages(unsigned int npages)
{
    return __improbable(switch_ram_committed) ? pmap_switch_dense_page_count() : npages;
}

/* The span is initialized in one pass: no delayed pages, which would compact its holes. */
static inline bool switch_ram_dense(void)
{
    return __improbable(switch_ram_committed);
}

/* A hole of the span: a busy, failed descriptor that is never freed (only descriptor memory is touched). */
static inline bool switch_ram_hole_init(struct vm_page *page, ppnum_t phys_page)
{
    return __improbable(switch_ram_committed) && switch_ram_hole_init_slow(page, phys_page);
}

/*
 * arm_vm_physmap_init: the physical aperture over the ledger's RAM only, the GPU pool write-combined; without a ledger,
 * XNU's one slide. arm_vm_page_granular_helper and _prot take the cache attribute of each page from
 * SWITCH_RAM_PHYSMAP_ATTR.
 */
#define SWITCH_RAM_PHYSMAP_PIECES 12u

struct switch_ram_piece {
    pmap_paddr_t start, end;
    bool wcomb;
};

extern bool switch_physmap_wcomb;
unsigned int switch_ram_physmap_pieces(pmap_paddr_t start, pmap_paddr_t end,
                                       struct switch_ram_piece pieces[SWITCH_RAM_PHYSMAP_PIECES]);

#define SWITCH_RAM_PHYSMAP_SLIDE(slide, table, va, len, ap, granule)                                                  \
    do {                                                                                                              \
        if (!pmap_switch_ram_enabled()) {                                                                             \
            slide(table, va, len, ap, granule);                                                                       \
            break;                                                                                                    \
        }                                                                                                             \
        struct switch_ram_piece switch_pieces_[SWITCH_RAM_PHYSMAP_PIECES];                                            \
        const pmap_paddr_t switch_pa_ = (va) - gVirtBase + gPhysBase;                                                 \
        const unsigned int switch_count_ = switch_ram_physmap_pieces(switch_pa_, switch_pa_ + (len), switch_pieces_); \
        for (unsigned int switch_i_ = 0; switch_i_ < switch_count_; switch_i_++) {                                    \
            switch_physmap_wcomb = switch_pieces_[switch_i_].wcomb;                                                   \
            slide(table, switch_pieces_[switch_i_].start - gPhysBase + gVirtBase,                                     \
                  switch_pieces_[switch_i_].end - switch_pieces_[switch_i_].start, ap, granule);                      \
            switch_physmap_wcomb = false;                                                                             \
        }                                                                                                             \
    } while (0)

#define SWITCH_RAM_PHYSMAP_ATTR(attr) (switch_physmap_wcomb ? CACHE_ATTRINDX_WRITECOMB : (attr))

#endif
