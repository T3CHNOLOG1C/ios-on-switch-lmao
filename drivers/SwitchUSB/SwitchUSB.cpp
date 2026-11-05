// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB on the USB-C port: start-up and the role manager (see SwitchUSB.h).
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSData.h>
#include <os/log.h>
#include <string.h>
#include "../SwitchBattery/SwitchBattery.h"
#include "../TegraI2C/TegraI2C.h"
#include "SwitchPD.h"
#include "SwitchUSB.h"
#include "UsbDmaMapper.h"

extern "C" __attribute__((used)) const unsigned long long switch_prelink_cut_null = 0;

namespace {
IOService *gDevicePort;
bool gHostPackaged;

// Data role manager: SwitchPD decides the power role (and owns VBUS); the PD listener records it, a thread call (it may
// sleep: kext loads, firmware) follows it with the data role. Everything below gLock is touched only with it held,
// the apply step only from the thread call.
IOLock *gLock;
thread_call_t gRoleCall, gReportCall;
bool gWantHost, gHostCable;            // from the last PD state
bool gHostUp, gHostBroken, gHostRole;  // what has been done
bool gHostFailed;                      // switch_usb_host_failed (SwitchXHCI's interrupt check), any thread
IOPMDriverAssertionID gHostHold;       // held from the host controller's first start for the rest of the boot
uint32_t gRoleChanges;                 // role changes applied (the report names the one it follows)
constexpr uint32_t kReportMS = 1000;   // the role line: this long after a change, when the port has settled

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

// SwitchPD's work loop: short. Our VBUS on a sink-only partner wants the host role; a cable from a host while we are
// its device means a bus reset is due. Nothing else (a bus suspend included) changes the role.
void pdChanged(void *, const SwitchPDState *state)
{
    IOLockLock(gLock);
    const bool wantHost = gHostPackaged && state->role == SWITCH_PD_ROLE_SOURCE;
    const bool hostCable =
        state->role == SWITCH_PD_ROLE_SINK && state->plugged && state->vbusPresent && !state->dataHost;
    const bool changed = wantHost != gWantHost || (hostCable && !gHostCable);
    gWantHost = wantHost;
    gHostCable = hostCable;
    IOLockUnlock(gLock);
    if (changed) thread_call_enter(gRoleCall);
}

// The role line, kReportMS after a role change (its own thread call, re-armed by every change: the last one wins):
// one os_log record with where the port stands, so a boot shows where host mode stops. The xHCI (USBCMD, USBSTS,
// every PORTSC, the last PP cycle) and the pad controller (port 0's capability, ID and VBUS overrides, pad power,
// charger detection, over-current maps); the PD controller (phase, STATUS1/2, switches, rails' GPIOs); the charger
// (REG01/05/08/09, battery current). Log only, never the boot console.
void reportRole(thread_call_param_t, thread_call_param_t change)
{
    char xhci[720], pd[300], charger[260];
    switchxhci_describe(xhci, sizeof(xhci));
    (void)switch_pd_describe(pd, sizeof(pd));
    (void)switch_battery_describe_otg(charger, sizeof(charger));
    const char *role = gHostRole ? "host" : "device";
    const unsigned number = (unsigned)(uintptr_t)change;
    // Under the 1 KiB os_log record (kprof keeps its lines under 900): one line, or two when it would not fit.
    if (strlen(xhci) + strlen(pd) + strlen(charger) < 820) {
        os_log(OS_LOG_DEFAULT, "SwitchUSB: %s role +1 s (change %u): %s | %s | %s", role, number, xhci, pd, charger);
    } else {
        os_log(OS_LOG_DEFAULT, "SwitchUSB: %s role +1 s (change %u, 1/2): %s", role, number, xhci);
        os_log(OS_LOG_DEFAULT, "SwitchUSB: %s role +1 s (change %u, 2/2): %s | %s", role, number, pd, charger);
    }
}

void armReport()
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(kReportMS, kMillisecondScale, &deadline);
    thread_call_cancel(gReportCall);
    thread_call_enter1_delayed(gReportCall, (thread_call_param_t)(uintptr_t)++gRoleChanges, deadline);
}

