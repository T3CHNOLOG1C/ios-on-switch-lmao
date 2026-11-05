// SPDX-License-Identifier: BSD-2-Clause-Patent
// Broadcom msgbuf protocol over PCIe for the BCM4356A3's FullMAC firmware: rings in host memory, their indices in
// the chip's RAM, doorbells through the PCIe2 core's mailbox, completions announced by MSI.
//
// Ported from MyEcoria/nxk user/wlan/src/msgbuf.rs (MIT OR Apache-2.0), itself Linux brcmfmac pcie.c,
// commonring.c and msgbuf.c; message layouts checked against CTCaer's switch-l4t-kernel-4.9 brcmfmac (shared area
// v5, the pre-v7 completion item sizes): control path (ioctls, firmware events), received frames, and one TX flow
// ring for interface 0 (a station needs no more: brcmfmac's STA mode keys flow rings on the interface alone).
//
// Memory: the AFI is not cache coherent, and IOBufferMemoryDescriptor hands out a cacheable kernel mapping, so
// every buffer is cleaned/invalidated by hand (SwitchWLANPrivate.h). Completion rings need one more rule. nxk
// notes the write index in RAM can be seen before the item it announces has landed in host memory, and zeroes each
// item it consumed so a zero message type means "not landed yet" (no message type is 0). Zeroing an item with the
// CPU dirties a whole 64-byte cache line, which holds several 16/24/32-byte items; evicting that line later would
// overwrite items the chip wrote meanwhile. So lines are zeroed (and cleaned out of the cache) only once every
// item in them has been consumed, and the read index the chip sees advances only past zeroed lines: the chip never
// writes into a line the CPU may still hold dirty, and every slot it writes was zero before.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include "SwitchWLANPrivate.h"

