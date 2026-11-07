// SPDX-License-Identifier: BSD-2-Clause-Patent
// The copy-engine bounce (TegraVICBounce.h): scratch memory kept across jobs and the copies into and out of it. Only the
// engine thread calls these, one job at a time.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include "TegraVICBounce.h"
#include "TegraVICBounceEngine.h"
#include "../TegraGM20B/TegraGM20BCopy.h"

namespace {
constexpr uint32_t kLandedWithinMs = 2000;  // a whole 128 MiB window copies in well under 100 ms

struct Landing {
    IOLock *lock;
    uint32_t pending;
    int status;
};

IOLock *gLock;

void landed(void *arg, int status)
{
    auto *landing = static_cast<Landing *>(arg);
    IOLockLock(landing->lock);
    if (status && !landing->status)
        landing->status = status;
    if (--landing->pending == 0)
        IOLockWakeup(landing->lock, landing, false);
    IOLockUnlock(landing->lock);
}

IOReturn ioreturnOf(int err)
{
    return err == -EBUSY    ? kIOReturnBusy
           : err == -ENOMEM ? kIOReturnNoMemory
           : err            ? kIOReturnIOError
                            : kIOReturnSuccess;
}
}  // namespace

IOReturn tegra_vic_bounce_stage(TegraVICBounce *b, const TegraVICSurfaceDesc *d, uint32_t align)
{
    if (!gLock && !(gLock = IOLockAlloc()))
        return kIOReturnNoMemory;
    tegra_vic_bounce_desc(d, align, &b->desc);
    if (b->scratch.size < b->desc.length) {
        switch_dma_pages_free(&b->scratch);
        if (!switch_dma_pages_alloc(&b->scratch, (size_t)b->desc.length))
            return kIOReturnNoMemory;
    }
    b->pages = (uint32_t)(b->desc.length / TEGRA_VIC_BOUNCE_PAGE);
    return kIOReturnSuccess;
}

IOReturn tegra_vic_bounce_copy(IOMemoryDescriptor *from, const TegraVICSurfaceDesc *fd, IOMemoryDescriptor *to,
                               const TegraVICSurfaceDesc *td)
{
    const TegraVICFormat *f = tegra_vic_format(fd->fourcc);
    Landing landing = {gLock, 0, 0};
    const tegra_gm20b_ce_done done = {TEGRA_GM20B_CE_NO_SYNCPT, landed, &landing};
    int err = 0;
    for (uint32_t p = 0; p < f->planes && !err; p++) {
        const tegra_gm20b_ce_plane src = {from, fd->planeOffset[p], (uint32_t)fd->planeBytesPerRow[p]};
        const tegra_gm20b_ce_plane dst = {to, td->planeOffset[p], (uint32_t)td->planeBytesPerRow[p]};
        IOLockLock(gLock);
        landing.pending++;
        IOLockUnlock(gLock);
        err = tegra_gm20b_ce_copy_plane(&src, &dst, tegra_vic_bounce_row_bytes(f, fd->width, p),
                                        (uint32_t)fd->planeHeight[p], nullptr, &done);
        if (err) {
            IOLockLock(gLock);
            landing.pending--;
            IOLockUnlock(gLock);
        }
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(kLandedWithinMs, kMillisecondScale, &deadline);
    IOLockLock(gLock);
    while (landing.pending)
        if (IOLockSleepDeadline(gLock, &landing, deadline, THREAD_UNINT) == THREAD_TIMED_OUT && landing.pending)
            panic("TegraVIC: copy-engine bounce lost its completion: %u plane copies outstanding after %u ms",
                  landing.pending, kLandedWithinMs);
    IOLockUnlock(gLock);
    return err ? ioreturnOf(err) : ioreturnOf(landing.status);
}
