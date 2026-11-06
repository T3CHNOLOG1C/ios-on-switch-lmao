// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lazy IOSurface coherency (GPU <-> CPU). On Tegra the GPU does not snoop the CPU caches. The TegraGM20B GPU driver
 * registers the backend below (tegra_coherency_register, a down-call from the driver at start). It keeps, per imported
 * surface (keyed by its first physical page), whether the GPU may have written it since the last CPU read lock. Here,
 * at the surface's IOSurface lock/unlock (whoever touches it on the CPU, in any process), the backend does the one cache
 * op that direction needs:
 *   - a CPU lock invalidates the surface's lines where the GPU wrote it (so the read sees DRAM, not lines the CPU cached
 *     before: a fresh surface's zeros baked the 16x1 black runs into Settings' launch snapshot, rendered on the GPU and
 *     read back on the CPU);
 *   - a CPU write unlock cleans the surface's lines (so the GPU reads what the CPU wrote).
 * Surfaces the GPU never imported, and surfaces the CPU never locks (the framebuffer), pay nothing; the GPU render path
 * pays nothing. The backend returns the bytes it touched, for its own 5 s counters (tegra-gm20b: coherency).
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <string.h>

#include <switch/iokit.h>

struct switch_iosurface_coherency g_tegra_coh;

void tegra_coherency_register(uint64_t (*read_lock)(uint64_t), uint64_t (*write_unlock)(uint64_t))
{
    g_tegra_coh.read_lock = read_lock;
    g_tegra_coh.write_unlock = write_unlock;
}

/*
 * The first physical page of the IOSurface an IOSurfaceRootUserClient lock/unlock names, and 1 in `readOnly` when the
 * lock is read-only. No IOSurface struct offsets: the lock's first input is the surface's global ID, which is also the
 * memory type userspace maps the surface by (IOConnectMapMemory64(conn, surfaceID)). clientMemoryForType() is the
 * IOUserClient virtual behind that map; here it returns the surface's backing IOMemoryDescriptor, whose first physical
 * segment is the page-aligned base the GPU driver imported and keyed its registry by. The descriptor is returned with a
 * reference this caller releases (as IOUserClient::mapClientMemory64 does). Returns 0 (nothing done) if the surface has
 * no such memory; refuses once-logged on an unexpected result. The ID and options are read from scalar inputs when
 * present, else from the inband structure, so either method ABI works.
 */
static uint64_t tegra_iosurface_lock_pa(IOUserClient *client, IOExternalMethodArguments *args, int *readOnly)
{
    *readOnly = 0;
    if (!client || !args) {
        return 0;
    }
    // The surface ID (input 0) and the lock options (input 1), whichever way the method takes them.
    uint64_t surfaceID = 0, options = 0;
    if (args->scalarInputCount >= 1) {
        surfaceID = args->scalarInput[0];
        if (args->scalarInputCount >= 2) {
            options = args->scalarInput[1];
        }
    } else if (args->structureInput && args->structureInputSize >= sizeof(uint32_t)) {
        const uint32_t *in = (const uint32_t *)args->structureInput;
        surfaceID = in[0];
        if (args->structureInputSize >= 2 * sizeof(uint32_t)) {
            options = in[1];
        }
    } else {
        return 0;
    }
    *readOnly = (options & kTegraIOSurfaceLockReadOnly) ? 1 : 0;

    IOOptionBits opts = 0;
    IOMemoryDescriptor *memory = NULL;
    IOReturn err = client->clientMemoryForType((UInt32)surfaceID, &opts, &memory);
    if ((kIOReturnSuccess != err) || !memory) {
        static bool said;
        if (!said) {
            said = true;
            IOLog("tegra: IOSurface coherency: clientMemoryForType(%u) returned 0x%x; coherency op skipped\n",
                  (unsigned)surfaceID, (unsigned)err);
        }
        return 0;
    }
    IOByteCount length = 0;
    addr64_t pa = memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
    memory->release();  // the reference clientMemoryForType handed us
    return pa ? (uint64_t)(pa & ~0xfffULL) : 0;
}

void switch_iokit_iosurface_coherency(IOUserClient *client, uint32_t selector, IOExternalMethodArguments *args)
{
    const OSMetaClass *meta = client->getMetaClass();
    const char *cls = meta ? meta->getClassName() : NULL;
    if (cls && 0 == strcmp(cls, "IOSurfaceRootUserClient")) {
        int readOnly = 0;
        uint64_t pa = tegra_iosurface_lock_pa(client, args, &readOnly);
        if (pa) {
            if (selector == kTegraIOSurfaceLock) {
                g_tegra_coh.read_lock(pa);  // GPU -> CPU: fresh bytes before the CPU reads
            } else if (!readOnly) {
                g_tegra_coh.write_unlock(pa);  // CPU -> GPU: the CPU's writes reach the GPU
            }
        }
    }
}