namespace swlan {
namespace {

// Common rings (brcmf_ring_max_item / brcmf_ring_itemsize, shared v5): two host-to-dongle, three back.
enum : unsigned {
    CONTROL_SUBMIT,
    RXPOST_SUBMIT,
    CONTROL_COMPLETE,
    TX_COMPLETE,
    RX_COMPLETE,
    RING_COUNT,
    FLOW = RING_COUNT
};
constexpr uint16_t kRingItems[RING_COUNT] = {64, 512, 64, 1024, 512};
constexpr uint16_t kRingItemSize[RING_COUNT] = {40, 32, 24, 16, 32};

// Shared area fields (BRCMF_SHARED_*) and the ring info block (BRCMF_SHARED_RING_*).
enum : uint32_t {
    SHARED_MAX_RXBUFPOST = 34,
    SHARED_RX_DATAOFFSET = 36,
    SHARED_DTOH_MB_DATA = 44,
    SHARED_RING_INFO = 48,
    SHARED_SCRATCH_LEN = 52,
    SHARED_SCRATCH_ADDR = 56,
    SHARED_RINGUPD_LEN = 64,
    SHARED_RINGUPD_ADDR = 68,
};
enum : uint32_t { RINGINFO_MEM = 0, RINGINFO_H2D_W = 4, RINGINFO_H2D_R = 8, RINGINFO_D2H_W = 12, RINGINFO_D2H_R = 16 };
constexpr uint32_t kRingMemSize = 16, kRingMaxItem = 4, kRingLenItems = 6, kRingBaseAddr = 8;
constexpr uint32_t kSharedHostReadyDb1 = 0x10000000;

// PCIe2 core mailbox (core rev < 64).
constexpr uint32_t kMailboxInt = 0x48, kMailboxMask = 0x4C, kH2DMailbox0 = 0x140, kH2DMailbox1 = 0x144;
constexpr uint32_t kMbFn0 = 0x0300, kMbD2HDoorbells = 0x00FF0000;
constexpr uint32_t kD2HFwHalt = 0x10000000;

// Message types (brcmfmac msgbuf.c MSGBUF_TYPE_*).
enum : uint8_t {
    MSG_FLOW_RING_CREATE = 0x03,
    MSG_FLOW_RING_CREATE_CMPLT = 0x04,
    MSG_FLOW_RING_DELETE = 0x05,
    MSG_FLOW_RING_DELETE_CMPLT = 0x06,
    MSG_IOCTLPTR_REQ = 0x09,
    MSG_IOCTLPTR_REQ_ACK = 0x0A,
    MSG_IOCTLRESP_BUF_POST = 0x0B,
    MSG_IOCTL_CMPLT = 0x0C,
    MSG_EVENT_BUF_POST = 0x0D,
    MSG_WL_EVENT = 0x0E,
    MSG_TX_POST = 0x0F,
    MSG_TX_STATUS = 0x10,
    MSG_RXBUF_POST = 0x11,
    MSG_RX_CMPLT = 0x12,
};
constexpr uint32_t kIoctlReqPktId = 0xFFFE;
constexpr uint32_t kControlBuffers = 8, kControlBufferSize = 8192;  // ioctl responses 0..7, events 8..15
constexpr uint32_t kIoctlMax = 1518;                                // BRCMF_TX_IOCTL_MAX_MSG_SIZE
constexpr uint32_t kRxBufferSize = 2048, kTxBufferSize = 2048;      // BRCMF_MSGBUF_MAX_PKT_SIZE
// The TX flow ring: its id follows the two common H2D rings (BRCMF_H2D_MSGRING_FLOWRING_IDSTART), its indices
// follow theirs in RAM; 512 items of the pre-v7 48-byte TX post.
constexpr uint16_t kFlowRingId = 2, kFlowItems = 512, kFlowItemSize = 48;
// The AWDL interface's flow rings follow the station's (ids 3..), smaller: AirDrop's traffic is a few peers.
constexpr uint16_t kAwdlFlowItems = 128;
constexpr uint32_t kRingInfoMaxFlowRings = 52;  // brcmf_pcie_dhi_ringinfo.max_flowrings (v5: all submission rings)
constexpr uint32_t kEthHeader = 14;
constexpr uint8_t kPktFrame8023 = 0x01;  // BRCMF_MSGBUF_PKT_FLAGS_FRAME_802_3, priority 0
constexpr uint32_t kScratchLength = 8, kRingUpdLength = 1024;
constexpr uint32_t kAreaLength = 512 * 1024;
constexpr uint32_t kLandingSpinUs = 10000;

// Firmware event frames (brcmf_fweh_process_skb): Ethernet header, 10-byte Broadcom header, the big-endian event
// message, then its data.
constexpr uint16_t kEthPLinkCtl = 0x886C;
constexpr uint32_t kEventMsg = 14 + 10, kEventData = kEventMsg + 48;

}  // namespace

bool Msgbuf::allocate(uint32_t length, uint32_t align, Buffer &out)
{
    const uint32_t at = (areaUsed_ + align - 1) & ~(align - 1);
    if (at + length > kAreaLength) return false;
    areaUsed_ = at + length;
    out.va = area_ + at;
    out.pa = areaPA_ + at;
    return true;
}

bool Msgbuf::attach(Chip *chip, EventHandler events, FrameHandler frames, void *context, char *why, size_t whyLength)
{
    chip_ = chip;
    events_ = events;
    frames_ = frames;
    context_ = context;
    const Mmio &tcm = chip->tcm();
    const uint32_t shared = chip->shared();
    const Core *pcie2 = chip->find(0x83C);
    pcie2_ = pcie2 ? pcie2->base : 0;

    // One physically contiguous area below 4 GiB, zeroed and out of the caches before the chip learns of it.
    if (!memory_) {
        auto *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
                static_cast<IOOptionBits>(kIOMemoryMapperNone),
            kAreaLength, 0x00000000FFFFF000ULL);
        IOByteCount segment = 0;
        const uint64_t pa = memory ? memory->getPhysicalSegment(0, &segment, kIOMemoryMapperNone) : 0;
        if (!memory || !pa || segment < kAreaLength || pa + kAreaLength > (1ULL << 32)) {
            snprintf(why, whyLength, "DMA area: %u bytes contiguous below 4 GiB not available (pa %#llx, run %llu)",
                     kAreaLength, pa, static_cast<unsigned long long>(segment));
            OSSafeReleaseNULL(memory);
            return false;
        }
        memory_ = memory;
        area_ = static_cast<uint8_t *>(memory->getBytesNoCopy());
        areaPA_ = static_cast<uint32_t>(pa);
    }
    areaUsed_ = 0;
    bzero(area_, kAreaLength);
    dcacheFlush(area_, kAreaLength);
    flow_ = FLOW_CLOSED;
    flowStatus_ = 0;
    replyReady_ = false;
    awdlRings_ = 0;
    awdlRefused_ = 0;
    for (auto &f : awdl_) f = {};

