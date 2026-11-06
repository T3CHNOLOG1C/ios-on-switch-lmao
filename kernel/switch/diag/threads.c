// SPDX-License-Identifier: GPL-2.0-only
#include <kern/thread.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/misc_protos.h>
#include <kern/debug.h>
#include <mach/vm_param.h>
#include <arm/cpu_data_internal.h>
#include <libkern/OSKextLibPrivate.h>
#include <libkern/kernel_mach_header.h>
#include <vm/vm_kern.h>

#include <switch/diag.h>

/*
 * Switch: every blocked thread whose kernel stack runs through a kext, as
 * "T<tid> h<block hint> <kext> [owner T<tid>]" plus its saved return addresses
 * below the context switch (unslid, low 36 bits). For a kernel-mutex wait the
 * owner comes from kdp_lck_mtx_find_owner, and each owner's own stack follows.
 */
static unsigned switch_thread_frames(thread_t thread, uint64_t *frames, unsigned max)
{
    const vm_offset_t low = thread->kernel_stack;
    const vm_offset_t high = low + kernel_stack_size;
    arm_kernel_saved_state_t *saved = &thread_get_kernel_state(thread)->machine.ss;
    unsigned count = 0;
    uint64_t fp = saved->fp;
    frames[count++] = saved->lr;
    while (count < max && fp >= low && fp + 16 <= high && !(fp & 0xf)) {
        frames[count++] = ((uint64_t *)fp)[1];
        fp = ((uint64_t *)fp)[0];
    }
    return count;
}

static void switch_print_frames(const uint64_t *frames, unsigned count)
{
    printf("   ");
    for (unsigned i = 2; i < count && i < 14; i++) {
        printf(" %llx", (uint64_t)VM_KERNEL_UNSLIDE(frames[i]) & 0xfffffffffULL);
    }
    printf("\n");
}

void switch_print_blocked_kext_threads(const char *(*name_of)(const void *), unsigned max_threads)
{
    unsigned printed = 0;
    unsigned blocked = 0;
    unsigned owners = 0;
    uint64_t owner_ids[4];
    thread_t thread = NULL;
    lck_mtx_lock(&tasks_threads_lock);
    queue_iterate(&threads, thread, thread_t, threads)
    {
        if (thread == current_thread() || !thread->kernel_stack || !((unsigned int)thread->state & TH_WAIT)) {
            continue;
        }
        blocked++;
        uint64_t frames[32];
        unsigned count = switch_thread_frames(thread, frames, 32);
        /* OSKextKextForAddress returns the kext's load address, or the kernel's header. */
        unsigned deepest = count;
        for (unsigned i = 0; i < count; i++) {
            void *image = OSKextKextForAddress((const void *)frames[i]);
            if (image && image != (void *)&_mh_execute_header) {
                deepest = i;
            }
        }
        if (deepest == count || printed == max_threads) {
            continue;
        }
        const char *name = name_of((const void *)frames[deepest]);
        printf("T%llu h%d %s", thread->thread_id, thread->block_hint, name ? name : "?");
        if (thread->block_hint == kThreadWaitKernelMutex) {
            thread_waitinfo_t info = {};
            kdp_lck_mtx_find_owner(thread->waitq.wq_q, thread->wait_event, &info);
            printf(" owner T%llu", info.owner);
            if (owners < 4) {
                owner_ids[owners++] = info.owner;
            }
        }
        printf("\n");
        switch_print_frames(frames, count);
        printed++;
    }
    for (unsigned o = 0; o < owners; o++) {
        queue_iterate(&threads, thread, thread_t, threads)
        {
            if (thread->thread_id != owner_ids[o]) {
                continue;
            }
            printf("owner T%llu state=%x h%d run=%d\n", thread->thread_id, (unsigned)thread->state, thread->block_hint,
                   thread->last_processor ? thread->last_processor->cpu_id : -1);
            if (thread->kernel_stack && thread != current_thread()) {
                uint64_t frames[32];
                switch_print_frames(frames, switch_thread_frames(thread, frames, 32));
            }
        }
    }
    lck_mtx_unlock(&tasks_threads_lock);
    printf("blocked threads with a stack: %u, shown: %u\n", blocked, printed);
}
