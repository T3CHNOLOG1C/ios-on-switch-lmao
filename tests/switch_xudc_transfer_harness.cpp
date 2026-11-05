// Host harness for drivers/SwitchUSB/XudcTransfer.h (tests/test_switch_xudc_transfer.py): plan() and outEvent() as C
// functions for ctypes, on plain arrays.
#include <stdint.h>

#include "XudcTransfer.h"

using namespace xudc;

extern "C" {

// runs: count (pa, length); out: per piece pa, offset, length, bounce. stats[5]: bounce bytes, discontinuity,
// boundary, high, edge. Returns the piece count (0: more than max).
uint32_t xudc_plan(const uint64_t *pas, const uint32_t *lengths, uint32_t count, uint32_t total, uint32_t mps, int out,
                   int firstPacket, uint64_t reach, uint64_t *piecePa, uint32_t *pieceOffset, uint32_t *pieceLength,
                   uint8_t *pieceBounce, uint32_t max, uint32_t *stats)
{
    Run runs[512];
    if (count > 512) return 0;
    for (uint32_t i = 0; i < count; ++i) runs[i] = {pas[i], lengths[i]};
    static Piece pieces[1024];
    if (max > 1024) max = 1024;
    PlanStats s = {};
    const uint32_t n = plan(runs, count, total, mps, out != 0, firstPacket != 0, reach, pieces, max, &s);
    for (uint32_t i = 0; i < n; ++i) {
        piecePa[i] = pieces[i].pa;
        pieceOffset[i] = pieces[i].offset;
        pieceLength[i] = pieces[i].length;
        pieceBounce[i] = pieces[i].bounce;
    }
    stats[0] = s.bounceBytes;
    stats[1] = s.discontinuity;
    stats[2] = s.boundary;
    stats[3] = s.high;
    stats[4] = s.edge;
    return n;
}

// out[4]: done, failed, actual, learned.
void xudc_out_event(uint8_t known, const uint32_t *lengths, uint32_t count, uint32_t index, uint32_t residue,
                    int shortPacket, uint32_t *out)
{
    const OutResult r = outEvent(known, lengths, count, index, residue, shortPacket != 0);
    out[0] = r.done;
    out[1] = r.failed;
    out[2] = r.actual;
    out[3] = r.learned;
}

uint32_t xudc_trb_max(void)
{
    return kTrbMaxBytes;
}
}
