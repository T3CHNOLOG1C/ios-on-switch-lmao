// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB device mode on the Switch's USB-C port: start-up (see SwitchUSB.h).
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <libkern/c++/OSData.h>
#include "../TegraI2C/TegraI2C.h"
#include "SwitchUSB.h"

namespace {
// avddio_usb (OLED DTB xudc@700d0000) is MAX77620 LDO1 at 1.05 V, shared with PCIe (drivers/SwitchWLAN TegraPCIe
// rail(): same register, same value); normally on already. hvdd_usb (3.3 V) and avdd_pll_utmip (SD3, 1.8 V) are
// always on.
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

void usbThread(void *parameter, wait_result_t)
{
    auto *port = static_cast<IOService *>(parameter);
    char why[160] = "";
    if (railOn(why, sizeof(why)) && switchxudc_start(port, why, sizeof(why))) {
        const OSData *configuration = OSDynamicCast(OSData, port->getProperty("configuration-string"));
        IOLog("SwitchUSB: device mode ready (XUDC at 0x700d0000, configuration %s)\n",
              configuration && configuration->getLength() ? (const char *)configuration->getBytesNoCopy() : "?");
    } else {
        IOLog("SwitchUSB: device mode off: %s\n", why);
    }
    port->release();  // returning ends the thread (Call_continuation -> thread_terminate)
}
}  // namespace

extern "C" int switch_usb_start(void)
{
    static uint32_t started;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return 0;
    // ArmDTPlatform publishes every /arm-io child as an IOPlatformDevice that takes the node's place in the device
    // tree plane, so the path names the nub itself.
    IORegistryEntry *node = IORegistryEntry::fromPath("/arm-io/usb-device", gIODTPlane);
    if (!node) return 0;  // not packaged with USB
    IOService *port = OSDynamicCast(IOService, node);
    if (!port) {
        IOLog("SwitchUSB: device mode off: /arm-io/usb-device was not published\n");
        node->release();
        return 0;
    }
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(&usbThread, port, &thread) != KERN_SUCCESS) {
        IOLog("SwitchUSB: device mode off: no thread\n");
        port->release();
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}
