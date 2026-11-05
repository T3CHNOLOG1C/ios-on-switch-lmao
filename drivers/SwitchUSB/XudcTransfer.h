// SPDX-License-Identifier: BSD-2-Clause-Patent
// How SwitchXUDC.cpp lays a request out on a transfer ring (header "Data endpoints"), and what an OUT transfer event
// says it received. No IOKit: test/xudc_transfer_test.cpp drives the same code on the host.
//
// A request is one TD: its physically contiguous runs (IOMemoryDescriptor::getPhysicalSegment after prepare()) become
// chained Normal TRBs pointing at the client's memory, IOC on the last only. Every TRB but the last holds whole packets
// (a multiple of the endpoint's max packet size), so no packet is ever split between two TRBs whatever the controller
// makes of TRB boundaries inside a packet (Linux tegra-xudc never gives it one; an mbuf chain from AppleUSBDeviceMux
// would). A packet that cannot go straight to the client's memory goes through a 4 KiB page of the driver's bounce pool
// instead, packets in a run sharing a page: one that crosses a physical discontinuity, a 64 KiB boundary (no TRB may),
// or 4 GiB (the XUDC is 32-bit here, as upstream Linux treats it on T210); and for OUT, one holding a run's start or
// end off a 64-byte cache line: the rest of that line is someone else's memory, which invalidating the line after the
// transfer would roll back (inside a run, a line two packets share is all this transfer's). A buffer of whole pages
// below 4 GiB, what large I/O is, goes entirely direct.
//
// OUT residue. Linux tegra-xudc takes a TD's actual length as its length minus the transfer event's residue, which
// holds only if the controller reports what is left of the whole TD (its context keeps EDTLA, the bytes of the TD so
// far); xHCI hosts report what is left of the event's TRB. Linux never builds a multi-TRB OUT TD on this controller in
// practice, so which it is has not been seen. The driver learns it: until known, a multi-TRB OUT TD's first TRB is one
// packet with IOC, whose event (full: residue 0 or the rest of the TD; short: below one packet or above the rest) can
// only mean one of the two, and then each OUT event is read the one way. A request too small to probe (under two
// packets) goes through one bounce page as one TRB; an event that fits neither marks the residue broken, the request
// fails rather than report a length that may be wrong, and the endpoint takes OUT requests one TRB at a time from then.
#pragma once
#include <stdint.h>

