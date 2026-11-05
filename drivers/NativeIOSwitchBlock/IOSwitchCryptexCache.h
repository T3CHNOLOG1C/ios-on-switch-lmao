// SPDX-License-Identifier: BSD-2-Clause-Patent
// The cryptex direct read map's block cache, kept free of kernel types so the host test (test/cryptex_cache_test.cpp)
// runs this exact code against a model. The caller holds a lock around every call, owns the windows' memory, issues
// the reads this returns and copies the blocks out.
//
// APFS reads the cryptex volumes' files one 4 KiB block at a time, each after the last. A card command costs ~0.30 ms
// for 4 KiB and ~1.6 ms for 128 KiB, and of every 128 KiB window of the System image a boot reads at all it reads ~81%
// (89% of 64 KiB windows, 73% of 256 KiB): 64-256 KiB windows cost about the same card time, 128 KiB the least, a
// fifth of reading the blocks one by one. So the image is cached in aligned windows of kIOSwitchWindowBlocks blocks:
//  - a small request that misses reads its whole window (every block of it not held or being read), and waits; one of
//    a lower I/O class reads only its own blocks, so that no foreground read waits on a background one;
//  - a request for blocks being read waits for that read: a block is never read twice at once, and never read while
//    it is held (a held block's bytes never change while it is held, so copies out of it need no lock);
//  - readAhead reads exactly the blocks asked for (the boot prefetch's runs), for the page-ins that follow;
//  - a large request is served here only if every block is held or being read, else straight into its own pages;
//  - an idle window (no request holding it, no read filling it) is reused least recently used first, and a window whose
//    held blocks have all been served (they are in the files' page cache now) goes to the cold end at once.
#ifndef IOSWITCH_CRYPTEX_CACHE_H
#define IOSWITCH_CRYPTEX_CACHE_H
#include <stddef.h>
#include <stdint.h>

constexpr uint32_t kIOSwitchCacheBlock = 4096;
constexpr uint32_t kIOSwitchWindowBlocks = 32;  // 128 KiB: one bit per block in a uint32_t
constexpr uint32_t kIOSwitchWindowBytes = kIOSwitchWindowBlocks * kIOSwitchCacheBlock;
constexpr uint32_t kIOSwitchSpanWindows = 9;                          // windows a request of up to 1 MiB can touch
constexpr uint32_t kIOSwitchWindowReads = kIOSwitchWindowBlocks / 2;  // most separate reads one window can need
constexpr uint32_t kIOSwitchAroundWindows = 2;  // windows a read-around request may touch (at most 16 blocks)
constexpr uint64_t kIOSwitchNoTag = ~0ull;

struct IOSwitchCacheRequest;

// A request waiting for blocks of one window that reads are filling.
struct IOSwitchWait {
    IOSwitchWait *next;
    IOSwitchCacheRequest *request;
    uint32_t need;  // the blocks it still waits for: always a subset of the window's `reading`
};

struct IOSwitchWindow {
    uint64_t tag;    // the window's first image block (a multiple of kIOSwitchWindowBlocks), kIOSwitchNoTag while empty
    uint32_t valid;  // blocks holding the image's bytes
    uint32_t reading;  // blocks a read is filling (never valid at the same time)
    uint32_t served;   // held blocks copied to a request since the window was filled
    uint32_t ahead;    // blocks read ahead for the prefetch, not for a request
    uint32_t users;    // requests holding the window: waiting for it or copying out of it
    uint8_t image;
    bool idle;                      // on the idle list: no users, nothing reading, reusable
    bool retired;                   // given back to the caller (its memory freed): never used again
    IOSwitchWindow *older, *newer;  // the idle list
    IOSwitchWindow *chain;          // the hash bucket's
    IOSwitchWait *waits;
    void *bytes, *memory;  // the caller's: the window's kIOSwitchWindowBytes and whatever owns them
};

