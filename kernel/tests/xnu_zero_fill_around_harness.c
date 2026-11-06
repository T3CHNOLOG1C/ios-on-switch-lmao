/*
 * Host harness for XNU osfmk/vm/vm_fault.c switch_zero_fill_around (r17-faults): the zero-fill fault-around that
 * makes one fault enter the rest of its 16 KiB group. The function is compiled from the XNU source as is (BUILD.bazel
 * cuts it out of @xnu's vm_fault.c into fault_around.h) against the stubs below, which model one VM object, its
 * resident pages and the pmap, and record every page the function allocates and enters.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The model's invariants (what the kernel guarantees the function), checked in every build: never compiled out. */
#define MODEL_CHECK(cond)                                                \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d model: %s\n", __FILE__, __LINE__, #cond); \
            exit(1);                                                     \
        }                                                                \
    } while (0)

typedef uint64_t vm_object_offset_t;
typedef uint64_t vm_map_offset_t;
typedef int vm_prot_t;
typedef int kern_return_t;
typedef int boolean_t;

#define PAGE_SIZE               4096ULL
#define FALSE                   0
#define TRUE                    1
#define KERN_SUCCESS            0
#define KERN_RESOURCE_SHORTAGE  6
#define KERN_CODESIGN_ERROR     50
#define VM_PROT_READ            1
#define VM_PROT_WRITE           2
#define VM_PROT_EXECUTE         4
#define VM_PURGABLE_DENY        3
#define VM_PURGABLE_NONVOLATILE 0
#define OBJECT_LOCK_EXCLUSIVE   2
#define DBG_ZERO_FILL_FAULT     1
#define VM_KERN_MEMORY_NONE     0
#define VM_OBJECT_NULL          ((vm_object_t)0)
#define VM_PAGE_NULL            ((vm_page_t)0)

struct vm_object {
    bool internal, pager_created, all_reusable, blocked_access, phys_contiguous, kernel;
    struct vm_object *shadow, *vo_copy;
    int purgable;
    vm_object_offset_t vo_size;
};
typedef struct vm_object *vm_object_t;

struct vm_page {
    vm_object_offset_t offset;
    bool vmp_busy, vmp_pmapped, zeroed, used;
};
typedef struct vm_page *vm_page_t;

struct vm_object_fault_info {
    vm_object_offset_t lo_offset, hi_offset;
};
typedef struct vm_object_fault_info *vm_object_fault_info_t;

struct pmap {
    int unused;
};
typedef struct pmap *pmap_t;
static struct pmap kernel_pmap_store, user_pmap_store;
#define kernel_pmap (&kernel_pmap_store)

/* the model: resident offsets, the pages the function allocated, what it entered and where */
#define MAX_PAGES 64
static struct vm_page pages[MAX_PAGES];
static unsigned int npages;
static vm_object_offset_t resident[MAX_PAGES];
static unsigned int nresident;
static vm_map_offset_t entered_va[MAX_PAGES];
static vm_object_offset_t entered_off[MAX_PAGES];
static unsigned int nentered;
static unsigned int zero_fills;
static unsigned int vm_page_free_count = 100000, vm_page_free_target = 4000;
static bool dynamic_paging = true;
static unsigned int alloc_budget = 100; /* vm_page_alloc fails after this many */
static int enter_result = KERN_SUCCESS;
static bool enter_retry; /* vm_fault_enter reports a pmap_enter that would block */
static unsigned int enter_fail_after = 100;
static bool lock_held = true;

static struct {
    int n;
} vm_statistics_zero_fill_count;

#define VM_DYNAMIC_PAGING_ENABLED()        (dynamic_paging)
#define is_kernel_object(o)                ((o)->kernel)
#define vm_object_lock_assert_exclusive(o) MODEL_CHECK(lock_held && (o) != VM_OBJECT_NULL)
#define counter_inc(c)                     ((c)->n++)
#define DTRACE_VM2(a, b, c, d, e) \
    do {                          \
    } while (0)
#define PAGE_WAKEUP_DONE(m) ((m)->vmp_busy = false)

static vm_page_t vm_page_lookup(vm_object_t object, vm_object_offset_t off)
{
    (void)object;
    MODEL_CHECK(off % PAGE_SIZE == 0);
    for (unsigned int i = 0; i < nresident; i++) {
        if (resident[i] == off) {
            return &pages[MAX_PAGES - 1]; /* any non-NULL page: the function only tests for NULL */
        }
    }
    return VM_PAGE_NULL;
}

