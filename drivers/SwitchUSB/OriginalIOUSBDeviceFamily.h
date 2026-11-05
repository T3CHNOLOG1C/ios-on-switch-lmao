// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pinned iPad7,1 21H461 IOUSBDeviceFamily ABI (com.apple.iokit.IOUSBDeviceFamily 2.0.0).
//
// IOUSBDeviceController is the original abstract base of a USB device-mode
// controller driver (AppleUSBXDCI is the in-cache reference subclass). Only
// the slot order, the instance size and the members called from SwitchXUDC
// are declared; every symbol is bound to the original by build_switchusb.py,
// which also checks each compiled slot against the original vtable.
// Slot names come from the symbolized macOS 27 IOUSBDeviceFamily (same
// sources); "opaqueNNN" slots are inherited unchanged and never called.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSArray.h>

struct _IOUSBDeviceIORequestTag;  // 0x68-byte family request (see SwitchXUDC.cpp)
class IOUSBDeviceControlRequest;

#define USBDF_OPAQUE(n) virtual __attribute__((deprecated("inherited original slot: never call"))) void opaque##n()

class IOUSBDeviceController : public IOService {
    OSDeclareAbstractStructors(
        IOUSBDeviceController) unsigned char originalState[0x228 - 0x88];  // private fields, used only by original code
public:
    // IOService slots the original overrides (14, 44, 86, 87, 117).
    void free() override;
    IOReturn setProperties(OSObject *) override;
    bool start(IOService *) override;
    void stop(IOService *) override;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) override;

    USBDF_OPAQUE(168);                                                  // createPipe
    USBDF_OPAQUE(169);                                                  // activatePipe
    USBDF_OPAQUE(170);                                                  // deactivatePipe
    USBDF_OPAQUE(171);                                                  // getAddress
    USBDF_OPAQUE(172);                                                  // createPipeGated
    USBDF_OPAQUE(173);                                                  // activatePipeGated
    USBDF_OPAQUE(174);                                                  // deactivatePipeGated
    virtual void softDisconnect(uint32_t reason) = 0;                   // 175
    USBDF_OPAQUE(176);                                                  // endpointTimeout
    virtual void goOnBus(bool on) = 0;                                  // 177
    USBDF_OPAQUE(178);                                                  // handleUserlandCommand
    virtual bool connectedWithAccessoryCable() = 0;                     // 179
    virtual IOReturn handleStallEndpoint(int address, bool stall) = 0;  // 180
    virtual IOReturn handleAbortEndpoint(int address) = 0;              // 181
    virtual IOReturn handleSetAddressPreStatusPhase(int address) = 0;   // 182
    virtual IOReturn handleSetAddressPostStatusPhase(int address) = 0;  // 183
    virtual int getConnectionSpeed() = 0;                               // 184: 1 FS, 2 HS, -1 none
    virtual IOReturn provideEndpointIDsForConfiguration(OSArray *interfaces, uint8_t *ids) = 0;                   // 185
    virtual IOReturn prepareConfiguration(OSArray *interfaces) = 0;                                               // 186
    virtual IOReturn activateEndpoint(int address, int type, int maxPacketSize, int interval, int property) = 0;  // 187
    virtual IOReturn deactivateEndpoint(int address) = 0;                                                         // 188
    virtual IOBufferMemoryDescriptor *createIOBuffer(IOOptionBits options, uint64_t capacity) = 0;                // 189
    USBDF_OPAQUE(190);                                          // setCurrentLimit
    USBDF_OPAQUE(191);                                          // getCurrentLimit
    virtual bool isEndpointStalled(int address) = 0;            // 192
    virtual IOReturn resetEndpointDataToggle(int address) = 0;  // 193
    USBDF_OPAQUE(194);                                          // handleTestMode
    virtual void handleUSBSuspend();                            // 195
    virtual void handleUSBReset();                              // 196
    virtual void handleUSBConnectionDone();                     // 197
    virtual void handleUSBResume();                             // 198
    virtual void forceReEnumeration() = 0;                      // 199
    USBDF_OPAQUE(200);                                          // forceControllerReinit
    USBDF_OPAQUE(201);                                          // handleSetConfiguration
    virtual IOReturn doRemoteWakeGated() = 0;                   // 202
    USBDF_OPAQUE(203);                                          // doFunctionRemoteWakeGated
    virtual IOReturn handleUSBCableConnect();                   // 204
    USBDF_OPAQUE(205);                                          // handleUSBCableDisconnect
    USBDF_OPAQUE(206);                                          // handleUSBSoftDisconnect
    USBDF_OPAQUE(207);                                          // getMaxSpeedCapability (returns 2, high speed)
    USBDF_OPAQUE(208);                                          // getMinSpeedCapability
    USBDF_OPAQUE(209);                                          // getDeviceAddress
    USBDF_OPAQUE(210);                                          // getConfiguration
    USBDF_OPAQUE(211);
    USBDF_OPAQUE(212);                                                         // filterPacket
    USBDF_OPAQUE(213);                                                         // addDevCapabilityDescriptors
    USBDF_OPAQUE(214);                                                         // hardwareException
    virtual void handleControlRequestStateChange(int oldState, int newState);  // 215
    USBDF_OPAQUE(216);                                                         // gotoPoweredState
    USBDF_OPAQUE(217);                                                         // gotoDefaultState
    USBDF_OPAQUE(218);                                                         // gotoConfiguredState
    USBDF_OPAQUE(219);                                                         // gotoDisconnectedState
    USBDF_OPAQUE(220);                                                         // gotoDeviceState
    USBDF_OPAQUE(221);                                                         // startUSBStack
    USBDF_OPAQUE(222);                                                         // setupDriverRequest
    USBDF_OPAQUE(223);                                                         // createControlRequestTracker
    USBDF_OPAQUE(224);                                                         // prepareDefaults
    USBDF_OPAQUE(225);                                                         // io
    USBDF_OPAQUE(226);                                                         // io (isochronous)
    virtual IOReturn ioGated(_IOUSBDeviceIORequestTag *request) = 0;           // 227
    USBDF_OPAQUE(228);                                                         // allocateRequest
    USBDF_OPAQUE(229);                                                         // freeRequest
    USBDF_OPAQUE(230);                                                         // getDMACommand
    USBDF_OPAQUE(231);                                                         // setupDeviceSetSELDataCompletion
    USBDF_OPAQUE(232);                                                         // updateLPMPolicy
    virtual IOReturn getBufferOptions(uint32_t &options);                      // 233

    // Non-virtual original members.
    void readyForHost();                                         // creates the control request, registerService()
    void completeEndpointIO(_IOUSBDeviceIORequestTag *request);  // completion + freeRequest

    // Original field: the EP0 control-transfer state machine (created by readyForHost).
    IOUSBDeviceControlRequest *controlRequest() const
    {
        IOUSBDeviceControlRequest *request;
        __builtin_memcpy(&request, reinterpret_cast<const unsigned char *>(this) + 432, sizeof(request));
        return request;
    }
};
static_assert(sizeof(IOService) == 0x88, "Pinned original IOService base size");
static_assert(sizeof(IOUSBDeviceController) == 0x228, "Pinned original IOUSBDeviceController size");

