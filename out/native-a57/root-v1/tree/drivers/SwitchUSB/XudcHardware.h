// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra X1 (T210B01) XUSB device controller (XUDC): clocks, power, USB2 PHY
// on OTG pad 0 (the USB-C port), event ring, endpoint contexts and transfer
// rings. Shared by the CDC-ACM console gadget (SwitchUSB.cpp) and the
// IOUSBDeviceController backend (SwitchXUDC.cpp). USB2 high speed only.
//
// Sequence: hekate bdk/usb/xusbd.c (xusb_device_init, _xusb_init_phy,
// _xusbd_init_device_clocks, xusb_device_enumerate); endpoint pause/reload/halt
// and the context bit layout: Linux drivers/usb/gadget/udc/tegra-xudc.c.
// The caller serializes all calls (one thread or one IOWorkLoop), except
// takeInterrupt(), which only touches ST and may run in a primary interrupt
// filter. Events are interrupt-driven when the caller attaches the XUDC SPI
// (44, INTID 76; CTRL.IE and the IPFS IP_INT mask are set by powerOn()).
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSObject.h>

namespace xudc {

enum : uint32_t {
    // TRB types and flags (xHCI layout).
    TRB_NORMAL = 1,
    TRB_DATA = 3,
    TRB_STATUS = 4,
    TRB_LINK = 6,
    TRB_TRANSFER_EVENT = 32,
    TRB_PORT_EVENT = 34,
    TRB_SETUP_EVENT = 63,
    TRB_CYCLE = 1u << 0,
    TRB_TOGGLE = 1u << 1,
    TRB_ISP = 1u << 2,
    TRB_IOC = 1u << 5,
    TRB_DIR_IN = 1u << 16,
    // Completion codes.
    COMP_SUCCESS = 1,
    COMP_SHORT_PACKET = 13,
    // Endpoint context types.
    EP_ISOC_OUT = 1,
    EP_BULK_OUT = 2,
    EP_INTR_OUT = 3,
    EP_CONTROL = 4,
    EP_ISOC_IN = 5,
    EP_BULK_IN = 6,
    EP_INTR_IN = 7,
    // PORTSC.
    PORTSC_CCS = 1u << 0,
    PORTSC_PR = 1u << 4,
    PORTSC_PLS_MASK = 0xfu << 5,
    PORTSC_PLS_U0 = 0u << 5,
    PORTSC_PLS_U3 = 3u << 5,
    PORTSC_CSC = 1u << 17,
    PORTSC_WRC = 1u << 19,
    PORTSC_PRC = 1u << 21,
    PORTSC_PLC = 1u << 22,
    PORTSC_CEC = 1u << 23,
    PORTSC_WPR = 1u << 30,
};
constexpr uint32_t trbType(uint32_t type)
{
    return type << 10;
}
constexpr uint32_t kRingTrbs = 16;  // per endpoint; the last one is the link TRB
constexpr uint32_t kMaxDci = 32;

// Event TRB as copied out of the ring.
struct Event {
    uint32_t d[4];
    uint32_t type() const { return (d[3] >> 10) & 0x3f; }
    uint32_t dci() const { return (d[3] >> 16) & 0x1f; }
    uint32_t code() const { return d[2] >> 24; }
    uint32_t residue() const { return d[2] & 0xffffff; }
    uint32_t trb() const { return d[0]; }  // transfer events: TRB physical address
    uint16_t setupSequence() const { return d[2] & 0xffff; }
};

// Physically contiguous, cacheable DMA memory below 4 GiB.
struct DmaBuffer {
    IOBufferMemoryDescriptor *md = nullptr;
    uint8_t *va = nullptr;
    uint32_t pa = 0;
    uint32_t size = 0;
    bool allocate(uint32_t bytes);
    void release();
};

void cacheFlush(const volatile void *p, size_t n);  // clean+invalidate by VA, then DSB

// UTMI pad 0 bring-up (PLLU, UTMIPLL, fuse calibration, bias tracking); fusePage is 0x7000F000.
bool usb2PadInit(volatile uint8_t *car, volatile uint8_t *fusePage, volatile uint8_t *padctl);

class Hardware {
public:
    bool map();                                         // MMIO only, no access
    void report(const char *why);                       // one log line, reads only live blocks
    bool powerOn();                                     // partitions, clocks, PHY, FPCI/IPFS, event ring, EP0
    void connect();                                     // VBUS/ID override, enable, port to RxDetect
    void disconnect();                                  // drop the pull-up; the host sees a detach
    bool nextEvent(Event &event);                       // false when the ring is empty
    void finishEvents();                                // ERDP update (+ EHB clear) after a batch
    bool takeInterrupt();                               // ST.IP set? Then acknowledge it (W1C, IP only)
    bool interruptPending();                            // ST.IP, read only
    void describeInterrupt(char *buffer, size_t size);  // ST, CTRL, ERDP, EREP, IPFS mask: for a panic line

    // Endpoints (DCI = 2 * number + IN; EP0 is DCI 0).
    void initEndpoint(uint32_t dci, uint32_t type, uint32_t maxPacket, uint32_t interval);
    void disableEndpoint(uint32_t dci);
    bool reloadEndpoint(uint32_t dci);
    void setHalt(uint32_t dci, bool halt);
    bool halted(uint32_t dci);
    void resetSequence(uint32_t dci);  // CLEAR_FEATURE(HALT): data toggle 0, ring position kept
    void dropQueued(uint32_t dci);     // discard every TRB not yet completed, keep the toggle
    uint32_t push(uint32_t dci, uint32_t bufferPA, uint32_t length, uint32_t flags);  // returns TRB PA
    void doorbell(uint32_t dci, uint16_t sequence = 0);
    void setAddress(uint32_t address);
    void run();  // CTRL.RUN (first data endpoint)
    uint32_t portStatus();
    void clearPortChange(uint32_t status, uint32_t change);
    void releaseLtssm();
    bool stateChangeRequested();

private:
    struct Ring {
        volatile uint32_t *trb = nullptr;
        uint32_t pa = 0, enqueue = 0, cycle = 1;
    };
    volatile uint8_t *car = nullptr, *pmcPage = nullptr, *fusePage = nullptr, *mc = nullptr, *padctl = nullptr,
                     *xudc = nullptr;
    DmaBuffer control;  // event ring, contexts, transfer rings
    Ring rings[kMaxDci];
    uint32_t eventIndex = 0, eventCycle = 1;

    static uint32_t r(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
    static void w(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
    uint32_t pmc(uint32_t o);
    void pmcWrite(uint32_t o, uint32_t v);
    uint32_t xr(uint32_t o) { return r(xudc, o); }
    void xw(uint32_t o, uint32_t v) { w(xudc, o, v); }
    bool waitBits(volatile uint8_t *b, uint32_t o, uint32_t mask, uint32_t value, uint32_t us);
    bool powerUp(uint32_t partition, uint32_t clockSet, uint32_t resetSet, uint32_t bit);
    bool initPhy();
    volatile uint32_t *context(uint32_t dci);
    void resetRing(uint32_t dci);
    void toggleBit(uint32_t reg, uint32_t dci, bool set);
};

}  // namespace xudc
