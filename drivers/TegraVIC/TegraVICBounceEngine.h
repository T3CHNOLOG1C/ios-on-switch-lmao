// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel half of the copy-engine bounce (TegraVICBounce.cpp), for the engine thread's jobs.
#ifndef TEGRA_VIC_BOUNCE_ENGINE_H
#define TEGRA_VIC_BOUNCE_ENGINE_H
#include <IOKit/IOTypes.h>
#include "TegraVICRequest.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

class IOMemoryDescriptor;

// One side's scratch: memory kept from job to job and grown when a surface needs more, and this job's geometry in it.
struct TegraVICBounce {
    switch_dma_pages scratch;
    TegraVICSurfaceDesc desc;
    uint32_t pages;  // the scratch pages this job maps
};

// The scratch for surface `d` at pitches that are multiples of `align`.
IOReturn tegra_vic_bounce_stage(TegraVICBounce *b, const TegraVICSurfaceDesc *d, uint32_t align);
// Every plane of `from` (geometry fd) into `to` (td) on the GPU's copy engine; returns once all have landed, woken by
// the copies' completion (a lost one panics). Both descriptors stay prepared until it returns.
IOReturn tegra_vic_bounce_copy(IOMemoryDescriptor *from, const TegraVICSurfaceDesc *fd, IOMemoryDescriptor *to,
                               const TegraVICSurfaceDesc *td);
#endif