namespace xudc {

constexpr uint32_t kTrbMaxBytes = 65536;  // a TRB's length field and the 64 KiB boundary no TRB may cross
constexpr uint32_t kBouncePageBytes = 4096;
constexpr uint32_t kCacheLine = 64;

struct Run {  // a physically contiguous part of the request, in request order
    uint64_t pa;
    uint32_t length;
};

struct Piece {        // one TRB
    uint64_t pa;      // the client's memory; unused for a bounce piece (the caller gives it a page)
    uint32_t offset;  // in the request
    uint32_t length;
    bool bounce;
};

enum : uint8_t { kResidueUnknown, kResiduePerTrb, kResiduePerTd, kResidueBroken };

struct PlanStats {
    uint32_t bounceBytes, discontinuity, boundary, high, edge;  // bounced bytes; packets bounced by reason
};

// Lays out a request of `total` bytes in `runs` (covering it in order) for an endpoint of max packet `mps`: pieces in
// request order, at most `max`. firstPacket: the first piece is one packet alone (the OUT residue probe). reach: the
// first physical address the controller cannot use. Returns the piece count, 0 if more than `max` would be needed.
inline uint32_t plan(const Run *runs, uint32_t runCount, uint32_t total, uint32_t mps, bool out, bool firstPacket,
                     uint64_t reach, Piece *pieces, uint32_t max, PlanStats *stats)
{
    if (!mps) return 0;
    const uint32_t directMax = kTrbMaxBytes / mps * mps;
    const uint32_t bounceMax = kBouncePageBytes >= mps ? kBouncePageBytes / mps * mps : 0;
    if (!bounceMax) return 0;
    uint32_t count = 0, run = 0, runStart = 0;  // the run holding `offset`, and its first offset
    Piece *open = nullptr;                      // the piece the next packet may join
    if (!total) {                               // a zero-length transfer: one empty TRB
        if (!max) return 0;
        pieces[0] = {0, 0, 0, false};
        return 1;
    }
    for (uint32_t offset = 0; offset < total;) {
        const uint32_t packet = total - offset < mps ? total - offset : mps;
        while (run < runCount && offset >= runStart + runs[run].length) runStart += runs[run++].length;
        if (run == runCount) return 0;  // the runs end before the request does
        const uint64_t pa = runs[run].pa + (offset - runStart);
        const bool inRun = offset + packet <= runStart + runs[run].length;
        const bool crosses = (pa >> 16) != ((pa + packet - 1) >> 16);
        const bool high = pa + packet > reach;
        const bool edge = out && inRun &&
                          ((offset == runStart && (pa & (kCacheLine - 1))) ||
                           (offset + packet == runStart + runs[run].length && ((pa + packet) & (kCacheLine - 1))));
        const bool bounce = !inRun || crosses || high || edge;
        if (bounce) {
            stats->bounceBytes += packet;
            if (!inRun)
                ++stats->discontinuity;
            else if (high)
                ++stats->high;
            else if (crosses)
                ++stats->boundary;
            else
                ++stats->edge;
        }
        const bool probeCut = firstPacket && offset == mps;  // the probe packet stands alone
        const bool joins = open && !probeCut && open->bounce == bounce &&
                           (bounce ? open->length + packet <= bounceMax
                                   : open->pa + open->length == pa && open->length + packet <= directMax &&
                                         (pa & (kTrbMaxBytes - 1)) != 0);
        if (joins) {
            open->length += packet;
        } else {
            if (count == max) return 0;
            open = &pieces[count++];
            *open = {bounce ? 0 : pa, offset, packet, bounce};
        }
        offset += packet;
    }
    return count;
}

struct OutResult {
    bool done;        // the TD is complete (else: a mid-TD event, the probe's)
    bool failed;      // the residue fits neither reading: fail the request
    uint32_t actual;  // bytes received, when done
    uint8_t learned;  // kResidue*: what this event showed (kResidueUnknown: nothing)
};

// A transfer event in an OUT TD of `count` TRBs (`lengths`), at TRB `index`, with `residue`, short packet or not, read
// with the residue as known so far.
inline OutResult outEvent(uint8_t known, const uint32_t *lengths, uint32_t count, uint32_t index, uint32_t residue,
                          bool shortPacket)
{
    uint32_t before = 0, after = 0, total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        total += lengths[i];
        if (i < index) before += lengths[i];
        if (i > index) after += lengths[i];
    }
    const uint32_t length = index < count ? lengths[index] : 0;
    OutResult r = {true, false, 0, kResidueUnknown};
    if (index >= count) {
        r.failed = true;
        return r;
    }
    if (index == count - 1) {  // the last TRB: both readings agree
        r.failed = residue > length;
        r.actual = r.failed ? 0 : total - residue;
        return r;
    }
    if (!shortPacket) {  // a full TRB in the middle: only the probe asks for one
        r.done = false;
        r.learned = residue == 0 ? kResiduePerTrb : residue == after ? kResiduePerTd : kResidueBroken;
        return r;
    }
    const bool perTrb = residue <= length, perTd = residue > after && residue <= length + after;
    uint8_t reading = known;
    if (known == kResidueUnknown) reading = perTrb && !perTd ? kResiduePerTrb : perTd && !perTrb ? kResiduePerTd : 0;
    if ((reading == kResiduePerTrb && perTrb) || (reading == kResiduePerTd && perTd)) {
        r.actual = reading == kResiduePerTrb ? before + length - residue : total - residue;
        if (known == kResidueUnknown) r.learned = reading;
        return r;
    }
    r.failed = true;
    r.learned = kResidueBroken;
    return r;
}

}  // namespace xudc
