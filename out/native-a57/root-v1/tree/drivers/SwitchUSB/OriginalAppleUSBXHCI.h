// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pinned iPad7,1 21H461 AppleUSBXHCI ABI (com.apple.driver.usb.AppleUSBXHCI 1.2, IOUSBHostFamily 1.2).
//
// AppleUSBXHCI is Apple's generic xHCI driver: rings, slots, endpoints, ports, hubs, power, all of it.
// On the iPad its only subclass is AppleNubT8011USBXHCI (com.apple.driver.usb.AppleUSBHostT8011), which
// overrides start (+0xe48 provider cast, "clock-mask", clears the cache-mode bits of the ring buffer
// options at +1172, then AppleUSBXHCI::start), createPort (ADT "port"/"port-type" Type-C ports), three
// power hooks and the register accessors (slots 289-297). SwitchXHCI is the same kind of glue for the
// Tegra X1 XUSB host controller. The class is concrete: no pure virtual slot.
//
// Only the slot order, the instance size and what SwitchXHCI overrides are declared. Every original
// symbol is bound to its 21H461 address by build_switchusbhost.py, which also checks the compiled
// vtable slot by slot against the original one (299 slots) and the instruction bytes at every binding.
//
// Facts used (iOS kernelcache, see the build script for the addresses):
//   - start (slot 86) maps getProvider()->getDeviceMemoryWithIndex(0) (slot 121), reads the capability
//     registers through regRead16/regRead32 (289/290) into +1280.., requires HCSPARAMS1.MaxIntrs >= 3
//     ("%d interrupters supported, %d are required"), creates one IOFilterInterruptEventSource per
//     provider interrupt index that getInterruptType() accepts (up to 8; index 0 primaryInterrupt,
//     1 ioInterrupt, 2 isochronousInterrupt) and runs legacy (shared line) interrupters when only one
//     exists ([this+3560] = 0).
//   - every controller DMA buffer is an AppleUSBHostDMABufferMemoryDescriptor prepared through an
//     IODMACommand on the mapper of copyMapper() (slot 253, AppleUSBHostController::start keeps it as
//     _mapper; the original is `ldr x0, [x0, #136]; b IOMapper::copyMapperForDevice`), and transfer DMA
//     commands use that mapper too.
//   - ring buffers are allocated with the options at +1172 (AppleUSBHostController::init: 0x13, or 0x713
//     on some commpage flag), transfer DMA commands with the specification copied from +200.
#pragma once
#include <IOKit/IOService.h>
class IOMapper;

#define USBX_OPAQUE(n) virtual __attribute__((deprecated("inherited original slot: never call"))) void opaque##n()

class AppleUSBXHCI : public IOService {
    OSDeclareDefaultStructors(
        AppleUSBXHCI) unsigned char originalState[0xe48 -
                                                  0x88];  // AppleUSBHostController (0x4e8) + AppleUSBXHCI fields
public:
    // IOService slots the original (AppleUSBXHCI or AppleUSBHostController) overrides.
    bool init(OSDictionary *dictionary) override;                                        // 21
    void free() override;                                                                // 14
    IOReturn setProperties(OSObject *properties) override;                               // 44
    bool didTerminate(IOService *provider, IOOptionBits options, bool *defer) override;  // 77
    void systemWillShutdown(IOOptionBits specifier) override;                            // 79
    void registerService(IOOptionBits options) override;                                 // 84
    bool start(IOService *provider) override;                                            // 86
    void stop(IOService *provider) override;                                             // 87
    bool open(IOService *forClient, IOOptionBits options, void *arg) override;           // 88
    void close(IOService *forClient, IOOptionBits options) override;                     // 89
    bool handleOpen(IOService *forClient, IOOptionBits options, void *arg) override;     // 91
    void handleClose(IOService *forClient, IOOptionBits options) override;               // 92
    bool handleIsOpen(const IOService *forClient) const override;                        // 93
    bool terminate(IOOptionBits options) override;                                       // 94
    bool matchPropertyTable(OSDictionary *table, SInt32 *score) override;                // 101
    IOWorkLoop *getWorkLoop() const override;                                            // 111
    IOReturn message(UInt32 type, IOService *provider, void *argument) override;         // 132
    IOReturn messageClients(UInt32 type, void *argument, vm_size_t argSize) override;    // 134
    void PMstop() override;                                                              // 145
    IOReturn setPowerState(unsigned long state, IOService *device) override;             // 155
    unsigned long maxCapabilityForDomainState(IOPMPowerFlags domainState) override;      // 156
    IOReturn powerStateDidChangeTo(IOPMPowerFlags capabilities, unsigned long state,
                                   IOService *device) override;  // 160

