// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOKit hooks (kernel/switch/iokit/). Included by iokit/Kernel/IOUserClient.cpp.
 */
#ifndef SWITCH_IOKIT_H
#define SWITCH_IOKIT_H

#include <stdint.h>

#ifdef __cplusplus
#include <mach/kern_return.h>

class IOUserClient;

/*
 * IOSurfaceLock and IOSurfaceUnlock are IOKit traps on IOSurfaceRootUserClient, not external methods: userland's
 * IOSurfaceClientLock/Unlock call IOConnectTrap3(connection, 2 or 3, surface ID, lock options, &seed).
 */
enum {
    kSwitchIOSurfaceTrapLock = 2,
    kSwitchIOSurfaceTrapUnlock = 3,
};

void switch_iokit_iosurface_trap(IOUserClient *client, uint32_t index, void *p1, void *p2);

/* iokit_user_client_trap, after the client's trap ran: an IOSurface lock or unlock gets the cache op it needs. */
static inline void switch_iokit_trap_done(IOUserClient *client, uint32_t index, void *p1, void *p2,
                                          kern_return_t result)
{
    if (result == KERN_SUCCESS && (index == kSwitchIOSurfaceTrapLock || index == kSwitchIOSurfaceTrapUnlock)) {
        switch_iokit_iosurface_trap(client, index, p1, p2);
    }
}
#endif

#endif