// One request's hold on the cache, from plan() until release().
struct IOSwitchCacheRequest {
    void *owner;                      // the caller's
    IOSwitchCacheRequest *nextReady;  // finished()'s list
    IOSwitchWindow *window[kIOSwitchSpanWindows];
    uint32_t need[kIOSwitchSpanWindows];  // the request's blocks in each window
    IOSwitchWait wait[kIOSwitchSpanWindows];
    uint32_t windows;  // windows held
    uint32_t waiting;  // windows it still waits on: ready to copy at 0
    bool failed;       // a read of a block it needed failed: complete it with an error
};

// One read for the caller to issue: blocks [first, first + count) of `window`, into window->bytes at first * 4 KiB.
struct IOSwitchCacheRead {
    IOSwitchWindow *window;
    uint32_t first, count;
};

struct IOSwitchCacheStats {
    uint64_t requests;     // taken by plan()
    uint64_t ready;        // of them, every block already held (no wait)
    uint64_t refused;      // left to the direct path: no window free, a large one not all here, or too wide
    uint64_t reads;        // reads issued (for requests and ahead)
    uint64_t blocksRead;   // their blocks
    uint64_t aheadBlocks;  // of them, read ahead (readAhead)
    uint64_t served;       // held blocks copied out for the first time since read
    uint64_t aheadServed;  // of them, read ahead
    uint64_t evicted;      // windows reused while holding blocks never served
    uint64_t failed;       // reads that failed
};

// The blocks [from, to) of a window as a mask (0 <= from <= to <= kIOSwitchWindowBlocks).
inline uint32_t ioswitchWindowMask(uint32_t from, uint32_t to)
{
    const uint32_t high = to >= kIOSwitchWindowBlocks ? ~0u : (1u << to) - 1;
    const uint32_t low = from >= kIOSwitchWindowBlocks ? ~0u : (1u << from) - 1;
    return high & ~low;
}

inline uint32_t ioswitchBlockCount(uint32_t mask)
{
    return static_cast<uint32_t>(__builtin_popcount(mask));
}

class IOSwitchCache {
public:
    enum Mode {
        kReadAround,   // a small request: read the rest of its windows too (at most kIOSwitchAroundWindows)
        kNeedOnly,     // a small one of a lower class: read only its own blocks (no foreground read ever waits on a
                       // window a background one is reading)
        kCoveredOnly,  // a large one: only if every block is held or being read, and then read nothing
    };

    // `windows` (with their bytes/memory set) and `buckets` (a power of two, at least 1) belong to the caller.
    void init(IOSwitchWindow *windows, uint32_t count, IOSwitchWindow **buckets, uint32_t bucketCount)
    {
        all = windows;
        allCount = count;
        table = buckets;
        mask = bucketCount - 1;
        for (uint32_t i = 0; i < bucketCount; ++i) table[i] = nullptr;
        oldest = newest = nullptr;
        liveCount = 0;
        st = {};
        for (uint32_t i = 0; i < count; ++i) {
            IOSwitchWindow &w = windows[i];
            w.tag = kIOSwitchNoTag;
            w.valid = w.reading = w.served = w.ahead = w.users = 0;
            w.image = 0;
            w.idle = w.retired = false;
            w.older = w.newer = w.chain = nullptr;
            w.waits = nullptr;
            linkIdle(&w, true);
            ++liveCount;
        }
    }

