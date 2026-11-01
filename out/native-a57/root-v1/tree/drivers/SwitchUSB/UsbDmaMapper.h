// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDMAMapper: DMA coherency for Apple's USB host stack on the Tegra X1.
//
// Tegra's XUSB host does not snoop the A57 caches (no dma-coherent in any T210 device tree), while
// AppleUSBXHCI was written for Apple SoCs whose USB DMA is coherent: it never cleans or invalidates.
// Every DMA it does goes through an IODMACommand on its controller's IOMapper (AppleUSBHostController::
// copyMapper, vtable slot 253, which SwitchXHCI overrides to return this one). It never translates (the
// bus address it returns is a physical address) and makes each mapping coherent:
//
//   * controller structures (AppleUSBHostDMABufferMemoryDescriptor: rings, contexts, scratchpads, TD
//     pools; physically contiguous, mapped once for their lifetime and shared with the controller):
//     identity, and their kernel mappings switched to normal non-cacheable (pmap_set_cache_attributes
//     VM_WIMG_WCOMB, which also cleans the pages) until they are unmapped, when the default attribute
//     is restored and the cache lines are invalidated;
//   * everything else (client buffers of any layout, prepared per transfer): bounced through a
//     non-cacheable pool below 4 GiB, copied in at map time and back at unmap time when the device
//     may have written (mapOptions kIODMAMapWriteAccess, IOMemoryDescriptor::dmaMap).
//
// iovmMapMemory/iovmUnmapMemory run in thread context (IODMACommand prepare/complete), never at
// interrupt level. The mapper is a native XNU class: its static initializer runs normally.
#pragma once
#include <IOKit/IOMapper.h>

class IOService;
// Creates (once) and starts the mapper attached to IOResources; nullptr on failure (logged).
IOMapper *switch_usb_dma_mapper(void);