    Buffer msi;
    allocate(4096, 4096, msi);  // the MSI target page: the AFI decodes writes to it, memory never sees them
    msiPA_ = msi.pa;

    const uint32_t info = tcm.r32(shared + SHARED_RING_INFO);
    uint32_t mem = tcm.r32(info + RINGINFO_MEM);
    const uint32_t h2dW = tcm.r32(info + RINGINFO_H2D_W), h2dR = tcm.r32(info + RINGINFO_H2D_R);
    const uint32_t d2hW = tcm.r32(info + RINGINFO_D2H_W), d2hR = tcm.r32(info + RINGINFO_D2H_R);
    for (unsigned k = 0; k <= RING_COUNT; ++k) {
        const uint16_t items = k == FLOW ? kFlowItems : kRingItems[k],
                       size = k == FLOW ? kFlowItemSize : kRingItemSize[k];
        Buffer b;
        // 64-byte aligned, so ring offsets and cache lines line up (the completion-ring rule above relies on it).
        if (!allocate(static_cast<uint32_t>(items) * size, 64, b)) {
            snprintf(why, whyLength, "DMA area too small for ring %u", k);
            return false;
        }
        if (k != FLOW) {  // the flow ring is described to the firmware by its create request instead
            tcm.w32(mem + kRingBaseAddr, b.pa);
            tcm.w32(mem + kRingBaseAddr + 4, 0);
            tcm.w16(mem + kRingMaxItem, items);
            tcm.w16(mem + kRingLenItems, size);
            mem += kRingMemSize;
        }
        // H2D indices: control submit, rx post, then the flow rings; D2H: the three completion rings.
        const bool h2d = k < 2 || k == FLOW;
        const uint32_t slot = k == FLOW ? 4 * kFlowRingId : (k < 2 ? 4 * k : 4 * (k - 2));
        rings_[k] = {b.va, b.pa, items, size, (h2d ? h2dW : d2hW) + slot, (h2d ? h2dR : d2hR) + slot, 0, 0, 0};
    }
    Buffer scratch, ringUpd;
    if (!allocate(kScratchLength, 64, scratch) || !allocate(kRingUpdLength, 64, ringUpd) ||
        !allocate(kIoctlMax + 18, 64, ioctlBuffer_)) {
        snprintf(why, whyLength, "DMA area too small");
        return false;
    }
    tcm.w32(shared + SHARED_SCRATCH_ADDR, scratch.pa);
    tcm.w32(shared + SHARED_SCRATCH_ADDR + 4, 0);
    tcm.w32(shared + SHARED_SCRATCH_LEN, kScratchLength);
    tcm.w32(shared + SHARED_RINGUPD_ADDR, ringUpd.pa);
    tcm.w32(shared + SHARED_RINGUPD_ADDR + 4, 0);
    tcm.w32(shared + SHARED_RINGUPD_LEN, kRingUpdLength);
    for (auto &buffer : control_) {
        if (!allocate(kControlBufferSize, 64, buffer)) {
            snprintf(why, whyLength, "DMA area too small for control buffers");
            return false;
        }
    }
    for (auto &t : tx_) {
        t.busy = false;
        t.status = 0;
        if (!allocate(kTxBufferSize, 64, t.b)) {
            snprintf(why, whyLength, "DMA area too small for transmit buffers");
            return false;
        }
    }
    // As many receive buffers as the firmware takes, up to ours.
    const uint16_t maxRx = tcm.r16(shared + SHARED_MAX_RXBUFPOST);
    rxCount_ = sizeof(rx_) / sizeof(rx_[0]);
    if (maxRx && maxRx < rxCount_) rxCount_ = maxRx;
    for (unsigned i = 0; i < rxCount_; ++i) {
        if (!allocate(kRxBufferSize, 64, rx_[i])) {
            rxCount_ = i;
            break;
        }
    }
    dataOffset_ = tcm.r32(shared + SHARED_RX_DATAOFFSET);
    dtohMailbox_ = tcm.r32(shared + SHARED_DTOH_MB_DATA);