// EP0 state machine (OSObject, 0xa0 bytes). States at +12: 1 idle, 2 setup
// received, 3 IN request, 4 OUT request with data, 5/6 OUT data stage,
// 7 waiting for the device's status (IN zero-length), 8 IN data done and
// waiting for the host's status (OUT zero-length), 9 stalled.
class IOUSBDeviceControlRequest {
public:
    void inputSetupPacket(uint8_t *packet);
    void processSetupPacket();
    void inputDataPacket(uint8_t *bytes, uint64_t length);
    void stallSetupRequest();
    void reset();
    int state() const
    {
        return *reinterpret_cast<const volatile int *>(reinterpret_cast<const unsigned char *>(this) + 12);
    }
    uint16_t setupLength() const
    {
        const unsigned char *setup = reinterpret_cast<const unsigned char *>(this) + 16;
        return static_cast<uint16_t>(setup[6] | setup[7] << 8);
    }

private:
    IOUSBDeviceControlRequest() = delete;
};

// Function interface and pipe accessors used to assign endpoint addresses.
class IOUSBDeviceInterface {
public:
    int getAlternateSettingCount() const;
    int getPipeCountForAlternateSetting(uint8_t alternate);
    class IOUSBDevicePipe *getPipeForAlternateSetting(int index, uint8_t alternate);

private:
    IOUSBDeviceInterface() = delete;
};

class IOUSBDevicePipe {
public:
    int getDirection();  // 0 OUT, 1 IN
private:
    IOUSBDevicePipe() = delete;
};

#undef USBDF_OPAQUE
