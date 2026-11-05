// SPDX-License-Identifier: BSD-2-Clause-Patent
// r12-sdio: which queued reads ride in one card command with the read the SD worker picked (NativeIOSwitchBlock.cpp
// pickLocked), wherever they sit in the queue.
//
// Until r12-sdio a chain only took the requests right behind its first one in the queue, so two reads that continue
// each other on the card but were queued with another request between them took two commands (~0.58 ms each on r7p5,
// most of it the card's access time; 4 KiB of data is ~40 us of it). Now a read chain grows at either end with any
// queued read (among the first `scan`) that continues it, as long as:
// - it is a read (writes keep FIFO order and their old rule: only the requests right behind, in queue order);
// - it overlaps no older request still queued: a read never moves ahead of a write it would see, nor of an older read of
//   the same blocks (the same rule the r6-lows priority pass applies to its pick);
// - the chain stays within one command (`limit` sectors, `maxCount` requests).
// The chain comes back in card order (rising LBA), so the descriptor table and the bounce path walk it as before.
#pragma once

#include <stdint.h>

template <typename Request> inline bool ioswitchOverlaps(const Request *a, const Request *b)
{
    return a->lba < b->lba + b->sectors && b->lba < a->lba + a->sectors;
}

// Marks `first` and the reads that join it (`chained`), returns how many, and the chain's first LBA and length. `first`
// is in the queue; nothing is unlinked yet (ioswitchUnlinkChain does that).
template <typename Request>
inline uint32_t ioswitchGatherReads(Request *head, Request *first, uint32_t limit, uint32_t maxCount, unsigned scan,
                                    uint64_t *start, uint32_t *sectors)
{
    first->chained = true;
    uint64_t low = first->lba, high = first->lba + first->sectors;
    uint32_t count = 1;
    // Passes over the queue until one adds nothing: requests queued in rising order join in one pass.
    for (bool grew = true; grew && count < maxCount && high - low < limit;) {
        grew = false;
        unsigned seen = 0;
        for (Request *r = head; r && seen < scan && count < maxCount; r = r->next, ++seen) {
            if (r->write || r->chained) continue;
            const bool after = r->lba == high, before = r->lba + r->sectors == low;
            if ((!after && !before) || high - low + r->sectors > limit) continue;
            bool overlaps = false;
            for (const Request *older = head; older != r && !overlaps; older = older->next)
                overlaps = !older->chained && ioswitchOverlaps(older, r);
            if (overlaps) continue;
            r->chained = true;
            ++count;
            if (after)
                high += r->sectors;
            else
                low -= r->sectors;
            grew = true;
        }
    }
    *start = low;
    *sectors = static_cast<uint32_t>(high - low);
    return count;
}

// Whether the `count` requests marked `chained` are reads that tile [start, start + sectors) exactly: inside it, pairwise
// disjoint, their lengths adding up to it. The driver checks this before it unlinks a chain (ioswitchGatherReads is
// host-tested, but a chain that does not tile would put the card's bytes in the wrong requests).
template <typename Request>
inline bool ioswitchChainTiles(const Request *head, uint64_t start, uint32_t sectors, uint32_t count)
{
    uint64_t total = 0;
    uint32_t found = 0;
    for (const Request *r = head; r; r = r->next) {
        if (!r->chained) continue;
        ++found;
        if (r->write || !r->sectors || r->lba < start || r->lba + r->sectors > start + sectors) return false;
        total += r->sectors;
        for (const Request *other = r->next; other; other = other->next)
            if (other->chained && ioswitchOverlaps(r, other)) return false;
    }
    return found == count && total == sectors;
}

// Clears every `chained` mark (a chain that failed ioswitchChainTiles stays queued as it was).
template <typename Request> inline void ioswitchClearChain(Request *head)
{
    for (Request *r = head; r; r = r->next) r->chained = false;
}

// Unlinks the `count` requests marked `chained` from the queue (head and tail kept right, the others in their order),
// clears the mark, and returns them linked in rising LBA order.
template <typename Request> inline Request *ioswitchUnlinkChain(Request *&head, Request *&tail, uint32_t count)
{
    Request *chain = nullptr, *last = nullptr, *prev = nullptr;
    for (Request *r = head; r && count;) {
        Request *const next = r->next;
        if (!r->chained) {
            prev = r;
            r = next;
            continue;
        }
        if (prev)
            prev->next = next;
        else
            head = next;
        if (tail == r) tail = prev;
        r->chained = false;
        --count;
        if (!chain || last->lba < r->lba) {
            // The usual case: queued in rising order, appended.
            r->next = nullptr;
            if (last)
                last->next = r;
            else
                chain = r;
            last = r;
        } else {
            Request **at = &chain;
            while ((*at)->lba < r->lba) at = &(*at)->next;
            r->next = *at;
            *at = r;
        }
        r = next;
    }
    return chain;
}
