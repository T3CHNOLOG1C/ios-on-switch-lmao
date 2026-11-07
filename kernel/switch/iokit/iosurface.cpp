// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOSurface coherency (GPU <-> CPU). On Tegra the GPU does not snoop the CPU caches. At every process's IOSurfaceLock
 * and IOSurfaceUnlock (IOSurfaceRootUserClient traps 2 and 3), the TegraGM20B driver does the one cache op that
 * direction needs on the surfaces it imported (tegra_gm20b_iosurface_trap, TegraGM20BCoherency.cpp): a lock after GPU
 * writes invalidates, an unlock that wrote cleans (before IOSurface's unlock moves the seed). Before a lock runs, the driver waits for the GPU's pending writes to
 * the surface (tegra_gm20b_iosurface_lock_will). Surfaces the GPU never imported cost a class compare and a hash
 * lookup.
 */
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <os/atomic_private.h>
#include <string.h>

#include <switch/iokit.h>

extern "C" void tegra_gm20b_iosurface_trap(uint32_t index, uint32_t surface_id, uint32_t options) __attribute__((weak));
extern "C" int tegra_gm20b_iosurface_lock_will(uint32_t surface_id, uint32_t options) __attribute__((weak));

static bool surface_client(IOUserClient *client)
{
    static const OSMetaClass *surfaceClass;
    const OSMetaClass *meta = client->getMetaClass();

    if (meta != os_atomic_load(&surfaceClass, relaxed)) {
        if (os_atomic_load(&surfaceClass, relaxed) || !meta ||
            strcmp(meta->getClassName(), "IOSurfaceRootUserClient") != 0) {
            return false;
        }
        os_atomic_store(&surfaceClass, meta, relaxed);
    }
    return true;
}

void switch_iokit_iosurface_trap(IOUserClient *client, uint32_t index, void *p1, void *p2)
{
    if (tegra_gm20b_iosurface_trap && surface_client(client)) {
        tegra_gm20b_iosurface_trap(index, (uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2);
    }
}

/* Apple's lock synchronises with the GPU: it waits for pending GPU writes, and with kIOSurfaceLockAvoidSync fails
 * with kIOReturnCannotLock instead of waiting. */
kern_return_t switch_iokit_iosurface_lock_will(IOUserClient *client, void *p1, void *p2)
{
    if (tegra_gm20b_iosurface_lock_will && surface_client(client) &&
        tegra_gm20b_iosurface_lock_will((uint32_t)(uintptr_t)p1, (uint32_t)(uintptr_t)p2)) {
        return kIOReturnCannotLock;
    }
    return KERN_SUCCESS;
}
