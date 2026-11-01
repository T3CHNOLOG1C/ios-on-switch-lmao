// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pinned iPad7,1 21H461 AppleARMWatchdogTimer ABI. This is not IOWatchDogTimer.
#pragma once
#include <IOKit/IOService.h>

class IOWatchdog : public IOService {
    OSDeclareAbstractStructors(IOWatchdog)
        // Original private fields are initialized/used only by original methods.
        unsigned char originalState[0xf8 - sizeof(IOService)];

public:
    void stop(IOService *) override;
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) override;
    virtual void enableWatchdog() = 0;   // original slot 168 / 0x540
    virtual void disableWatchdog() = 0;  // original slot 169 / 0x548
    virtual void petWatchdog() = 0;      // original slot 170 / 0x550
};
static_assert(sizeof(IOService) == 0x88, "Pinned original IOService base size");
static_assert(sizeof(IOWatchdog) == 0xf8, "Pinned original IOWatchdog object size");
extern "C" bool ioswitch_original_watchdog_start(IOWatchdog *, IOService *, void *, UInt32);
extern "C" void ioswitch_original_watchdog_check(IOWatchdog *);