    // The AWDL flow rings, last, so none of the station's memory moves; as many as the firmware's flow rings
    // (v5: max_flowrings counts the two common submission rings too) and the area allow. Unused until AWDL runs.
    const uint16_t submissionRings = tcm.r16(info + kRingInfoMaxFlowRings);
    maxFlowRings_ = submissionRings > 2 ? static_cast<uint16_t>(submissionRings - 2) : 0;
    for (unsigned k = 0; k < kAwdlFlows && 1u + k < maxFlowRings_; ++k) {
        Buffer b;
        if (!allocate(static_cast<uint32_t>(kAwdlFlowItems) * kFlowItemSize, 64, b) ||
            !allocate(kTxBufferSize, 64, awdl_[k].pending))
            break;
        const uint32_t slot = 4u * (kFlowRingId + 1 + k);
        rings_[FLOW + 1 + k] = {b.va, b.pa, kAwdlFlowItems, kFlowItemSize, h2dW + slot, h2dR + slot, 0, 0, 0};
        awdlRings_ = k + 1;
    }

    for (uint32_t id = 0; id < 2 * kControlBuffers; ++id) {
        if (!postControl(id)) {
            snprintf(why, whyLength, "control submit ring full at buffer %u", id);
            return false;
        }
    }
    for (uint32_t id = 0; id < rxCount_; ++id) {
        if (!postRx(id)) {
            snprintf(why, whyLength, "rx post ring full at buffer %u", id);
            return false;
        }
    }
    return true;
}

void Msgbuf::enableInterrupts()
{
    chip_->write(pcie2_ + kMailboxMask, kMbD2HDoorbells | kMbFn0);
    // brcmf_pcie_hostready: some firmware waits for this doorbell before it serves the rings.
    if (chip_->sharedFlags() & kSharedHostReadyDb1) chip_->write(pcie2_ + kH2DMailbox1, 1);
}

void Msgbuf::disableInterrupts()
{
    chip_->write(pcie2_ + kMailboxMask, 0);
}

bool Msgbuf::chipSignalling(uint32_t *mailboxInt, uint32_t *mailboxMask)
{
    *mailboxInt = chip_->read(pcie2_ + kMailboxInt);
    *mailboxMask = chip_->read(pcie2_ + kMailboxMask);
    return *mailboxInt != 0xFFFFFFFF && (*mailboxInt & *mailboxMask) != 0;
}

// Room for one more item on a host-to-dongle ring (one slot always stays free); nullptr when full.
uint8_t *Msgbuf::reserve(unsigned ring)
{
    Ring &r = rings_[ring];
    if ((r.pos + 1) % r.items == chip_->tcm().r16(r.rIndex)) return nullptr;
    uint8_t *item = r.va + static_cast<uint32_t>(r.pos) * r.size;
    memset(item, 0, r.size);
    return item;
}

// Item written: out of the cache, then the new write index, then the doorbell.
void Msgbuf::commit(unsigned ring)
{
    Ring &r = rings_[ring];
    dcacheClean(r.va + static_cast<uint32_t>(r.pos) * r.size, r.size);
    r.pos = static_cast<uint16_t>((r.pos + 1) % r.items);
    chip_->tcm().w16(r.wIndex, r.pos);
    chip_->write(pcie2_ + kH2DMailbox0, 1);
}