    // A request for blocks [first, first + blocks) of image `image` (imageBlocks long). True when the cache takes it:
    // r holds its windows, r->waiting says how many it waits on (0: copy now), and reads[0, *count) are to be issued (at
    // most kIOSwitchAroundWindows * kIOSwitchWindowReads, which `capacity` must allow but in kCoveredOnly). False: go
    // direct.
    bool plan(IOSwitchCacheRequest *r, uint8_t image, uint64_t first, uint32_t blocks, uint64_t imageBlocks, Mode mode,
              IOSwitchCacheRead *reads, uint32_t capacity, uint32_t *count)
    {
        *count = 0;
        r->windows = r->waiting = 0;
        r->failed = false;
        r->nextReady = nullptr;
        if (!blocks || first >= imageBlocks || blocks > imageBlocks - first) return refuse(r, 0);
        const uint64_t firstTag = first - first % kIOSwitchWindowBlocks;
        const uint64_t last = first + blocks - 1, lastTag = last - last % kIOSwitchWindowBlocks;
        const uint32_t n = static_cast<uint32_t>((lastTag - firstTag) / kIOSwitchWindowBlocks + 1);
        if (n > kIOSwitchSpanWindows ||
            (mode != kCoveredOnly && (n > kIOSwitchAroundWindows || capacity < n * kIOSwitchWindowReads)))
            return refuse(r, 0);
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t tag = firstTag + uint64_t(i) * kIOSwitchWindowBlocks;
            const uint64_t from = first > tag ? first : tag, to = first + blocks < tag + kIOSwitchWindowBlocks
                                                                      ? first + blocks
                                                                      : tag + kIOSwitchWindowBlocks;
            const uint32_t need = ioswitchWindowMask(uint32_t(from - tag), uint32_t(to - tag));
            IOSwitchWindow *w = lookup(image, tag);
            if (mode == kCoveredOnly && (!w || (need & ~(w->valid | w->reading)))) return refuse(r, i);
            if (!w && !(w = take(image, tag))) return refuse(r, i);
            pin(w);
            r->window[i] = w;
            r->need[i] = need;
            r->windows = i + 1;
        }
        for (uint32_t i = 0; i < n; ++i) {
            IOSwitchWindow *w = r->window[i];
            if (mode != kCoveredOnly) {
                const uint32_t around = mode == kReadAround ? within(w->tag, imageBlocks) : r->need[i];
                *count += emit(w, around & ~(w->valid | w->reading), false, reads + *count);
            }
            const uint32_t pending = r->need[i] & ~w->valid;
            if (pending) {
                IOSwitchWait *wait = &r->wait[i];
                wait->request = r;
                wait->need = pending;
                wait->next = w->waits;
                w->waits = wait;
                ++r->waiting;
            }
        }
        ++st.requests;
        if (!r->waiting) ++st.ready;
        return true;
    }

    // Reads blocks [first, first + blocks) of image `image` ahead into windows, for the requests to come: those of them
    // not held or being read, in the window holding `first` only (at most kIOSwitchWindowReads reads). Returns the
    // blocks it covered (to the window's end, or the range's), so a caller walks a range window by window; 0 when no
    // window is free.
    uint32_t readAhead(uint8_t image, uint64_t first, uint64_t blocks, uint64_t imageBlocks, IOSwitchCacheRead *reads,
                       uint32_t *count)
    {
        *count = 0;
        if (!blocks || first >= imageBlocks) return 0;
        if (blocks > imageBlocks - first) blocks = imageBlocks - first;
        const uint64_t tag = first - first % kIOSwitchWindowBlocks;
        const uint64_t end =
            first + blocks < tag + kIOSwitchWindowBlocks ? first + blocks : tag + kIOSwitchWindowBlocks;
        IOSwitchWindow *w = lookup(image, tag);
        if (!w && !(w = take(image, tag))) return 0;
        const uint32_t want = ioswitchWindowMask(uint32_t(first - tag), uint32_t(end - tag)) & ~(w->valid | w->reading);
        if (w->idle && want) unlinkIdle(w);  // reading from here on: not reusable until the reads finish
        *count = emit(w, want, true, reads);
        makeIdleIfUnused(w);  // (a window taken for nothing goes straight back)
        return uint32_t(end - first);
    }

    // A read from emit() has finished (ok: the blocks hold the image's bytes). Returns the requests it made ready (linked
    // by nextReady); each is then copied out (unless failed) and released.
    IOSwitchCacheRequest *finished(IOSwitchWindow *w, uint32_t first, uint32_t count, bool ok)
    {
        const uint32_t m = ioswitchWindowMask(first, first + count);
        w->reading &= ~m;
        if (ok) {
            w->valid |= m;
        } else {
            w->ahead &= ~m;  // read again later, perhaps for a request
            ++st.failed;
        }
        IOSwitchCacheRequest *ready = nullptr;
        for (IOSwitchWait **link = &w->waits; *link;) {
            IOSwitchWait *wait = *link;
            bool done;
            if (!ok && (wait->need & m)) {
                wait->request->failed = true;
                done = true;
            } else {
                wait->need &= ~w->valid;
                done = !wait->need;
            }
            if (!done) {
                link = &wait->next;
                continue;
            }
            *link = wait->next;
            if (!--wait->request->waiting) {
                wait->request->nextReady = ready;
                ready = wait->request;
            }
        }
        makeIdleIfUnused(w);
        return ready;
    }

    // The request is done with its windows (copied: its blocks were copied out).
    void release(IOSwitchCacheRequest *r, bool copied)
    {
        for (uint32_t i = 0; i < r->windows; ++i) {
            IOSwitchWindow *w = r->window[i];
            if (copied) {
                const uint32_t fresh = r->need[i] & w->valid & ~w->served;
                st.served += ioswitchBlockCount(fresh);
                st.aheadServed += ioswitchBlockCount(fresh & w->ahead);
                w->served |= fresh;
            }
            unpin(w);
        }
        r->windows = 0;
    }

    // One idle window beyond `keep` live ones, taken out for the caller to free, or nullptr.
    IOSwitchWindow *retire(uint32_t keep)
    {
        if (liveCount <= keep || !oldest) return nullptr;
        IOSwitchWindow *w = oldest;
        unlinkIdle(w);
        if (w->tag != kIOSwitchNoTag) unhash(w);
        w->tag = kIOSwitchNoTag;
        w->retired = true;
        --liveCount;
        return w;
    }

    uint32_t live() const { return liveCount; }
    const IOSwitchCacheStats &stats() const { return st; }

    // The invariants, or nullptr (for the tests).
    const char *check() const
    {
        uint32_t idle = 0, live = 0;
        for (const IOSwitchWindow *w = oldest, *prev = nullptr; w; prev = w, w = w->newer) {
            if (!w->idle || w->older != prev) return "idle list broken";
            if (++idle > allCount) return "idle list loops";
        }
        for (uint32_t i = 0; i < allCount; ++i) {
            const IOSwitchWindow &w = all[i];
            if (w.retired) {
                if (w.idle || w.users || w.reading || w.waits) return "retired window in use";
                continue;
            }
            ++live;
            if (w.valid & w.reading) return "a block both held and being read";
            if (w.idle && (w.users || w.reading || w.waits)) return "idle window in use";
            if (!w.idle && !w.users && !w.reading) return "unused window not idle";
            if (w.tag == kIOSwitchNoTag && (w.valid || w.reading || w.users)) return "empty window in use";
            if (w.tag != kIOSwitchNoTag && (w.tag % kIOSwitchWindowBlocks || lookup(w.image, w.tag) != &w))
                return "window not found by its tag";
            if (w.served & ~w.valid) return "served block not held";
            uint32_t waits = 0;
            for (const IOSwitchWait *wait = w.waits; wait; wait = wait->next) {
                if (!wait->need || (wait->need & ~w.reading)) return "a wait for blocks nothing is reading";
                if (++waits > w.users) return "more waits than users";
            }
        }
        if (live != liveCount) return "live count";
        for (uint32_t b = 0; b <= mask; ++b)
            for (const IOSwitchWindow *w = table[b]; w; w = w->chain)
                if (w->retired || w->tag == kIOSwitchNoTag || bucket(w->image, w->tag) != b) return "hash chain";
        uint32_t idleFlags = 0;
        for (uint32_t i = 0; i < allCount; ++i) idleFlags += all[i].idle;
        return idleFlags == idle ? nullptr : "idle flags";
    }