static vm_page_t vm_page_alloc(vm_object_t object, vm_object_offset_t off)
{
    MODEL_CHECK(object->internal && lock_held && off < object->vo_size);
    MODEL_CHECK(vm_page_lookup(object, off) == VM_PAGE_NULL);
    if (alloc_budget == 0 || npages >= MAX_PAGES - 1) {
        return VM_PAGE_NULL;
    }
    alloc_budget--;
    vm_page_t m = &pages[npages++];
    memset(m, 0, sizeof(*m));
    m->offset = off;
    m->vmp_busy = true;
    m->used = true;
    resident[nresident++] = off;
    return m;
}

static void vm_fault_cs_clear(vm_page_t m)
{
    (void)m;
}

static void vm_page_zero_fill(vm_page_t m)
{
    MODEL_CHECK(m->vmp_busy);
    m->zeroed = true;
    zero_fills++;
}

static kern_return_t vm_fault_enter(vm_page_t m, pmap_t pmap, vm_map_offset_t va, uint64_t fault_page_size,
                                    uint64_t fault_phys_offset, vm_prot_t prot, vm_prot_t caller_prot, boolean_t wired,
                                    boolean_t change_wiring, int wire_tag, vm_object_fault_info_t fault_info,
                                    boolean_t *need_retry, int *type_of_fault, uint8_t *object_lock_type)
{
    (void)prot;
    (void)caller_prot;
    (void)wire_tag;
    (void)fault_info;
    MODEL_CHECK(m->vmp_busy && m->zeroed && m->vmp_pmapped && pmap != kernel_pmap);
    MODEL_CHECK(fault_page_size == PAGE_SIZE && fault_phys_offset == 0 && !wired && !change_wiring);
    MODEL_CHECK(need_retry != NULL && *need_retry == FALSE); /* never a blocking pmap_enter under our locks */
    MODEL_CHECK(*type_of_fault == DBG_ZERO_FILL_FAULT && *object_lock_type == OBJECT_LOCK_EXCLUSIVE);
    MODEL_CHECK(va % PAGE_SIZE == 0);
    if (nentered >= enter_fail_after) {
        if (enter_retry) {
            *need_retry = TRUE;
            return KERN_RESOURCE_SHORTAGE;
        }
        return enter_result;
    }
    entered_va[nentered] = va;
    entered_off[nentered] = m->offset;
    nentered++;
    return KERN_SUCCESS;
}

#include "fault_around.h"

static struct vm_object object;
static struct vm_object_fault_info info;

static void reset(vm_object_offset_t size, vm_object_offset_t lo, vm_object_offset_t hi)
{
    memset(&object, 0, sizeof(object));
    object.internal = true;
    object.purgable = VM_PURGABLE_DENY;
    object.vo_size = size;
    info.lo_offset = lo;
    info.hi_offset = hi;
    npages = nresident = nentered = zero_fills = 0;
    vm_page_free_count = 100000;
    dynamic_paging = true;
    alloc_budget = 100;
    enter_result = KERN_SUCCESS;
    enter_retry = false;
    enter_fail_after = 100;
}

/* the faulting page (offset off, address va) is resident already: the fault entered it */
static unsigned int fault(vm_object_offset_t off, vm_map_offset_t va, vm_prot_t prot)
{
    resident[nresident++] = off;
    return switch_zero_fill_around(&object, off, &user_pmap_store, va, prot, VM_PROT_WRITE, &info);
}

static int cases;

#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                             \
        }                                                         \
    } while (0)

static bool entered(vm_map_offset_t va, vm_object_offset_t off)
{
    for (unsigned int i = 0; i < nentered; i++) {
        if (entered_va[i] == va && entered_off[i] == off) {
            return !pages[i].vmp_busy;
        }
    }
    return false;
}

