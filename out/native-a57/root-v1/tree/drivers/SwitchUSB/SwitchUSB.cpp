// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB on the USB-C port: start-up and the role manager (see SwitchUSB.h).
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSData.h>
#include "../SwitchBattery/SwitchBattery.h"
#include "../TegraI2C/TegraI2C.h"
#include "SwitchPD.h"
#include "SwitchUSB.h"

extern "C" __attribute__((used)) const unsigned long long ioswitch_prelink_cut_null = 0;

namespace {
IOService *gDevicePort;
bool gHostPackaged;

// Role manager: the PD listener records what the port wants; a thread call (it may sleep: I2C, kext loads, firmware)
// makes it so. Everything below gLock is touched only with it held, the apply step only from the thread call.
IOLock *gLock;
thread_call_t gRoleCall;
bool gWantHost, gHostCable;            // from the last PD state
bool gHostUp, gHostBroken, gHostRole;  // what has been done

// avddio_usb (OLED DTB xudc@700d0000 / xusb@70090000) is MAX77620 LDO1 at 1.05 V, shared with PCIe (drivers/SwitchWLAN
// TegraPCIe rail(): same register, same value); normally on already. hvdd_usb (3.3 V) and avdd_pll_utmip (SD3, 1.8 V)
// are always on.
bool railOn(char *why, size_t whySize)
{
    constexpr uint8_t kPmic = 0x3C, kLdo1Cfg = 0x25, kModeNormal = 3u << 6, kLdo1_1V05 = (1050000 - 800000) / 25000;
    uint8_t value = 0;
    int err = tegra_i2c_init(5);
    if (!err) err = tegra_i2c_read(5, kPmic, kLdo1Cfg, &value, 1);
    if (!err && !(value & kModeNormal)) {
        const uint8_t on = kModeNormal | kLdo1_1V05;
        err = tegra_i2c_write(5, kPmic, kLdo1Cfg, &on, 1);
        IOSleep(1);
    }
    if (err) snprintf(why, whySize, "PMIC LDO1 (avddio_usb) not readable: I2C5 error %d", err);
    return !err;
}

// SwitchPD's work loop: short. A sink on the port (an OTG device) wants the host role; a cable from a host while we
// are its device means a bus reset is due.
void pdChanged(void *, const SwitchPDState *state)
{
    IOLockLock(gLock);
    const bool wantHost = gHostPackaged && state->otgDevice;
    const bool hostCable = state->plugged && !state->sourcing && state->vbusPresent && !state->dataHost;
    const bool changed = wantHost != gWantHost || (hostCable && !gHostCable);
    gWantHost = wantHost;
    gHostCable = hostCable;
    IOLockUnlock(gLock);
    if (changed) thread_call_enter(gRoleCall);
}

// Thread call (THREAD_CALL_OPTIONS_ONCE: never two at once; entered again while it runs, it runs again after).
void applyRole(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    const bool wantHost = gWantHost, hostCable = gHostCable;
    IOLockUnlock(gLock);
    if (wantHost && !gHostRole && !gHostBroken) {
        char why[160] = "";
        if (!gHostUp && !(gHostUp = switchxhci_start(why, sizeof(why)))) {
            IOLog("SwitchUSB: host mode off: %s\n", why);
            gHostBroken = true;
            return;
        }
        switchxudc_set_host_owns_port(true);
        if (switch_battery_set_otg_boost(true)) {
            switchxudc_set_host_owns_port(false);
            return;
        }
        switchxhci_set_host_role(true);
        gHostRole = true;
        IOLog("SwitchUSB: host role (a device on the port, VBUS on)\n");
        switchxhci_expect_port_change();
    } else if (!wantHost && gHostRole) {
        switchxhci_set_host_role(false);
        (void)switch_battery_set_otg_boost(false);
        switchxudc_set_host_owns_port(false);
        gHostRole = false;
        IOLog("SwitchUSB: device role\n");
    }
    if (hostCable && !gHostRole) switchxudc_host_expected();
}

void usbThread(void *, wait_result_t)
{
    char why[160] = "";
    if (gDevicePort) {
        if (railOn(why, sizeof(why)) && switchxudc_start(gDevicePort, why, sizeof(why))) {
            const OSData *configuration = OSDynamicCast(OSData, gDevicePort->getProperty("configuration-string"));
            IOLog("SwitchUSB: device mode ready (XUDC at 0x700d0000, configuration %s)\n",
                  configuration && configuration->getLength() ? (const char *)configuration->getBytesNoCopy() : "?");
        } else {
            IOLog("SwitchUSB: device mode off: %s\n", why);
        }
    }
    if (gHostPackaged) {
        if (!gDevicePort && !railOn(why, sizeof(why))) {
            IOLog("SwitchUSB: host mode off: %s\n", why);
            gHostBroken = true;
        } else if (switch_pd_enable_source()) {
            IOLog("SwitchUSB: host mode off: the PD controller cannot source VBUS\n");
            gHostBroken = true;
        }
    }
    // The current state comes at once; the role thread call does the rest.
    if (switch_pd_add_listener(&pdChanged, nullptr) && gHostPackaged && !gHostBroken)
        IOLog("SwitchUSB: host mode off: no PD controller\n");
}  // returning ends the thread (Call_continuation -> thread_terminate)

IOService *publishedNode(const char *path)
{
    // ArmDTPlatform publishes every /arm-io child as an IOPlatformDevice that takes the node's place in the device
    // tree plane, so the path names the nub itself.
    IORegistryEntry *node = IORegistryEntry::fromPath(path, gIODTPlane);
    IOService *nub = OSDynamicCast(IOService, node);
    if (node && !nub) {
        IOLog("SwitchUSB: %s was not published\n", path);
        node->release();
    }
    return nub;
}
}  // namespace

extern "C" int switch_usb_start(void)
{
    static uint32_t started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return 0;
    gDevicePort = publishedNode("/arm-io/usb-device");
    if (IOService *host = publishedNode("/arm-io/usb-xhci")) {
        gHostPackaged = true;
        host->release();
    }
    if (!gDevicePort && !gHostPackaged) return 0;  // not packaged with USB
    gLock = IOLockAlloc();
    gRoleCall = gLock ? thread_call_allocate_with_options(&applyRole, nullptr, THREAD_CALL_PRIORITY_KERNEL,
                                                          THREAD_CALL_OPTIONS_ONCE)
                      : nullptr;
    thread_t thread = THREAD_NULL;
    if (!gRoleCall || kernel_thread_start(&usbThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("SwitchUSB: device mode off: no thread\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
