// SPDX-License-Identifier: BSD-2-Clause-Patent
// An accelerator that only exists to be found. QuartzCore's render server, before it asks Metal for a device, polls
// IOServiceGetMatchingServices(IOServiceMatching("IOAcceleratorES")) up to 300 times 100 ms apart (CAMetalContextCreate's
// dispatch_once block, QuartzCore 21H461 0x189595afc: cmp w21,#0x12b; usleep(0x186a0)). With no such service backboardd's
// window server start sat out the whole 30 s on every boot. This class has no MetalPluginName, so Metal's
// _MTLRegisterDevices skips it (getMetalPluginClassForService returns nil) and CA keeps its CPU renderer exactly as before.
// It derives from the original IOAcceleratorES (drivers/NativeMetal/OriginalAcceleratorABI.h; original-bindings.S from
// scripts/data/ioaccelerator-base-abi.json), so it is built like TegraGM20BAccelerator.cpp: static initializer staged out
// of __mod_init_func and run by IOSwitchAccelStubPublish.cpp once IOGPUFamily has loaded.
#include "../NativeMetal/OriginalAcceleratorABI.h"

class IOSwitchAccelStub : public IOAcceleratorES {
    OSDeclareDefaultStructors(IOSwitchAccelStub)
};
OSDefineMetaClassAndStructors(IOSwitchAccelStub, IOAcceleratorES)

    extern "C" IOService *ioswitch_accel_stub_allocate(void)
{
    return new IOSwitchAccelStub;
}
extern "C" unsigned ioswitch_accel_stub_instance_size(void)
{
    return sizeof(IOSwitchAccelStub);
}
