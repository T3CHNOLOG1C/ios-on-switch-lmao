// SPDX-License-Identifier: BSD-2-Clause-Patent
// Publishes IOSwitchAccelStub.cpp's accelerator from bsd_init, long before backboardd starts: load IOGPUFamily (prelinked;
// nothing else loads it) so the IOAcceleratorES metaclass exists, register our subclass inside preModLoad/postModLoad, then
// attach one registered, never-started instance to the platform expert. Same staging as TegraGM20B.cpp's publish() and
// UniXNU-qemu's QemuMetalPublish.cpp. Registering is what IOServiceGetMatchingServices needs (kIOServiceMatchedState,
// IOService::getMatchingServices). Any failure is a build or ABI mismatch, so it panics instead of costing 30 s per boot.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSMetaClass.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void ioswitch_accel_stub_static_init(void) __asm__("__GLOBAL__sub_I_IOSwitchAccelStub.cpp");
IOService *ioswitch_accel_stub_allocate(void);
unsigned ioswitch_accel_stub_instance_size(void);
void ioswitch_accel_stub_publish(void);
}

void ioswitch_accel_stub_publish(void)
{
    // tegra-gpu=4 publishes the real AGXAccelerator (drivers/TegraGM20B). The stub would end QuartzCore's poll before that
    // exists and Metal would cache an empty device list for backboardd's lifetime, so it stays out of the way then.
    unsigned gpuStage = 0;
    if (PE_parse_boot_argn("tegra-gpu", &gpuStage, sizeof(gpuStage)) && gpuStage >= 4) return;
    // ioswitch-accel-stub=0: no stub (QuartzCore waits 30 s), for a boot where IOGPUFamily cannot load.
    unsigned stub = 1;
    if (PE_parse_boot_argn("ioswitch-accel-stub", &stub, sizeof(stub)) && !stub) return;
    const kern_return_t loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (loaded) panic("IOSwitch accel stub: IOGPUFamily load failed 0x%x", loaded);
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136 || ioswitch_accel_stub_instance_size() != 136) {
        panic("IOSwitch accel stub: IOAcceleratorES %p size %u, ours %u", parent, parent ? parent->getClassSize() : 0,
              ioswitch_accel_stub_instance_size());
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("IOSwitch accel stub: preModLoad failed");
    ioswitch_accel_stub_static_init();
    const OSReturn registered = OSMetaClass::postModLoad(handle);
    if (registered) panic("IOSwitch accel stub: postModLoad 0x%x", registered);
    IOService *accelerator = ioswitch_accel_stub_allocate();
    if (!accelerator || !accelerator->init()) panic("IOSwitch accel stub: allocation failed");
    if (!accelerator->attach(IOService::getPlatform())) panic("IOSwitch accel stub: attach to the platform failed");
    accelerator->registerService();
    accelerator->release();
}