// A control buffer back to the firmware: ioctl responses (ids 0..7) or events (8..15). Flushed first, so no line
// of it is dirty in the cache while the chip writes it.
bool Msgbuf::postControl(uint32_t id)
{
    dcacheFlush(control_[id].va, kControlBufferSize);
    uint8_t *p = reserve(CONTROL_SUBMIT);
    if (!p) return false;
    p[0] = id < kControlBuffers ? MSG_IOCTLRESP_BUF_POST : MSG_EVENT_BUF_POST;
    put32(p + 4, id);
    put16(p + 8, static_cast<uint16_t>(kControlBufferSize));
    put32(p + 16, control_[id].pa);
    put32(p + 20, 0);
    commit(CONTROL_SUBMIT);
    return true;
}

// brcmf_msgbuf_rxbuf_data_post.
bool Msgbuf::postRx(uint32_t id)
{
    dcacheFlush(rx_[id].va, kRxBufferSize);
    uint8_t *p = reserve(RXPOST_SUBMIT);
    if (!p) return false;
    p[0] = MSG_RXBUF_POST;
    put32(p + 4, id);
    put16(p + 10, static_cast<uint16_t>(kRxBufferSize));
    put32(p + 24, rx_[id].pa);
    put32(p + 28, 0);
    commit(RXPOST_SUBMIT);
    return true;
}

// Next completion of `ring`, copied out, if the firmware announced one. An announced item still reading zero is
// given 10 ms to land (see the top of this file); past that the DMA path is broken, which a panic names.
bool Msgbuf::next(unsigned ring, uint8_t *item)
{
    Ring &r = rings_[ring];
    const uint16_t write = chip_->tcm().r16(r.wIndex);
    if (write == r.pos) return false;
    if (write >= r.items) {  // a chip gone from the bus reads all ones: never chase that index
        IOLog("SwitchWLAN: ring %u write index %#x of %u items, chip gone?\n", ring, write, r.items);
        return false;
    }
    uint8_t *p = r.va + static_cast<uint32_t>(r.pos) * r.size;
    dcacheFlush(p, r.size);
    for (uint32_t waited = 0; !p[0]; waited += 10) {
        if (waited >= kLandingSpinUs) {
            panic("SwitchWLAN: ring %u item %u announced (write index %u) but still empty after %u ms: the chip's DMA "
                  "writes are not reaching memory (area %#x)",
                  ring, r.pos, write, kLandingSpinUs / 1000, areaPA_);
        }
        IODelay(10);
        dcacheFlush(p, r.size);
    }
    memcpy(item, p, r.size);
    r.pos = static_cast<uint16_t>((r.pos + 1) % r.items);
    r.consumed += r.size;
    return true;
}

// Lines whose items are all consumed: zeroed and cleaned out of the cache, then the read index advanced to the
// first item not wholly inside them.
void Msgbuf::retire(unsigned ring)
{
    Ring &r = rings_[ring];
    const uint32_t bytes = static_cast<uint32_t>(r.items) * r.size;
    const uint64_t target = r.consumed & ~static_cast<uint64_t>(kCacheLine - 1);
    if (r.cleared >= target) return;
    while (r.cleared < target) {
        uint8_t *line = r.va + static_cast<uint32_t>(r.cleared % bytes);
        memset(line, 0, kCacheLine);
        dcacheFlush(line, kCacheLine);
        r.cleared += kCacheLine;
    }
    chip_->tcm().w16(r.rIndex, static_cast<uint16_t>((r.cleared / r.size) % r.items));
}