// Thread call (THREAD_CALL_OPTIONS_ONCE: never two at once; entered again while it runs, it runs again after).
// Host: the XUDC off the bus first (as a cable pull), the pad to the host (ID grounded; PP cycled when the controller
// already runs), then the controller the first time, which powers its ports with the pad already the host's
// (xhci-tegra.c: ID override, then PORTSC.PP). Device: the reverse.
void applyRole(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    const bool wantHost = gWantHost, hostCable = gHostCable;
    IOLockUnlock(gLock);
    if (__atomic_load_n(&gHostFailed, __ATOMIC_ACQUIRE)) gHostBroken = true;
    if (wantHost && !gHostRole && !gHostBroken) {
        char why[160] = "";
        switchxudc_set_host_owns_port(true);
        switchxhci_set_host_role(true);
        if (!gHostUp && !(gHostUp = switchxhci_start(why, sizeof(why)))) {
            IOLog("SwitchUSB: host mode off: %s\n", why);
            gHostBroken = true;
            switchxhci_set_host_role(false);
            switchxudc_set_host_owns_port(false);
            (void)switch_pd_allow_source(false);  // SwitchPD takes VBUS off and keeps the port sink-only
            return;
        }
        if (!gHostHold) {
            if (IOPMrootDomain *root = IOService::getPMRootDomain())
                gHostHold = root->createPMAssertion(kIOPMDriverAssertionCPUBit, kIOPMDriverAssertionLevelOn,
                                                    gDevicePort ? gDevicePort : IOService::getResourceService(),
                                                    "SwitchUSB host controller");
            IOLog("SwitchUSB: system sleep held off for this boot: the host controller has no SC7 path\n");
        }
        gHostRole = true;
        IOLog("SwitchUSB: host role (a device on the port, VBUS on)\n");
        switchxhci_expect_port_change();
        armReport();
    } else if (!wantHost && gHostRole) {
        switchxhci_set_host_role(false);
        switchxudc_set_host_owns_port(false);
        gHostRole = false;
        IOLog("SwitchUSB: device role\n");
        armReport();
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
    if (gHostPackaged && !gDevicePort && !railOn(why, sizeof(why))) {
        IOLog("SwitchUSB: host mode off: %s\n", why);
        gHostBroken = true;
    }
    if (gHostPackaged && !gHostBroken && switch_usb_dma_reserve()) {
        IOLog("SwitchUSB: host mode off: no 8 MiB DMA bounce pool below 4 GiB\n");
        gHostBroken = true;
    }
    // The current state comes at once; the role thread call does the rest. Then a sink may attach.
    if (switch_pd_add_listener(&pdChanged, nullptr)) {
        if (gHostPackaged && !gHostBroken) IOLog("SwitchUSB: host mode off: no PD controller\n");
    } else if (gHostPackaged && !gHostBroken) {
        (void)switch_pd_allow_source(true);
    }
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

// SwitchXHCI's interrupt check gave host mode up for this boot (it logged why): SwitchPD stops sourcing and keeps the
// port sink-only, which ends the host role through pdChanged; the role thread never takes the host role again.
void switch_usb_host_failed(void)
{
    __atomic_store_n(&gHostFailed, true, __ATOMIC_RELEASE);
    (void)switch_pd_allow_source(false);
}

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
    gReportCall = gRoleCall ? thread_call_allocate_with_options(&reportRole, nullptr, THREAD_CALL_PRIORITY_LOW,
                                                                THREAD_CALL_OPTIONS_ONCE)
                            : nullptr;
    thread_t thread = THREAD_NULL;
    if (!gReportCall || kernel_thread_start(&usbThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("SwitchUSB: device mode off: no thread\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}

extern "C" void switch_usb_suspend(void)
{
    if (gDevicePort) switchxudc_sleep(true);
}

extern "C" void switch_usb_resume(void)
{
    if (gDevicePort) switchxudc_sleep(false);
    if (gRoleCall) thread_call_enter(gRoleCall);  // a cable that came or went while asleep
}
