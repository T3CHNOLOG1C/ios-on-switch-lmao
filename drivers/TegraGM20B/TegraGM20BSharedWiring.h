// SPDX-License-Identifier: MIT
// One wiring of an IOSurface's pages per task, shared by every connection of that task that imports the same range (the
// render server's raw path and NVK each hold a connection, and a raw texture's m2v twin imports the surface again). The
// first import wires the range, lists its pages and makes the GPU memory object; later ones take a reference, and
// the last release unwires. Each import keeps its own binds, coherency reference and zombie: only the pages and the
// memory object are shared.
#ifndef TEGRA_GM20B_SHARED_WIRING_H
#define TEGRA_GM20B_SHARED_WIRING_H
#include <IOKit/IOMemoryDescriptor.h>
#include <mach/mach_types.h>
#include <stdint.h>
#include "TegraGM20BGpu.h"

struct SharedWiring {
	SharedWiring *next;
	task_t task;
	uint64_t address, bytes;
	uint32_t surfaceId, refs;  // table lock
	IOMemoryDescriptor *wired;
	uint64_t *pas;  // one per 4 KiB page; mem points into it
	uint32_t npages;
	tgm_mem *mem;
	bool readOnly;
};

// The task's live wiring of exactly [address, address + bytes) for surfaceId, while the task still maps its first and
// last pages there, with a reference for the caller; nullptr: none.
SharedWiring *sharedWiringFind(task_t task, uint64_t address, uint64_t bytes, uint32_t surfaceId);

// A wiring the caller just made (refs 0), published with a reference for it. When another import published the same
// range meanwhile, that one is returned with the reference and `made` is freed.
SharedWiring *sharedWiringPublish(SharedWiring *made);

// Drops a reference; the last one deletes the memory object, unwires the pages and frees the list.
void sharedWiringPut(SharedWiring *w);

#endif