void Msgbuf::drain()
{
    uint8_t item[40];
    while (next(CONTROL_COMPLETE, item)) {
        const uint32_t id = le32(item + 4);
        if (item[0] == MSG_IOCTL_CMPLT && id < kControlBuffers) {
            size_t length = le16(item + 12);
            if (length > sizeof(reply_)) length = sizeof(reply_);
            dcacheFlush(control_[id].va, length);
            memcpy(reply_, control_[id].va, length);
            replyLength_ = length;
            replyStatus_ = static_cast<int16_t>(le16(item + 8));
            replyReady_ = true;
            postControl(id);
        } else if (item[0] == MSG_WL_EVENT && id >= kControlBuffers && id < 2 * kControlBuffers) {
            uint32_t length = le16(item + 12);
            if (dataOffset_ + length > kControlBufferSize) length = kControlBufferSize - dataOffset_;
            const uint8_t *frame = control_[id].va + dataOffset_;
            dcacheFlush(frame, length);
            if (length >= kEventData && ((frame[12] << 8) | frame[13]) == kEthPLinkCtl && events_) {
                Event e;
                e.flags = (static_cast<uint32_t>(frame[kEventMsg + 2]) << 8) | frame[kEventMsg + 3];
                e.type = be32(frame + kEventMsg + 4);
                e.status = be32(frame + kEventMsg + 8);
                e.reason = be32(frame + kEventMsg + 12);
                const uint32_t dataLength = be32(frame + kEventMsg + 20);
                memcpy(e.addr, frame + kEventMsg + 24, 6);
                e.ifidx = frame[kEventMsg + 46];  // after the 16-byte interface name
                e.bsscfgidx = frame[kEventMsg + 47];
                e.data = frame + kEventData;
                e.length = dataLength < length - kEventData ? dataLength : length - kEventData;
                events_(context_, e);
            }
            postControl(id);
        } else if (item[0] == MSG_FLOW_RING_CREATE_CMPLT && le16(item + 10) > kFlowRingId) {
            const unsigned k = le16(item + 10) - kFlowRingId - 1u;  // an AWDL flow ring
            if (k < awdlRings_ && awdl_[k].state == FLOW_PENDING) {
                AwdlFlow &f = awdl_[k];
                f.state = le16(item + 8) ? FLOW_REFUSED : FLOW_OPEN;
                if (f.state == FLOW_OPEN && f.pendingLength) post(FLOW + 1 + k, f.ifidx, f.pending.va, f.pendingLength);
                f.pendingLength = 0;
            }
        } else if (item[0] == MSG_FLOW_RING_CREATE_CMPLT) {
            flowStatus_ = le16(item + 8);
            flow_ = flowStatus_ ? FLOW_REFUSED : FLOW_OPEN;
        } else if (item[0] == MSG_FLOW_RING_DELETE_CMPLT && le16(item + 10) > kFlowRingId) {
            const unsigned k = le16(item + 10) - kFlowRingId - 1u;
            if (k < awdlRings_) awdl_[k].state = FLOW_CLOSED;
        }
        // MSG_IOCTLPTR_REQ_ACK and status messages (general and ring status) need no answer.
    }
    retire(CONTROL_COMPLETE);
    // brcmf_msgbuf_process_rx_complete: the frame at its own offset, or at the firmware's default one.
    while (next(RX_COMPLETE, item)) {
        const uint32_t id = le32(item + 4);
        if (item[0] != MSG_RX_CMPLT || id >= rxCount_) continue;
        const uint32_t offset = le16(item + 16) ? le16(item + 16) : dataOffset_;
        uint32_t length = le16(item + 14);
        if (offset >= kRxBufferSize)
            length = 0;
        else if (offset + length > kRxBufferSize)
            length = kRxBufferSize - offset;
        const uint8_t *frame = rx_[id].va + offset;
        dcacheFlush(frame, length);
        ++rxFrames_;
        if (frames_ && length >= kEthHeader) frames_(context_, frame, length, item[1]);  // copied out before the repost
        postRx(id);
    }
    retire(RX_COMPLETE);
    while (next(TX_COMPLETE, item)) {
        const uint32_t id = le32(item + 4);
        if (item[0] != MSG_TX_STATUS || id < 1 || id > sizeof(tx_) / sizeof(tx_[0])) continue;
        tx_[id - 1].busy = false;
        tx_[id - 1].status = le16(item + 8);
        ++txDone_;
    }
    retire(TX_COMPLETE);
}

