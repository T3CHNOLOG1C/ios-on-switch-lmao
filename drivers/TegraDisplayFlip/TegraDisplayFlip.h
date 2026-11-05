// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 display controller A: flip window A to another physically contiguous buffer and poll for vblank.
//
// Not wired in: NativeLegacyDisplay double-buffers window A itself (ioswitch-scanout-double=1), latching flips on the
// DC A frame-end interrupt rather than polling. Kept for its register notes. Hekate leaves window A scanning the
// 720x1280 portrait panel surface (B8G8R8A8, pitch-linear, stride 2880) at 0xF5A00000 (hekate bdk/display/di.inl
// _di_winA_pitch, bdk/memory_map.h IPL_FB_ADDRESS). This class only swaps the start address; every other window
// register stays.
//
// Registers are 32-bit words; offsets below are word indices (byte offset = index * 4), as in hekate
// bdk/display/di.h (DISPLAY_A(off) is MMIO_REG32(0x54200000, off << 2)) and L4T switch-l4t-kernel-nvidia
// drivers/video/tegra/dc/dc_reg.h, which agree:
//   0x037 DC_CMD_INT_STATUS     write 1 to clear; FRAME_END_INT bit 1, V_BLANK_INT bit 2
//   0x038 DC_CMD_INT_MASK       routes a status bit to the GIC (SPI 73); left alone (hekate keeps it 0)
//   0x039 DC_CMD_INT_ENABLE     lets a status bit latch; hekate sets it before polling (di.c display_wait_interrupt)
//   0x040 DC_CMD_STATE_ACCESS   READ_MUX_ACTIVE bit 0, WRITE_MUX_ACTIVE bit 2 (0 = assembly copy)
//   0x041 DC_CMD_STATE_CONTROL  GENERAL_ACT_REQ bit 0, WIN_A_ACT_REQ bit 1, GENERAL_UPDATE bit 8, WIN_A_UPDATE bit 9;
//                               an ACT_REQ bit reads 1 until the hardware has latched it (L4T dc/window.c
//                               tegra_dc_trigger_windows treats a clear WIN_x_ACT_REQ as "flip done")
//   0x042 DC_CMD_DISPLAY_WINDOW_HEADER  WINDOW_A_SELECT bit 4; selects the window the 0x7xx/0x8xx registers address
//   0x043 DC_CMD_REG_ACT_CONTROL  WIN_A_ACT_HCNTR_SEL bit 2: 1 = latch on the line counter (hekate's init value,
//                               can tear), 0 = latch on the frame counter. Hekate clears the window D bit the same way
//                               for its console (di.c display_window_d_console_enable).
//   0x700 DC_WIN_WIN_OPTIONS    WIN_ENABLE bit 30, SCAN_COLUMN bit 4
//   0x703 DC_WIN_COLOR_DEPTH    0xC B8G8R8A8, 0xD R8G8B8A8
//   0x705 DC_WIN_SIZE           H_SIZE bits 12:0, V_SIZE bits 28:16
//   0x70A DC_WIN_LINE_STRIDE    bytes in bits 15:0
//   0x800 DC_WINBUF_START_ADDR  physical address (SMMU is not enabled for the DC on this boot chain)
//   0x80B DC_WINBUF_SURFACE_KIND  bits 1:0, 0 = pitch-linear
//   0x80D DC_WINBUF_START_ADDR_HI  written by L4T window.c; kept 0 here since buffers must lie below 4 GiB
#pragma once
#include <IOKit/IOTypes.h>
#include <libkern/c++/OSObject.h>

class IOMemoryMap;
class IOBufferMemoryDescriptor;

class TegraDisplayFlip : public OSObject {
OSDeclareDefaultStructors(TegraDisplayFlip) public :
    // Maps DC A and checks that window A is an enabled 32-bit pitch-linear window. Switches window A to
    // frame-latched updates (REG_ACT_CONTROL) and lets V_BLANK/FRAME_END latch in INT_STATUS; free() restores both.
    static TegraDisplayFlip *create();

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    uint32_t stride() const { return stride_; }
    uint64_t bufferLength() const { return (uint64_t)stride_ * height_; }
    // The address the hardware is scanning out now (active copy of DC_WINBUF_START_ADDR).
    uint64_t scanoutAddress();

    // Points window A at a buffer laid out exactly like the current one (same stride, size and format). The buffer
    // must be physically contiguous, page aligned and below 4 GiB. Returns once the hardware latched the new
    // address at a frame boundary: from then on the previous buffer is no longer read and may be reused.
    // Thread context only (polls with IODelay); kIOReturnTimeout if nothing latched within timeoutUs.
    IOReturn flip(uint64_t physicalAddress, uint64_t length, uint32_t timeoutUs = 50000);

    // Waits for the start of the next vertical blank by polling INT_STATUS (the DC interrupt stays masked).
    // Thread context only: sleeps in 1 ms steps, then spins for the last stretch. timestamp is mach_absolute_time().
    IOReturn waitForVblank(uint32_t timeoutUs, uint64_t *timestamp = nullptr);

    // Write-combined (Normal non-cacheable) kernel mapping of physical scanout memory, e.g. the boot framebuffer or a
    // second range reserved next to it. Use this, not kIOMapInhibitCache, for anything the CPU fills.
    static IOMemoryMap *mapScanoutMemory(uint64_t physicalAddress, uint64_t length);
    // A physically contiguous buffer below 4 GiB from kernel memory, page aligned. getBytesNoCopy() is NOT write-combined: for
    // kernel_task the cache option only applies to later map() calls, and map() into kernel_task without kIOMapUnique returns that
    // same cacheable address (IOBufferMemoryDescriptor.cpp, IOGeneralMemoryDescriptor::doMap). Clean+invalidate it and map the
    // physical range with mapScanoutMemory, as NativeLegacyDisplay's second buffer does. Physical address: getPhysicalSegment(0,nullptr,kIOMemoryMapperNone).
    // Allocate early (driver start): 3.5 MiB of contiguous pages gets hard to find later. These are XNU-managed
    // pages that keep a cacheable physmap alias (ios-flame saw corruption from such WC aliases); prefer
    // mapScanoutMemory on RAM the loader keeps out of XNU, and use this only when that is not available.
    static IOBufferMemoryDescriptor *allocateScanoutBuffer(uint64_t length);

protected:
    void free() override;

private:
    bool attach();
    uint32_t read(uint32_t index) const { return dc_[index]; }
    void write(uint32_t index, uint32_t value) { dc_[index] = value; }
    uint32_t readWindowAActive(uint32_t index);

    IOMemoryMap *registers_;
    volatile uint32_t *dc_;
    uint32_t width_, height_, stride_;
    uint32_t savedIntEnable_, savedActControl_;
};
