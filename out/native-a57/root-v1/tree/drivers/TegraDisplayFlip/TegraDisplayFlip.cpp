// SPDX-License-Identifier: BSD-2-Clause-Patent
// Register map and sources: see TegraDisplayFlip.h.
#include "TegraDisplayFlip.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>

OSDefineMetaClassAndStructors(TegraDisplayFlip, OSObject)

    namespace
{
    constexpr uint64_t kDisplayABase = 0x54200000, kDisplayALength = 0x40000;  // dc@54200000 in mainline tegra210.dtsi

    constexpr uint32_t kIntStatus = 0x037, kIntEnable = 0x039, kStateAccess = 0x040, kStateControl = 0x041;
    constexpr uint32_t kWindowHeader = 0x042, kRegActControl = 0x043;
    constexpr uint32_t kWinOptions = 0x700, kWinColorDepth = 0x703, kWinSize = 0x705, kWinLineStride = 0x70a;
    constexpr uint32_t kWinbufStartAddr = 0x800, kWinbufSurfaceKind = 0x80b, kWinbufStartAddrHi = 0x80d;

    constexpr uint32_t kFrameEndInt = 1u << 1, kVblankInt = 1u << 2;
    constexpr uint32_t kReadMuxActive = 1u << 0;
    constexpr uint32_t kGeneralActReq = 1u << 0, kWinAActReq = 1u << 1, kGeneralUpdate = 1u << 8, kWinAUpdate = 1u << 9;
    constexpr uint32_t kWindowASelect = 1u << 4;
    constexpr uint32_t kWinAActHcntrSel = 1u << 2;
    constexpr uint32_t kWinEnable = 1u << 30, kScanColumn = 1u << 4;
    constexpr uint32_t kColorB8G8R8A8 = 0xc, kColorR8G8B8A8 = 0xd;

    uint64_t deadlineAfter(uint32_t microseconds)
    {
        uint64_t interval = 0;
        nanoseconds_to_absolutetime((uint64_t)microseconds * 1000, &interval);
        return mach_absolute_time() + interval;
    }
}

TegraDisplayFlip *TegraDisplayFlip::create()
{
    TegraDisplayFlip *flip = OSTypeAlloc(TegraDisplayFlip);
    if (flip && (!flip->init() || !flip->attach())) OSSafeReleaseNULL(flip);
    return flip;
}

bool TegraDisplayFlip::attach()
{
    IODeviceMemory *memory = IODeviceMemory::withRange(kDisplayABase, kDisplayALength);
    if (memory) registers_ = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    OSSafeReleaseNULL(memory);
    if (!registers_) {
        IOLog("TegraDisplayFlip: cannot map DC A registers\n");
        return false;
    }
    dc_ = reinterpret_cast<volatile uint32_t *>(registers_->getVirtualAddress());

    const uint32_t options = readWindowAActive(kWinOptions);
    const uint32_t depth = readWindowAActive(kWinColorDepth);
    const uint32_t size = readWindowAActive(kWinSize);
    const uint32_t kind = readWindowAActive(kWinbufSurfaceKind);
    width_ = size & 0x1fff;
    height_ = (size >> 16) & 0x1fff;
    stride_ = readWindowAActive(kWinLineStride) & 0xffff;
    if (!(options & kWinEnable) || (options & kScanColumn) || (depth != kColorB8G8R8A8 && depth != kColorR8G8B8A8) ||
        (kind & 3) || !width_ || !height_ || stride_ < width_ * 4) {
        IOLog("TegraDisplayFlip: window A not a 32-bit pitch-linear scanout (options %#x depth %#x size %#x kind %#x "
              "stride %u)\n",
              options, depth, size, kind, stride_);
        dc_ = nullptr;  // nothing was changed, so free() must not restore anything
        return false;
    }
    // Reads and writes below go to the assembly copy; only ACT_REQ moves it to the active one.
    write(kStateAccess, 0);
    savedActControl_ = read(kRegActControl);
    write(kRegActControl, savedActControl_ & ~kWinAActHcntrSel);
    savedIntEnable_ = read(kIntEnable);
    write(kIntEnable, savedIntEnable_ | kVblankInt | kFrameEndInt);
    return true;
}

