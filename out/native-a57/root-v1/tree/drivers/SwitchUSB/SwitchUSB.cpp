// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB on the Switch's USB-C port (Tegra X1 XUDC, USB2 high speed).
//
// Boot-arg switchusb:
//   0 (default) off
//   1 log the USB clock/power/pad state once; no register writes
//   2 CDC-ACM gadget streaming the kernel console: /dev/cu.usbmodem* on a Mac
//   3 Apple USB device stack: SwitchXUDC under IOUSBDeviceFamily, so
//     AppleUSBDeviceMux appears to the Mac's usbmuxd (SwitchXUDC.cpp)
//
// The console tap (switchusb_console_write) runs in every mode so the ring
// already holds early boot when the gadget comes up.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include "SwitchUSB.h"
#include "XudcHardware.h"

using namespace xudc;

extern "C" int switchxudc_start(void);

// ---------------------------------------------------------------------------
// Console ring: filled by switchusb_console_write(), drained by the gadget.
namespace {
constexpr uint32_t kRingBytes = 256 * 1024;   // power of two
char gRing[kRingBytes];
uint64_t gRingHead;                            // bytes ever written
uint32_t gRingBusy;
uint64_t gRingDropped;
}

extern "C" void switchusb_console_write(const char *bytes, int count)
{
  if (!bytes || count <= 0) return;
  if (__atomic_exchange_n(&gRingBusy, 1, __ATOMIC_ACQUIRE)) {
    __atomic_fetch_add(&gRingDropped, (uint64_t)count, __ATOMIC_RELAXED);
    return;
  }
  uint64_t head = gRingHead, n = (uint64_t)count;
  for (uint64_t i = 0; i < n; ++i) gRing[(head + i) & (kRingBytes - 1)] = bytes[i];
  __atomic_store_n(&gRingHead, head + n, __ATOMIC_RELEASE);
  __atomic_store_n(&gRingBusy, 0, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// CDC-ACM console gadget (mode 2).
namespace {
enum : uint32_t { DCI_EP0 = 0, DCI_DATA_OUT = 2, DCI_DATA_IN = 3, DCI_NOTIFY = 5 };
const uint32_t kDataEndpoints[] = { DCI_DATA_OUT, DCI_DATA_IN, DCI_NOTIFY };
constexpr uint32_t kRxBytes = 0x1000, kTxBytes = 0x4000;

// One configuration: comm interface 0 (interrupt IN 0x82, never written),
// data interface 1 (bulk OUT 0x01, bulk IN 0x81). VID 0x1209 / PID 0x0001 is
// pid.codes' test pair; change it for anything shared.
const uint8_t kDeviceDescriptor[18] = {
  18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};
const uint8_t kQualifierDescriptor[10] = { 10, 6, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 1, 0 };
const uint8_t kConfigDescriptor[67] = {
  9, 2, 67, 0, 2, 1, 0, 0xc0, 50,
  9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,      // CDC communications, ACM, V.250
  5, 0x24, 0x00, 0x10, 0x01,               // header, CDC 1.10
  5, 0x24, 0x01, 0x00, 0x01,               // call management: data interface 1
  4, 0x24, 0x02, 0x02,                     // ACM: line coding + control line state
  5, 0x24, 0x06, 0x00, 0x01,               // union: master 0, slave 1
  7, 5, 0x82, 0x03, 16, 0, 9,              // notification, interrupt IN, 32 ms at HS
  9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,      // CDC data
  7, 5, 0x01, 0x02, 0x00, 0x02, 0,         // bulk OUT 512
  7, 5, 0x81, 0x02, 0x00, 0x02, 0,         // bulk IN 512
};
const char *const kStrings[] = { nullptr, "IOSwitch", "Switch iPadOS console", "IOSW0001" };

inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

enum class Ep0 { Idle, DataIn, DataOut, Status };
enum class Device { Default, AddressPending, Addressed, ConfigPending, Configured };
}

class SwitchUSB : public IOService {
  OSDeclareDefaultStructors(SwitchUSB)
  Hardware hw;
  DmaBuffer ep0Buffer, rxBuffer, txBuffer;
  uint32_t mode = 0;
  Ep0 ep0 = Ep0::Idle;
  Device device = Device::Default;
  uint16_t seqNum = 0;
  uint8_t setupRequest = 0, configuration = 0, controlLines = 0;
  uint8_t lineCoding[7] = { 0x00, 0xc2, 0x01, 0x00, 0x00, 0x00, 0x08 };  // 115200 8N1
  uint32_t speed = 0, maxPacket = 512;
  bool txBusy = false, rxBusy = false;
  uint32_t txLength = 0;
  uint64_t txTail = 0;

  bool pollEvents();
  void portChange();
  void busReset(bool reloadEp0);
  void transferEvent(const Event &e);
  void setupEvent(const Event &e);
  void standardRequest(const uint8_t *s);
  void classRequest(const uint8_t *s);
  void sendIn(const void *bytes, uint32_t length, uint16_t wLength);
  void receiveOut(uint32_t length);
  void status(bool in);
  void stall() { hw.setHalt(DCI_EP0, true); ep0 = Ep0::Idle; }
  void configure(uint8_t value);
  void queueReceive();
  bool pumpTransmit();

  static void worker(void *, wait_result_t);
  void run();
public:
  bool begin(uint32_t requested);
};
OSDefineMetaClassAndStructors(SwitchUSB, IOService)

bool SwitchUSB::pollEvents()
{
  Event e;
  bool any = false;
  for (uint32_t handled = 0; handled < 64 && hw.nextEvent(e); ++handled) {
    switch (e.type()) {
    case TRB_TRANSFER_EVENT: transferEvent(e); break;
    case TRB_PORT_EVENT: portChange(); break;
    case TRB_SETUP_EVENT: setupEvent(e); break;
    default: break;
    }
    any = true;
  }
  if (any) hw.finishEvents();
  return any;
}

// hekate _xusb_handle_port_change() order.
void SwitchUSB::portChange()
{
  uint32_t sc = hw.portStatus();
  bool ltssm = hw.stateChangeRequested();
  if (sc & PORTSC_PRC) {
    hw.clearPortChange(sc, PORTSC_PRC);
    if (!(sc & PORTSC_PR)) busReset(true);
  }
  if (sc & PORTSC_WPR) hw.releaseLtssm();
  if (sc & PORTSC_WRC) hw.clearPortChange(sc, PORTSC_WRC);
  sc = hw.portStatus();
  if (sc & PORTSC_CSC) {
    speed = (sc >> 10) & 0xf;  // 1 full, 3 high, 4 super
    maxPacket = speed == 1 ? 64 : 512;
    if (!(sc & PORTSC_CCS)) busReset(false);
    hw.clearPortChange(sc, PORTSC_CSC);
  }
  if (ltssm) hw.releaseLtssm();
  sc = hw.portStatus();
  if (sc & PORTSC_PLC) hw.clearPortChange(sc, PORTSC_PLC);
  sc = hw.portStatus();
  if (sc & PORTSC_CEC) hw.clearPortChange(sc, PORTSC_CEC);
}

// Bus reset or disconnect: back to the default state with a fresh EP0 ring
// (Linux tegra_xudc_reset reloads EP0 the same way). Unsent log stays queued.
void SwitchUSB::busReset(bool reloadEp0)
{
  device = Device::Default;
  ep0 = Ep0::Idle;
  configuration = 0;
  controlLines = 0;
  txBusy = rxBusy = false;
  if (!reloadEp0) return;
  hw.initEndpoint(DCI_EP0, EP_CONTROL, 64, 0);
  if (!hw.reloadEndpoint(DCI_EP0)) IOLog("SwitchUSB: EP0 reload timed out\n");
}

void SwitchUSB::transferEvent(const Event &e)
{
  uint32_t dci = e.dci(), code = e.code(), residue = e.residue();
  bool ok = code == COMP_SUCCESS || code == COMP_SHORT_PACKET;
  if (dci == DCI_EP0) {
    if (!ok) return;  // sequence-number/direction errors: a newer setup replaced this transfer
    switch (ep0) {
    case Ep0::DataIn: status(false); break;
    case Ep0::DataOut:
      cacheFlush(ep0Buffer.va, 64);
      if (setupRequest == 0x20) memcpy(lineCoding, ep0Buffer.va, sizeof(lineCoding));
      status(true);
      break;
    case Ep0::Status:
      ep0 = Ep0::Idle;
      if (device == Device::AddressPending) device = Device::Addressed;
      if (device == Device::ConfigPending) { device = Device::Configured; queueReceive(); }
      break;
    case Ep0::Idle: break;
    }
  } else if (dci == DCI_DATA_IN) {
    txBusy = false;
    if (ok && residue <= txLength) txTail += txLength - residue;
    else IOLog("SwitchUSB: bulk IN completion %u\n", code);
  } else if (dci == DCI_DATA_OUT) {
    rxBusy = false;  // host input is accepted and dropped for now
    if (!ok) IOLog("SwitchUSB: bulk OUT completion %u\n", code);
    else if (device == Device::Configured) queueReceive();
  }
}

void SwitchUSB::setupEvent(const Event &e)
{
  uint8_t s[8];
  memcpy(s, e.d, 8);
  seqNum = e.setupSequence();
  hw.setHalt(DCI_EP0, false);
  if (seqNum == 0xfffe || seqNum == 0xffff) { stall(); return; }  // Tegra210 quirk (tegra-xudc.c)
  ep0 = Ep0::Idle;
  setupRequest = s[1];
  if ((s[0] & 0x60) == 0x20) classRequest(s);
  else if ((s[0] & 0x60) == 0) standardRequest(s);
  else stall();
}

void SwitchUSB::sendIn(const void *bytes, uint32_t length, uint16_t wLength)
{
  if (length > wLength) length = wLength;
  if (length > ep0Buffer.size) length = ep0Buffer.size;
  memcpy(ep0Buffer.va, bytes, length);
  cacheFlush(ep0Buffer.va, length ? length : 1);
  ep0 = Ep0::DataIn;
  hw.push(DCI_EP0, ep0Buffer.pa, length, trbType(TRB_DATA) | TRB_ISP | TRB_IOC | TRB_DIR_IN);
  hw.doorbell(DCI_EP0, seqNum);
}

void SwitchUSB::receiveOut(uint32_t length)
{
  cacheFlush(ep0Buffer.va, 64);
  ep0 = Ep0::DataOut;
  hw.push(DCI_EP0, ep0Buffer.pa, length, trbType(TRB_DATA) | TRB_ISP | TRB_IOC);
  hw.doorbell(DCI_EP0, seqNum);
}

void SwitchUSB::status(bool in)
{
  ep0 = Ep0::Status;
  hw.push(DCI_EP0, 0, 0, trbType(TRB_STATUS) | TRB_IOC | (in ? TRB_DIR_IN : 0));
  hw.doorbell(DCI_EP0, seqNum);
}

void SwitchUSB::standardRequest(const uint8_t *s)
{
  uint8_t type = s[0], request = s[1];
  uint16_t value = le16(s + 2), index = le16(s + 4), length = le16(s + 6);
  uint8_t reply[255];
  uint32_t dci = (index & 0xf) * 2 + ((index & 0x80) ? 1 : 0);
  if (dci == 1) dci = DCI_EP0;

  switch (type << 8 | request) {
  case 0x8006: {  // GET_DESCRIPTOR
    uint8_t kind = value >> 8, which = value & 0xff;
    if (kind == 1) return sendIn(kDeviceDescriptor, sizeof(kDeviceDescriptor), length);
    if (kind == 6) return sendIn(kQualifierDescriptor, sizeof(kQualifierDescriptor), length);
    if (kind == 2) {
      memcpy(reply, kConfigDescriptor, sizeof(kConfigDescriptor));
      for (uint32_t at = 0; at + 7 <= sizeof(kConfigDescriptor); at += reply[at])
        if (reply[at + 1] == 5 && reply[at + 3] == 2) { reply[at + 4] = (uint8_t)maxPacket; reply[at + 5] = (uint8_t)(maxPacket >> 8); }
      return sendIn(reply, sizeof(kConfigDescriptor), length);
    }
    if (kind == 3 && which == 0) { const uint8_t lang[4] = { 4, 3, 0x09, 0x04 }; return sendIn(lang, 4, length); }
    if (kind == 3 && which < sizeof(kStrings) / sizeof(kStrings[0])) {
      uint32_t n = 2;
      for (const char *p = kStrings[which]; *p && n + 2 <= sizeof(reply); ++p) { reply[n++] = (uint8_t)*p; reply[n++] = 0; }
      reply[0] = (uint8_t)n; reply[1] = 3;
      return sendIn(reply, n, length);
    }
    return stall();
  }
  case 0x8000: reply[0] = 1; reply[1] = 0; return sendIn(reply, 2, length);  // self powered
  case 0x8100: case 0x810a: reply[0] = reply[1] = 0; return sendIn(reply, request == 0 ? 2 : 1, length);
  case 0x8200:
    if (dci >= kMaxDci) return stall();
    reply[0] = hw.halted(dci); reply[1] = 0;
    return sendIn(reply, 2, length);
  case 0x8008: reply[0] = configuration; return sendIn(reply, 1, length);
  case 0x0005:  // SET_ADDRESS: CTRL.DEVADDR and the EP0 context's device address
    hw.setAddress(value & 0x7f);
    device = Device::AddressPending;
    return status(true);
  case 0x0009: configure(value & 0xff); return;
  case 0x0001: case 0x0003: case 0x010b: return status(true);  // device features, SET_INTERFACE
  case 0x0201: case 0x0203:  // CLEAR/SET_FEATURE(ENDPOINT_HALT)
    if (value != 0 || (dci != DCI_EP0 && dci != DCI_DATA_OUT && dci != DCI_DATA_IN && dci != DCI_NOTIFY)) return stall();
    if (dci != DCI_EP0) { if (request == 3) hw.setHalt(dci, true); else hw.resetSequence(dci); }
    return status(true);
  default: return stall();
  }
}

void SwitchUSB::classRequest(const uint8_t *s)
{
  uint16_t value = le16(s + 2), length = le16(s + 6);
  switch (s[0] << 8 | s[1]) {
  case 0x2120: if (length != sizeof(lineCoding)) return stall(); return receiveOut(length);  // SET_LINE_CODING
  case 0xa121: return sendIn(lineCoding, sizeof(lineCoding), length);                          // GET_LINE_CODING
  case 0x2122: controlLines = value & 3; return status(true);                                  // SET_CONTROL_LINE_STATE
  case 0x2123: return status(true);                                                            // SEND_BREAK
  default: return stall();
  }
}

// SET_CONFIGURATION: data endpoints (and the idle notification endpoint) are
// (re)initialized, then the controller runs (hekate _xusb_handle_set_request_configuration).
void SwitchUSB::configure(uint8_t value)
{
  if (value > 1) return stall();
  configuration = value;
  txBusy = rxBusy = false;
  if (!value) {
    for (uint32_t dci : kDataEndpoints) hw.setHalt(dci, true);
    device = Device::Addressed;
    return status(true);
  }
  hw.initEndpoint(DCI_DATA_OUT, EP_BULK_OUT, maxPacket, 0);
  hw.initEndpoint(DCI_DATA_IN, EP_BULK_IN, maxPacket, 0);
  hw.initEndpoint(DCI_NOTIFY, EP_INTR_IN, 16, speed == 1 ? 6 : 8);  // 2^n x 125 us
  for (uint32_t dci : kDataEndpoints)
    if (!hw.reloadEndpoint(dci)) { IOLog("SwitchUSB: endpoint %u reload timed out\n", dci); return stall(); }
  hw.run();
  device = Device::ConfigPending;
  status(true);
}

void SwitchUSB::queueReceive()
{
  if (rxBusy) return;
  cacheFlush(rxBuffer.va, rxBuffer.size);
  hw.push(DCI_DATA_OUT, rxBuffer.pa, rxBuffer.size, trbType(TRB_NORMAL) | TRB_ISP | TRB_IOC);
  hw.doorbell(DCI_DATA_OUT);
  rxBusy = true;
}

// Next console chunk. Lengths are never a multiple of the packet size, so every
// transfer ends with a short packet and no zero-length packet is needed.
bool SwitchUSB::pumpTransmit()
{
  if (device != Device::Configured || txBusy) return false;
  uint64_t head = __atomic_load_n(&gRingHead, __ATOMIC_ACQUIRE);
  if (head - txTail > kRingBytes) txTail = head - kRingBytes;  // overrun: oldest bytes lost
  uint64_t available = head - txTail;
  if (!available) return false;
  uint32_t n = available > txBuffer.size ? txBuffer.size : (uint32_t)available;
  if (n % maxPacket == 0) n--;
  for (uint32_t i = 0; i < n; ++i) txBuffer.va[i] = (uint8_t)gRing[(txTail + i) & (kRingBytes - 1)];
  cacheFlush(txBuffer.va, n);
  txLength = n;
  hw.push(DCI_DATA_IN, txBuffer.pa, n, trbType(TRB_NORMAL) | TRB_ISP | TRB_IOC);
  hw.doorbell(DCI_DATA_IN);
  txBusy = true;
  return true;
}

void SwitchUSB::run()
{
  hw.report(mode == 1 ? "state" : "starting");
  if (mode != 2) return;
  if (!hw.powerOn()) { hw.report("bring-up failed"); return; }
  hw.connect();
  for (;;) {
    bool busy = pollEvents();
    busy |= pumpTransmit();
    if (!busy) IOSleep(1);
  }
}

void SwitchUSB::worker(void *self, wait_result_t)
{
  static_cast<SwitchUSB *>(self)->run();
}

bool SwitchUSB::begin(uint32_t requested)
{
  mode = requested;
  if (!hw.map()) { IOLog("SwitchUSB: MMIO map failed\n"); return false; }
  if (mode == 2 && (!ep0Buffer.allocate(0x1000) || !rxBuffer.allocate(kRxBytes) || !txBuffer.allocate(kTxBytes))) {
    IOLog("SwitchUSB: DMA allocation failed\n"); return false;
  }
  thread_t thread = THREAD_NULL;
  if (kernel_thread_start(&SwitchUSB::worker, this, &thread) != KERN_SUCCESS) {
    IOLog("SwitchUSB: worker start failed\n"); return false;
  }
  thread_deallocate(thread);
  return true;
}

extern "C" int switchusb_start(void)
{
  static uint32_t started;
  uint32_t mode = 0;
  if (!PE_parse_boot_argn("switchusb", &mode, sizeof(mode)) || mode == 0 || mode > 3) return 0;
  if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return 0;
  if (mode == 3) return switchxudc_start();
  IOService *resources = IOService::getResourceService();
  auto *usb = new SwitchUSB;
  if (!resources || !usb || !usb->init() || !usb->attach(resources)) {
    if (usb) usb->release();
    IOLog("SwitchUSB: service setup failed\n");
    return -1;
  }
  if (!usb->begin(mode)) return -1;  // stays attached: the mappings live for the boot
  usb->registerService();
  return 0;
}
