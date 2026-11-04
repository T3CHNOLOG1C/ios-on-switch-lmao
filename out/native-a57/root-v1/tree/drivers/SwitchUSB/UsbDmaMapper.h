// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDMAMapper: DMA for Apple's USB host stack on the Tegra X1, whose XUSB host does not snoop the A57 caches.
//
// AppleUSBHostController asks its subclass for a mapper (copyMapper, vtable slot 253) and keeps it as the mapper of its
// buffers and DMA commands, so every DMA address AppleUSBXHCI hands the controller comes from here. It never
// translates (a bus address is a physical address):
//   * controller structures (rings, contexts, scratchpads) are allocated non-cacheable: SwitchXHCI gives AppleUSBXHCI's
//     ring buffer options the write-combine cache mode (normal non-cacheable), so they are mapped as they are;
//   * other physically contiguous memory (a cacheable client buffer) is mapped as it is, cleaned before the
//     controller reads it and cleaned and invalidated after it may have written (IOMemoryDescriptor::performOperation,
//     kIOMemoryIncoherentIOStore/Flush);
//   * anything else (scattered pages, or out of the controller's reach) goes through a non-cacheable bounce pool below
//     4 GiB, copied in at map time and back at unmap time when the controller may have written.
// Map and unmap run in thread context (IODMACommand prepare/complete), never at interrupt level.
#pragma once
#include <IOKit/IOMapper.h>

// Creates (once) and starts the mapper attached to IOResources; nullptr on failure (logged). `reach` is the first
// physical address the controller cannot address (HCCPARAMS1.AC64: 64-bit or 32-bit DMA).
IOMapper *switch_usb_dma_mapper(uint64_t reach);