uint32_t Msgbuf::service(bool *halted)
{
    *halted = false;
    chip_->write(pcie2_ + kMailboxMask, 0);
    const uint32_t status = chip_->read(pcie2_ + kMailboxInt);
    if (status == 0xFFFFFFFF) return status;  // the chip is gone; the caller reports it, the mask stays off
    if (status) chip_->write(pcie2_ + kMailboxInt, status);
    if (status & kMbFn0) {
        const Mmio &tcm = chip_->tcm();
        const uint32_t data = tcm.r32(dtohMailbox_);
        if (data) tcm.w32(dtohMailbox_, 0);
        // Deep-sleep requests (D2H_DEV_DS_ENTER_REQ) are not granted (nxk): the ring indices live in the chip's RAM,
        // which a sleeping chip may not serve. Without the acknowledgement the firmware simply stays awake.
        if (data & kD2HFwHalt) *halted = true;
    }
    drain();
    if (!*halted) chip_->write(pcie2_ + kMailboxMask, kMbD2HDoorbells | kMbFn0);
    return status;
}

bool Msgbuf::ioctlSubmit(uint32_t cmd, const uint8_t *data, size_t length, size_t outLength, char *why,
                         size_t whyLength)
{
    size_t n = length > outLength ? length : outLength;
    if (n > kIoctlMax) n = kIoctlMax;
    memset(ioctlBuffer_.va, 0, n);
    memcpy(ioctlBuffer_.va, data, length < n ? length : n);
    dcacheClean(ioctlBuffer_.va, n);
    uint8_t *p = reserve(CONTROL_SUBMIT);
    if (!p) {
        snprintf(why, whyLength, "control submit ring full");
        return false;
    }
    p[0] = MSG_IOCTLPTR_REQ;
    put32(p + 4, kIoctlReqPktId);
    put32(p + 8, cmd);
    put16(p + 12, ++transId_);
    put16(p + 14, static_cast<uint16_t>(n));
    put16(p + 16, static_cast<uint16_t>(n));
    put32(p + 24, ioctlBuffer_.pa);
    put32(p + 28, 0);
    replyReady_ = false;
    replyLength_ = 0;
    commit(CONTROL_SUBMIT);
    return true;
}

bool Msgbuf::flowCreateSubmit(const uint8_t destination[6], const uint8_t source[6])
{
    if (flow_ == FLOW_OPEN || flow_ == FLOW_PENDING) return true;
    uint8_t *p = reserve(CONTROL_SUBMIT);
    if (!p) return false;
    const Ring &f = rings_[FLOW];
    p[0] = MSG_FLOW_RING_CREATE;
    memcpy(p + 8, destination, 6);
    memcpy(p + 14, source, 6);
    put16(p + 22, kFlowRingId);
    put16(p + 28, f.items);
    put16(p + 30, f.size);
    put32(p + 32, f.pa);
    put32(p + 36, 0);
    flow_ = FLOW_PENDING;
    commit(CONTROL_SUBMIT);
    return true;
}

uint32_t Msgbuf::send(const uint8_t *frame, size_t length)
{
    if (flow_ != FLOW_OPEN) {
        ++txRefused_;
        return 0;
    }
    return post(FLOW, 0, frame, length);
}

// One Ethernet frame onto a TX flow ring of interface `ifidx`: its header in the ring item, the payload read from one
// of our buffers. The request id (1..32) its completion answers for, 0 when it cannot go now.
uint32_t Msgbuf::post(unsigned ring, uint8_t ifidx, const uint8_t *frame, size_t length)
{
    if (length < kEthHeader || length > kTxBufferSize) {
        ++txRefused_;
        return 0;
    }
    unsigned slot = 0;
    while (slot < sizeof(tx_) / sizeof(tx_[0]) && tx_[slot].busy) ++slot;
    if (slot == sizeof(tx_) / sizeof(tx_[0])) {
        ++txRefused_;
        return 0;
    }
    uint8_t *p = reserve(ring);
    if (!p) {
        ++txRefused_;
        return 0;
    }
    TxBuffer &t = tx_[slot];
    memcpy(t.b.va, frame, length);
    dcacheClean(t.b.va, length);
    t.busy = true;
    const uint32_t id = slot + 1;
    p[0] = MSG_TX_POST;
    p[1] = ifidx;
    put32(p + 4, id);
    memcpy(p + 8, frame, kEthHeader);
    p[22] = kPktFrame8023;
    p[23] = 1;  // one segment
    put32(p + 32, t.b.pa + kEthHeader);
    put32(p + 36, 0);
    put16(p + 42, static_cast<uint16_t>(length - kEthHeader));
    commit(ring);
    ++txFrames_;
    return id;
}

