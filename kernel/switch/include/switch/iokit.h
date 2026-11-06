// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOKit hooks (kernel/switch/iokit/). Included by iokit/Kernel/IOUserClient.cpp.
 */
#ifndef SWITCH_IOKIT_H
#define SWITCH_IOKIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* TegraGM20B's IOSurface coherency backend (kernel/switch/iokit/iosurface.cpp), registered once at its start. */
void tegra_coherency_register(uint64_t (*read_lock)(uint64_t), uint64_t (*write_unlock)(uint64_t));
#ifdef __cplusplus
}

#include <IOKit/IOReturn.h>

class IOUserClient;
struct IOExternalMethodArguments;

/*
 * The IOSurfaceRootUserClient external-method selectors for lock and unlock (21H461; confirmed against the userland
 * IOSurfaceClientLock/Unlock, which pass selector 2 and 3), and the read-only lock option (IOSurfaceTypes.h).
 */
enum {
    kTegraIOSurfaceLock = 2,
    kTegraIOSurfaceUnlock = 3,
    kTegraIOSurfaceLockReadOnly = 0x1
};

struct switch_iosurface_coherency {
    uint64_t (*read_lock)(uint64_t pa);
    uint64_t (*write_unlock)(uint64_t pa);
};
extern struct switch_iosurface_coherency g_tegra_coh;

void switch_iokit_iosurface_coherency(IOUserClient *client, uint32_t selector, IOExternalMethodArguments *args);

/* is_io_connect_method, after the client's method ran: an IOSurface lock or unlock gets the cache op it needs. */
static inline void switch_iokit_external_method_done(IOUserClient *client, uint32_t selector,
                                                     IOExternalMethodArguments *args, IOReturn ret)
{
    if (kIOReturnSuccess == ret && g_tegra_coh.read_lock &&
        (selector == kTegraIOSurfaceLock || selector == kTegraIOSurfaceUnlock)) {
        switch_iokit_iosurface_coherency(client, selector, args);
    }
}
#endif

#endif