int main(void)
{
    const vm_prot_t rw = VM_PROT_READ | VM_PROT_WRITE;
    const vm_map_offset_t base = 0x10000c000ULL; /* a 16 KiB-aligned group */

    /* first page of a group in a large entry: the other three, at the matching offsets */
    reset(1 << 20, 0, 1 << 20);
    CHECK(fault(0x8000, base, rw) == 3);
    CHECK(entered(base + 0x1000, 0x9000) && entered(base + 0x2000, 0xa000) && entered(base + 0x3000, 0xb000));
    CHECK(zero_fills == 3 && vm_statistics_zero_fill_count.n >= 3);
    cases++;

    /* last page of a group: the three below it */
    reset(1 << 20, 0, 1 << 20);
    CHECK(fault(0xb000, base + 0x3000, rw) == 3);
    CHECK(entered(base, 0x8000) && entered(base + 0x1000, 0x9000) && entered(base + 0x2000, 0xa000));
    cases++;

    /* an entry that starts inside the group (object offset 0x5000 at base + 0x1000): never below it */
    reset(1 << 20, 0x5000, 1 << 20);
    CHECK(fault(0x6000, base + 0x2000, rw) == 2);
    CHECK(entered(base + 0x1000, 0x5000) && entered(base + 0x3000, 0x7000) && !entered(base, 0x4000));
    cases++;

    /* object offset 0 at the group's third page: the two pages below would be negative offsets */
    reset(1 << 20, 0, 1 << 20);
    CHECK(fault(0, base + 0x2000, rw) == 1);
    CHECK(entered(base + 0x3000, 0x1000) && nentered == 1);
    cases++;

    /* the entry ends inside the group (hi_offset), and the object ends inside the group (vo_size) */
    reset(1 << 20, 0, 0xa000);
    CHECK(fault(0x8000, base, rw) == 1 && entered(base + 0x1000, 0x9000));
    reset(0x9000, 0, 1 << 20);
    CHECK(fault(0x8000, base, rw) == 0 && nentered == 0);
    cases++;

    /* resident neighbours stay as they are; the rest of the group is filled */
    reset(1 << 20, 0, 1 << 20);
    resident[nresident++] = 0x9000;
    CHECK(fault(0x8000, base, rw) == 2 && entered(base + 0x2000, 0xa000) && entered(base + 0x3000, 0xb000));
    CHECK(!entered(base + 0x1000, 0x9000));
    cases++;

    /* not the plain case (shadow, copy object, pager, purgeable, all reusable, kernel object, kernel pmap, executable,
	 * no dynamic paging): nothing */
    for (unsigned int k = 0; k < 9; k++) {
        static struct vm_object other;
        pmap_t pmap = &user_pmap_store;
        vm_prot_t prot = rw;

        reset(1 << 20, 0, 1 << 20);
        switch (k) {
        case 0:
            object.shadow = &other;
            break;
        case 1:
            object.vo_copy = &other;
            break;
        case 2:
            object.pager_created = true;
            break;
        case 3:
            object.purgable = VM_PURGABLE_NONVOLATILE;
            break;
        case 4:
            object.all_reusable = true;
            break;
        case 5:
            object.kernel = true;
            break;
        case 6:
            pmap = kernel_pmap;
            break;
        case 7:
            prot |= VM_PROT_EXECUTE;
            break;
        case 8:
            dynamic_paging = false;
            break;
        }
        resident[nresident++] = 0x8000;
        CHECK(switch_zero_fill_around(&object, 0x8000, pmap, base, prot, VM_PROT_WRITE, &info) == 0);
        CHECK(nentered == 0 && npages == 0);
        cases++;
    }

    /* free memory at the target: no extra pages */
    reset(1 << 20, 0, 1 << 20);
    vm_page_free_count = vm_page_free_target;
    CHECK(fault(0x8000, base, rw) == 0 && npages == 0);
    cases++;

    /* no free page: stop */
    reset(1 << 20, 0, 1 << 20);
    alloc_budget = 1;
    CHECK(fault(0x8000, base, rw) == 1 && npages == 1);
    cases++;

    /* a pmap_enter that would block: the page stays resident unmapped (unbusied) and the group ends there */
    reset(1 << 20, 0, 1 << 20);
    enter_fail_after = 1;
    enter_retry = true;
    CHECK(fault(0x8000, base, rw) == 1 && npages == 2 && !pages[1].vmp_busy);
    reset(1 << 20, 0, 1 << 20);
    enter_fail_after = 0;
    enter_result = KERN_CODESIGN_ERROR;
    CHECK(fault(0x8000, base, rw) == 0 && npages == 1 && !pages[0].vmp_busy);
    cases++;

    printf("PASS %d zero-fill fault-around cases\n", cases);
    return 0;
}