private:
    IOSwitchWindow *all = nullptr, **table = nullptr, *oldest = nullptr, *newest = nullptr;
    uint32_t allCount = 0, mask = 0, liveCount = 0;
    IOSwitchCacheStats st = {};

    uint32_t bucket(uint8_t image, uint64_t tag) const
    {
        uint64_t h = (tag / kIOSwitchWindowBlocks) * 0x9e3779b97f4a7c15ull ^ (uint64_t(image) << 61);
        return static_cast<uint32_t>(h >> 32) & mask;
    }
    IOSwitchWindow *lookup(uint8_t image, uint64_t tag) const
    {
        for (IOSwitchWindow *w = table[bucket(image, tag)]; w; w = w->chain)
            if (w->tag == tag && w->image == image) return w;
        return nullptr;
    }
    void unhash(IOSwitchWindow *w)
    {
        for (IOSwitchWindow **link = &table[bucket(w->image, w->tag)]; *link; link = &(*link)->chain)
            if (*link == w) {
                *link = w->chain;
                break;
            }
        w->chain = nullptr;
    }
    // The window's blocks inside an image of imageBlocks blocks.
    static uint32_t within(uint64_t tag, uint64_t imageBlocks)
    {
        return imageBlocks - tag >= kIOSwitchWindowBlocks ? ~0u : ioswitchWindowMask(0, uint32_t(imageBlocks - tag));
    }
    void linkIdle(IOSwitchWindow *w, bool cold)
    {
        w->idle = true;
        if (cold) {
            w->older = nullptr;
            w->newer = oldest;
            if (oldest) oldest->older = w;
            oldest = w;
            if (!newest) newest = w;
        } else {
            w->newer = nullptr;
            w->older = newest;
            if (newest) newest->newer = w;
            newest = w;
            if (!oldest) oldest = w;
        }
    }
    void unlinkIdle(IOSwitchWindow *w)
    {
        (w->older ? w->older->newer : oldest) = w->newer;
        (w->newer ? w->newer->older : newest) = w->older;
        w->older = w->newer = nullptr;
        w->idle = false;
    }
    // Idle once nothing holds or fills it: at the cold end when nothing it holds is still unserved.
    void makeIdleIfUnused(IOSwitchWindow *w)
    {
        if (w->users || w->reading || w->idle || w->retired) return;
        linkIdle(w, !(w->valid & ~w->served));
    }
    void pin(IOSwitchWindow *w)
    {
        if (w->idle) unlinkIdle(w);
        ++w->users;
    }
    void unpin(IOSwitchWindow *w)
    {
        --w->users;
        makeIdleIfUnused(w);
    }
    // The least recently used idle window, emptied and named (image, tag); nullptr when every window is in use.
    IOSwitchWindow *take(uint8_t image, uint64_t tag)
    {
        IOSwitchWindow *w = oldest;
        if (!w) return nullptr;
        unlinkIdle(w);
        if (w->tag != kIOSwitchNoTag) {
            if (w->valid & ~w->served) ++st.evicted;
            unhash(w);
        }
        w->tag = tag;
        w->image = image;
        w->valid = w->reading = w->served = w->ahead = 0;
        w->waits = nullptr;
        const uint32_t b = bucket(image, tag);
        w->chain = table[b];
        table[b] = w;
        return w;
    }
    // The contiguous runs of `want` as reads (marked reading); returns how many.
    uint32_t emit(IOSwitchWindow *w, uint32_t want, bool ahead, IOSwitchCacheRead *reads)
    {
        uint32_t n = 0;
        w->reading |= want;
        if (ahead) w->ahead |= want;
        for (uint32_t b = 0; b < kIOSwitchWindowBlocks;) {
            if (!(want & (1u << b))) {
                ++b;
                continue;
            }
            uint32_t e = b;
            while (e < kIOSwitchWindowBlocks && (want & (1u << e))) ++e;
            reads[n++] = {w, b, e - b};
            st.blocksRead += e - b;
            if (ahead) st.aheadBlocks += e - b;
            b = e;
        }
        st.reads += n;
        return n;
    }
    bool refuse(IOSwitchCacheRequest *r, uint32_t held)
    {
        for (uint32_t i = 0; i < held; ++i) unpin(r->window[i]);
        r->windows = 0;
        ++st.refused;
        return false;
    }
};
#endif