void TegraDisplayFlip::free()
{
    if (dc_) {
        write(kIntEnable, savedIntEnable_);
        write(kIntStatus, kVblankInt | kFrameEndInt);
        write(kRegActControl, savedActControl_);
    }
    dc_ = nullptr;
    OSSafeReleaseNULL(registers_);
    OSObject::free();
}

uint32_t TegraDisplayFlip::readWindowAActive(uint32_t index)
{
    const uint32_t access = read(kStateAccess);
    write(kStateAccess, access | kReadMuxActive);
    write(kWindowHeader, kWindowASelect);
    const uint32_t value = read(index);
    write(kStateAccess, access);
    return value;
}

uint64_t TegraDisplayFlip::scanoutAddress()
{
    return ((uint64_t)readWindowAActive(kWinbufStartAddrHi) << 32) | readWindowAActive(kWinbufStartAddr);
}

IOReturn TegraDisplayFlip::flip(uint64_t physicalAddress, uint64_t length, uint32_t timeoutUs)
{
    if (!physicalAddress || (physicalAddress & PAGE_MASK) || length < bufferLength() ||
        physicalAddress > (1ull << 32) || length > (1ull << 32) - physicalAddress)
        return kIOReturnBadArgument;
    // CPU stores to the buffer (write-combined or cleaned) must be in memory before the DC can fetch them.
    __asm__ volatile("dsb sy" ::: "memory");
    write(kWindowHeader, kWindowASelect);
    write(kWinbufStartAddr, (uint32_t)physicalAddress);
    write(kWinbufStartAddrHi, 0);
    write(kStateControl, kGeneralUpdate | kWinAUpdate);
    write(kStateControl, kGeneralActReq | kWinAActReq);
    const uint64_t deadline = deadlineAfter(timeoutUs);
    while (read(kStateControl) & (kGeneralActReq | kWinAActReq)) {
        if (mach_absolute_time() > deadline) {
            IOLog("TegraDisplayFlip: window A flip to %#llx not latched within %u us\n", physicalAddress, timeoutUs);
            return kIOReturnTimeout;
        }
        IODelay(20);
    }
    return kIOReturnSuccess;
}

IOReturn TegraDisplayFlip::waitForVblank(uint32_t timeoutUs, uint64_t *timestamp)
{
    uint64_t spinWindow = 0;
    nanoseconds_to_absolutetime(1500 * 1000, &spinWindow);
    const uint64_t deadline = deadlineAfter(timeoutUs);
    write(kIntStatus, kVblankInt);
    for (;;) {
        if (read(kIntStatus) & kVblankInt) break;
        const uint64_t now = mach_absolute_time();
        if (now > deadline) return kIOReturnTimeout;
        if (deadline - now > spinWindow)
            IOSleep(1);
        else
            IODelay(20);
    }
    if (timestamp) *timestamp = mach_absolute_time();
    return kIOReturnSuccess;
}

IOMemoryMap *TegraDisplayFlip::mapScanoutMemory(uint64_t physicalAddress, uint64_t length)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(physicalAddress, length);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
    OSSafeReleaseNULL(memory);
    return map;
}

IOBufferMemoryDescriptor *TegraDisplayFlip::allocateScanoutBuffer(uint64_t length)
{
    // No system mapper on this platform, so the physical mask and contiguity are honoured by the allocator.
    const IOOptionBits options = IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPhysicallyContiguous) |
                                 IOOptionBits(kIOMemoryMapperNone) | IOOptionBits(kIOMapWriteCombineCache);
    IOBufferMemoryDescriptor *buffer =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, length, 0x00000000fffff000ULL);
    if (!buffer) return nullptr;
    IOByteCount segment = 0;
    if (!buffer->getPhysicalSegment(0, &segment, kIOMemoryMapperNone) || segment < length) {
        IOLog("TegraDisplayFlip: scanout buffer of %llu bytes is not physically contiguous\n", length);
        OSSafeReleaseNULL(buffer);
    }
    return buffer;
}