bool Msgbuf::sent(uint32_t id, uint16_t *status) const
{
    if (id < 1 || id > sizeof(tx_) / sizeof(tx_[0]) || tx_[id - 1].busy) return false;
    *status = tx_[id - 1].status;
    return true;
}

// brcmf_flowring_create for a non-station interface: one ring per destination, every group address on the
// broadcast one. The first frame to a new destination is kept until its ring is open (drain sends it).
int Msgbuf::awdlSend(uint8_t ifidx, const uint8_t *frame, size_t length)
{
    if (length < kEthHeader || length > kTxBufferSize) {
        ++awdlRefused_;
        return -1;
    }
    static const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t *da = (frame[0] & 1) ? kBroadcast : frame;
    unsigned k = 0, free = awdlRings_;
    for (; k < awdlRings_; ++k) {
        if ((awdl_[k].state == FLOW_OPEN || awdl_[k].state == FLOW_PENDING) && awdl_[k].ifidx == ifidx &&
            !memcmp(awdl_[k].da, da, 6))
            break;
        if (free == awdlRings_ && (awdl_[k].state == FLOW_CLOSED || awdl_[k].state == FLOW_REFUSED)) free = k;
    }
    if (k < awdlRings_) {
        AwdlFlow &f = awdl_[k];
        f.lastUse = ++awdlUse_;
        if (f.state == FLOW_OPEN) return post(FLOW + 1 + k, ifidx, frame, length) ? 1 : -1;
        if (f.pendingLength) {
            ++awdlRefused_;
            return -1;
        }
        memcpy(f.pending.va, frame, length);
        f.pendingLength = static_cast<uint16_t>(length);
        return 0;
    }
    if (free == awdlRings_) {  // every AWDL ring in use (or being deleted)
        ++awdlRefused_;
        return -1;
    }
    Ring &r = rings_[FLOW + 1 + free];
    uint8_t *p = reserve(CONTROL_SUBMIT);
    if (!p) {
        ++awdlRefused_;
        return -1;
    }
    AwdlFlow &f = awdl_[free];
    memcpy(f.da, da, 6);
    f.ifidx = ifidx;
    f.state = FLOW_PENDING;
    f.lastUse = ++awdlUse_;
    memcpy(f.pending.va, frame, length);
    f.pendingLength = static_cast<uint16_t>(length);
    r.pos = 0;  // a fresh ring: both indices from zero
    chip_->tcm().w16(r.wIndex, 0);
    bzero(r.va, static_cast<uint32_t>(r.items) * r.size);
    dcacheFlush(r.va, static_cast<uint32_t>(r.items) * r.size);
    p[0] = MSG_FLOW_RING_CREATE;
    p[1] = ifidx;
    memcpy(p + 8, da, 6);
    memcpy(p + 14, frame + 6, 6);
    put16(p + 22, static_cast<uint16_t>(kFlowRingId + 1 + free));
    put16(p + 28, r.items);
    put16(p + 30, r.size);
    put32(p + 32, r.pa);
    put32(p + 36, 0);
    commit(CONTROL_SUBMIT);
    return 0;
}

void Msgbuf::awdlClose()
{
    for (unsigned k = 0; k < awdlRings_; ++k) {
        AwdlFlow &f = awdl_[k];
        f.pendingLength = 0;
        if (f.state != FLOW_OPEN && f.state != FLOW_PENDING) continue;
        uint8_t *p = reserve(CONTROL_SUBMIT);
        if (!p) {
            f.state = FLOW_CLOSED;  // the control ring is full: forget it (the chip is in trouble anyway)
            continue;
        }
        p[0] = MSG_FLOW_RING_DELETE;
        p[1] = f.ifidx;
        put16(p + 8, static_cast<uint16_t>(kFlowRingId + 1 + k));
        commit(CONTROL_SUBMIT);
        f.state = FLOW_DELETING;
    }
}

}  // namespace swlan
