// SPDX-License-Identifier: BSD-2-Clause-Patent
// RAM-backed NVRAM controller for XNU's IODTNVRAM.
//
// IODTNVRAM builds its variables from /chosen/nvram-proxy-data and keeps them
// in memory; it only needs a controller to commit them. Without one every sync
// fails with kIOReturnNotReady and the "IONVRAM" resource is never published
// (IOPlatformExpert::registerNVRAMController publishes it); the original
// AppleImage4 waits for that resource without a timeout before every NVRAM
// write, so those writers would block forever. This controller
// holds the banks in RAM, seeded from the same proxy, so syncs succeed and the
// resource appears. Nothing survives a reboot yet: sync() is where a future
// persistence backend would write the current bank.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/nvram/IONVRAMController.h>
#include <libkern/c++/OSData.h>
#include "SwitchNVRAM.h"

class SwitchNVRAM : public IONVRAMController {
    OSDeclareDefaultStructors(SwitchNVRAM) uint8_t *banks = nullptr;
    uint32_t bankSize = 0, bankCount = 0, current = 0;

    bool loadChosenImage();

public:
    bool start(IOService *provider) override;
    void free() override;
    void sync() override;
    IOReturn select(uint32_t bank) override;
    IOReturn eraseBank() override;
    IOReturn read(IOByteCount offset, UInt8 *buffer, IOByteCount length) override;
    IOReturn write(IOByteCount offset, UInt8 *buffer, IOByteCount length) override;
};

OSDefineMetaClassAndStructors(SwitchNVRAM, IONVRAMController)

    namespace
{
    uint32_t chosenU32(IORegistryEntry * chosen, const char *key, uint32_t fallback)
    {
        OSData *data = OSDynamicCast(OSData, chosen->getProperty(key));
        if (!data || data->getLength() != sizeof(uint32_t)) return fallback;
        uint32_t value;
        memcpy(&value, data->getBytesNoCopy(), sizeof(value));
        return value;
    }
}

// Same geometry IODTNVRAM read from /chosen; the current bank starts as the
// proxy image, so the handler's findCurrentBank() sees its own generation.
bool SwitchNVRAM::loadChosenImage()
{
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) return false;
    bankSize = chosenU32(chosen, "nvram-bank-size", 0);
    bankCount = chosenU32(chosen, "nvram-bank-count", 1);
    current = chosenU32(chosen, "nvram-current-bank", 0);
    bool ok = bankSize && bankSize <= 0x10000 && bankCount && bankCount <= 8 && current < bankCount;
    if (ok) banks = IONewZeroData(uint8_t, bankSize *bankCount);
    ok = ok && banks;
    if (ok) {
        OSData *proxy = OSDynamicCast(OSData, chosen->getProperty("nvram-proxy-data"));
        if (proxy && proxy->getLength() == bankSize)
            memcpy(banks + current * bankSize, proxy->getBytesNoCopy(), bankSize);
    }
    chosen->release();
    return ok;
}

bool SwitchNVRAM::start(IOService *provider)
{
    if (!IONVRAMController::start(provider)) return false;
    if (!loadChosenImage()) {
        IOLog("SwitchNVRAM: /chosen has no usable nvram-bank-size/count\n");
        return false;
    }
    return true;
}

void SwitchNVRAM::free()
{
    if (banks) IODeleteData(banks, uint8_t, bankSize *bankCount);
    banks = nullptr;
    IONVRAMController::free();
}

// IODTNVRAM calls select/erase/read/write/sync with its controller lock held.
IOReturn SwitchNVRAM::select(uint32_t bank)
{
    if (bank >= bankCount) return kIOReturnBadArgument;
    current = bank;
    return kIOReturnSuccess;
}

IOReturn SwitchNVRAM::eraseBank()
{
    bzero(banks + current * bankSize, bankSize);
    return kIOReturnSuccess;
}

IOReturn SwitchNVRAM::read(IOByteCount offset, UInt8 *buffer, IOByteCount length)
{
    if (!buffer || offset > bankSize || length > bankSize - offset) return kIOReturnBadArgument;
    memcpy(buffer, banks + current * bankSize + offset, length);
    return kIOReturnSuccess;
}

IOReturn SwitchNVRAM::write(IOByteCount offset, UInt8 *buffer, IOByteCount length)
{
    if (!buffer || offset > bankSize || length > bankSize - offset) return kIOReturnBadArgument;
    memcpy(banks + current * bankSize + offset, buffer, length);
    return kIOReturnSuccess;
}

void SwitchNVRAM::sync()
{
    // IODTNVRAM calls this after each full-bank write. A persistence backend
    // would queue banks + current * bankSize (bankSize bytes) for durable
    // storage here; until then the image lives for this boot only.
}

extern "C" int ioswitch_nvram_publish(void)
{
    static int status = -1;  // boot-only: -1 untouched, 0 published, 1 failed
    if (status >= 0) return status;
    status = 1;
    IOService *resources = IOService::getResourceService();
    SwitchNVRAM *nvram = OSTypeAlloc(SwitchNVRAM);
    if (!resources || !nvram || !nvram->init() || !nvram->attach(resources)) {
        IOLog("SwitchNVRAM: publish failed\n");
        OSSafeReleaseNULL(nvram);
        return status;
    }
    if (!nvram->start(resources)) {
        nvram->detach(resources);
        nvram->release();
        return status;
    }
    // IONVRAMController::registerService hands us to IODTNVRAM (which reads the
    // current bank header) and publishes the "IONVRAM" resource.
    nvram->registerService();
    nvram->release();
    IOLog("SwitchNVRAM: RAM-backed controller registered (not persistent)\n");
    status = 0;
    return status;
}