    // AppleUSBHostController (168-259) and AppleUSBXHCI (260-298), inherited unchanged.
    USBX_OPAQUE(168);
    USBX_OPAQUE(169);
    USBX_OPAQUE(170);
    USBX_OPAQUE(171);
    USBX_OPAQUE(172);
    USBX_OPAQUE(173);
    USBX_OPAQUE(174);
    USBX_OPAQUE(175);
    USBX_OPAQUE(176);
    USBX_OPAQUE(177);  // AppleNubT8011USBXHCI: setActive
    USBX_OPAQUE(178);
    USBX_OPAQUE(179);
    USBX_OPAQUE(180);  // lowerOnePowerStateTo (T8011 overrides: PHY power)
    USBX_OPAQUE(181);
    USBX_OPAQUE(182);  // lowerOnePowerStateTo path (T8011: setActive)
    USBX_OPAQUE(183);  // raiseOnePowerStateTo (T8011: USB3HOST tunables)
    USBX_OPAQUE(184);
    USBX_OPAQUE(185);
    USBX_OPAQUE(186);
    USBX_OPAQUE(187);
    USBX_OPAQUE(188);
    USBX_OPAQUE(189);
    USBX_OPAQUE(190);
    USBX_OPAQUE(191);
    USBX_OPAQUE(192);
    USBX_OPAQUE(193);
    USBX_OPAQUE(194);
    USBX_OPAQUE(195);
    USBX_OPAQUE(196);
    USBX_OPAQUE(197);
    USBX_OPAQUE(198);
    USBX_OPAQUE(199);
    USBX_OPAQUE(200);
    USBX_OPAQUE(201);
    USBX_OPAQUE(202);
    USBX_OPAQUE(203);
    USBX_OPAQUE(204);
    USBX_OPAQUE(205);
    USBX_OPAQUE(206);
    USBX_OPAQUE(207);
    USBX_OPAQUE(208);
    USBX_OPAQUE(209);
    USBX_OPAQUE(210);
    USBX_OPAQUE(211);
    USBX_OPAQUE(212);
    USBX_OPAQUE(213);
    USBX_OPAQUE(214);
    USBX_OPAQUE(215);
    USBX_OPAQUE(216);
    USBX_OPAQUE(217);
    USBX_OPAQUE(218);
    USBX_OPAQUE(219);
    USBX_OPAQUE(220);
    USBX_OPAQUE(221);
    USBX_OPAQUE(222);
    USBX_OPAQUE(223);
    USBX_OPAQUE(224);
    USBX_OPAQUE(225);
    USBX_OPAQUE(226);
    USBX_OPAQUE(227);
    USBX_OPAQUE(228);
    USBX_OPAQUE(229);
    USBX_OPAQUE(230);
    USBX_OPAQUE(231);
    USBX_OPAQUE(232);
    USBX_OPAQUE(233);
    USBX_OPAQUE(234);
    USBX_OPAQUE(235);
    USBX_OPAQUE(236);
    USBX_OPAQUE(237);
    USBX_OPAQUE(238);
    USBX_OPAQUE(239);
    USBX_OPAQUE(240);
    USBX_OPAQUE(241);
    USBX_OPAQUE(242);
    USBX_OPAQUE(243);
    USBX_OPAQUE(244);
    USBX_OPAQUE(245);
    USBX_OPAQUE(246);
    USBX_OPAQUE(247);
    USBX_OPAQUE(248);
    USBX_OPAQUE(249);
    USBX_OPAQUE(250);
    USBX_OPAQUE(251);
    USBX_OPAQUE(252);
    virtual IOMapper *copyMapper();  // 253: IOMapper::copyMapperForDevice([this+136]) (returns a reference)
    USBX_OPAQUE(254);
    USBX_OPAQUE(255);
    USBX_OPAQUE(256);
    USBX_OPAQUE(257);
    USBX_OPAQUE(258);
    USBX_OPAQUE(259);
    USBX_OPAQUE(260);
    USBX_OPAQUE(261);
    USBX_OPAQUE(262);
    USBX_OPAQUE(263);
    USBX_OPAQUE(264);
    USBX_OPAQUE(265);
    USBX_OPAQUE(266);
    USBX_OPAQUE(267);
    USBX_OPAQUE(268);
    USBX_OPAQUE(269);
    USBX_OPAQUE(270);
    USBX_OPAQUE(271);
    USBX_OPAQUE(272);
    USBX_OPAQUE(273);  // createPort (generic AppleUSB20/30XHCIPort from the Supported Protocol capabilities)
    USBX_OPAQUE(274);
    USBX_OPAQUE(275);
    USBX_OPAQUE(276);
    USBX_OPAQUE(277);
    USBX_OPAQUE(278);
    USBX_OPAQUE(279);
    USBX_OPAQUE(280);
    USBX_OPAQUE(281);
    USBX_OPAQUE(282);
    USBX_OPAQUE(283);
    USBX_OPAQUE(284);
    USBX_OPAQUE(285);
    USBX_OPAQUE(286);
    USBX_OPAQUE(287);
    USBX_OPAQUE(288);
    // Register accessors: (mapped base, byte offset). The read-modify-write helpers 294-297 call these
    // through the vtable, so overriding 290/292/293 sees every 32/64-bit register access.
    virtual uint16_t regRead16(uint64_t base, uint32_t offset);               // 289 (32-bit read, half selected)
    virtual uint32_t regRead32(uint64_t base, uint32_t offset);               // 290 ml_io_read32
    virtual uint64_t regRead64(uint64_t base, uint32_t offset);               // 291 ml_io_read64
    virtual void regWrite32(uint64_t base, uint32_t offset, uint32_t value);  // 292 ml_io_write32
    virtual void regWrite64(uint64_t base, uint32_t offset, uint64_t value);  // 293 ml_io_write64
    USBX_OPAQUE(294);                                                         // setBits32
    USBX_OPAQUE(295);                                                         // setBits64
    USBX_OPAQUE(296);                                                         // clearBits32
    USBX_OPAQUE(297);                                                         // clearBits64
    USBX_OPAQUE(298);

    // Original fields SwitchXHCI writes before AppleUSBXHCI::start (AppleUSBHostController::init sets them).
    uint32_t &ringBufferOptions()
    {
        return *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this) + 1172);
    }
};
static_assert(sizeof(IOService) == 0x88, "Pinned original IOService base size");
static_assert(sizeof(AppleUSBXHCI) == 0xe48, "Pinned original AppleUSBXHCI size");

#undef USBX_OPAQUE
