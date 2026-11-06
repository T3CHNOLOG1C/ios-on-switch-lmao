// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchXHCI's IOKit plumbing that needs nothing of AppleUSBXHCI's layout: the host stack's resource personalities, the
// provider nub, and the USB-C connector's root port.
#pragma once
#include <IOKit/IOReturn.h>
#include <stdint.h>

class IOService;
class OSMetaClass;

namespace xhost {

const OSMetaClass *lookupClass(const char *name);

// An IOResources personality ("IOResourceMatch" IOKit), as in the kext's Info.plist, into the catalogue (matched now).
bool addResourcePersonality(const char *ioClass, const char *category, const char *bundle);

// Provider nub: register window index 0 and `intid` as interrupt index 0 (the GIC nub's own interrupt properties, so
// IOService resolves it through ArmGIC). Retained; nullptr on failure.
IOService *makeNub(uint64_t registers, uint64_t bytes, uint32_t intid);

// AppleUSBXHCI's root port for xHCI port `number` once it is registered (its setProperties takes nothing before):
// the AppleUSBHostPort under `controller` whose "port" (kUSBHostPortPropertyPortNumber, AppleUSBXHCIPort's 4-byte
// OSData) is that number. Waits up to `timeoutNS` (thread context). Retained; nullptr if none registered in time.
IOService *waitForRootPort(IOService *controller, unsigned number, uint64_t timeoutNS);

// A cable in (on) or out on the root port's connector, as an iPad's port controller reports it: AppleUSBHostPort's
// UsbForcePower (IOUSBHostFamilyDefinitions.h kUSBHostPropertyForcePower) runs the port's own forcePower, the one its
// cableChangeOccurred and the removable and Type-C port classes call. On: the port and its controller held On until
// a device attaches (then the device's needs set the power); off: held Off. Thread context (the port's command gate).
IOReturn forcePortPower(IOService *port, bool on);

}  // namespace xhost
