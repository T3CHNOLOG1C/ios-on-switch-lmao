// Host test of drivers/NativeIOSwitchBlock/IOSwitchClientCopy.h: the SD driver's copy between a request's memory and
// the bounce buffer, segment by segment through copypv, without cppvFsnk (the cache flush readBytes/writeBytes add).
// The cppv* values come from XNU's vm/pmap.h (-D on the command line, test_native_sd_client_copy.py).
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

typedef uint64_t IOByteCount;
typedef uint64_t addr64_t;
constexpr uint32_t kIOMemoryMapperNone = 0x800;

#include "IOSwitchClientCopy.h"

// "Physical memory": PA kBase + i is phys[i].
constexpr addr64_t kBase = 0x100000000ull;
static std::vector<uint8_t> phys(1 << 20);

// A descriptor of scattered physical runs (as a UPL of non-contiguous pages, or a sub-page buffer).
struct Run {
    addr64_t pa;
    IOByteCount bytes;
};
struct Memory {
    std::vector<Run> runs;
    int missingAt = -1;  // run index getPhysicalSegment pretends is not there
    unsigned lookups = 0;
    addr64_t getPhysicalSegment(IOByteCount offset, IOByteCount *length, uint32_t options)
    {
        assert(options == kIOMemoryMapperNone);
        ++lookups;
        for (size_t i = 0; i < runs.size(); ++i) {
            if (offset < runs[i].bytes) {
                if ((int)i == missingAt) break;
                *length = runs[i].bytes - offset;
                return runs[i].pa + offset;
            }
            offset -= runs[i].bytes;
        }
        *length = 0;
        return 0;
    }
};

struct Call {
    addr64_t source, sink;
    unsigned size;
    int which;
};
static std::vector<Call> calls;
static int failAt = -1;

static int fakeCopypv(addr64_t source, addr64_t sink, unsigned size, int which)
{
    calls.push_back({source, sink, size, which});
    if ((int)calls.size() - 1 == failAt) return 5;  // KERN_FAILURE-ish
    // Exactly one side physical, the other a kernel virtual address (copypv's own rule).
    assert(((which & cppvPsrc) != 0) != ((which & cppvPsnk) != 0));
    assert(which & cppvKmap);
    uint8_t *src = (which & cppvPsrc) ? &phys.at(source - kBase) : reinterpret_cast<uint8_t *>(source);
    uint8_t *dst = (which & cppvPsnk) ? &phys.at(sink - kBase) : reinterpret_cast<uint8_t *>(sink);
    if (which & cppvPsrc) assert(source - kBase + size <= phys.size());
    if (which & cppvPsnk) assert(sink - kBase + size <= phys.size());
    memcpy(dst, src, size);
    return 0;
}

static Memory scattered()
{
    // 16 KiB pages out of order, a run that is longer than the request needs at the end, and a 512-byte piece.
    Memory m;
    m.runs = {{kBase + 0x40000, 0x4000}, {kBase + 0x10000, 0x200}, {kBase + 0x80000, 0x4000},
              {kBase + 0x20000, 0x3e00}, {kBase + 0xc0000, 0x10000}};
    return m;
}

static void reset()
{
    calls.clear();
    failAt = -1;
    std::fill(phys.begin(), phys.end(), 0xee);
}

int main()
{
    const uint32_t length = 0x4000 + 0x200 + 0x4000 + 0x3e00 + 0x1000;  // ends inside the last run
    std::vector<uint8_t> bounce(length + 64, 0);
    for (uint32_t i = 0; i < length; ++i) bounce[i] = static_cast<uint8_t>(i * 7 + (i >> 9));

    // A card read: bounce -> client, every byte where its offset says, nothing past `length`, no flush flag.
    reset();
    Memory m = scattered();
    assert(ioswitchClientCopy(&m, bounce.data(), length, true, fakeCopypv));
    assert(calls.size() == 5);
    uint64_t total = 0, offset = 0;
    for (const Call &c : calls) {
        assert(c.which == (cppvPsnk | cppvNoRefSrc | cppvNoModSnk | cppvKmap));
        assert(!(c.which & cppvFsnk) && !(c.which & cppvFsrc));
        assert(c.source == reinterpret_cast<uintptr_t>(bounce.data() + offset));
        total += c.size;
        offset += c.size;
    }
    assert(total == length);
    offset = 0;
    for (const Run &r : m.runs) {
        const uint64_t take = std::min<uint64_t>(r.bytes, length - offset);
        assert(!memcmp(&phys[r.pa - kBase], bounce.data() + offset, take));
        if (take < r.bytes) assert(phys[r.pa - kBase + take] == 0xee);  // not one byte past the request
        offset += take;
        if (offset == length) break;
    }

    // A card write: client -> bounce, the same walk the other way.
    std::vector<uint8_t> staged(length, 0);
    calls.clear();
    assert(ioswitchClientCopy(&m, staged.data(), length, false, fakeCopypv));
    for (const Call &c : calls) {
        assert(c.which == (cppvPsrc | cppvNoRefSrc | cppvKmap));
        assert(!(c.which & cppvFsnk) && !(c.which & cppvFsrc));
    }
    assert(!memcmp(staged.data(), bounce.data(), length));

    // A single 512-byte request in one segment.
    reset();
    Memory one;
    one.runs = {{kBase + 0x1000, 0x4000}};
    assert(ioswitchClientCopy(&one, bounce.data(), 512, true, fakeCopypv));
    assert(calls.size() == 1 && calls[0].size == 512 && calls[0].sink == kBase + 0x1000);

    // Nothing to copy: no lookups, no copies.
    reset();
    Memory none = scattered();
    assert(ioswitchClientCopy(&none, bounce.data(), 0, true, fakeCopypv));
    assert(calls.empty() && none.lookups == 0);

    // A segment the descriptor cannot produce: the request fails, no copy runs past it.
    reset();
    Memory hole = scattered();
    hole.missingAt = 2;
    assert(!ioswitchClientCopy(&hole, bounce.data(), length, true, fakeCopypv));
    assert(calls.size() == 2);

    // A descriptor shorter than the request: fails.
    reset();
    Memory shortMemory;
    shortMemory.runs = {{kBase, 0x1000}};
    assert(!ioswitchClientCopy(&shortMemory, bounce.data(), 0x2000, true, fakeCopypv));
    assert(calls.size() == 1);

    // copypv failing on the third segment: the request fails there.
    reset();
    Memory failing = scattered();
    failAt = 2;
    assert(!ioswitchClientCopy(&failing, bounce.data(), length, true, fakeCopypv));
    assert(calls.size() == 3);

    // From an offset inside the memory (the bounce path's pieces): the walk starts in the right run at the right place.
    reset();
    Memory middle = scattered();
    const uint32_t from = 0x4000 + 0x100, piece = 0x200 - 0x100 + 0x4000 + 0x80;
    assert(ioswitchClientCopy(&middle, bounce.data(), piece, true, fakeCopypv, from));
    assert(calls.size() == 3);
    assert(calls[0].sink == kBase + 0x10000 + 0x100 && calls[0].size == 0x100);
    assert(calls[1].sink == kBase + 0x80000 && calls[1].size == 0x4000);
    assert(calls[2].sink == kBase + 0x20000 && calls[2].size == 0x80);
    assert(!memcmp(&phys[0x10100], bounce.data(), 0x100));
    assert(!memcmp(&phys[0x80000], bounce.data() + 0x100, 0x4000));
    assert(!memcmp(&phys[0x20000], bounce.data() + 0x4100, 0x80) && phys[0x20080] == 0xee);

    // A chain of requests back to back on the card (8, 1 and 16 sectors) and a window into it: each request the window
    // touches is staged once, at its own offset and at the window's bounce offset; nothing else.
    struct Request {
        Request *next;
        uint32_t sectors;
    };
    Request c = {nullptr, 16}, b = {&c, 1}, a = {&b, 8};
    struct Stage {
        const Request *request;
        uint32_t offset;
        uint8_t *at;
        uint32_t bytes;
    };
    std::vector<Stage> staged2;
    auto record = [&](Request *r, uint32_t offset, uint8_t *at, uint32_t n) {
        staged2.push_back({r, offset, at, n});
        return true;
    };
    uint8_t window[0x10000];
    assert(ioswitchChainCopy(&a, 3584, 2048, window, record));
    assert(staged2.size() == 3);
    assert(staged2[0].request == &a && staged2[0].offset == 3584 && staged2[0].at == window && staged2[0].bytes == 512);
    assert(staged2[1].request == &b && staged2[1].offset == 0 && staged2[1].at == window + 512 &&
           staged2[1].bytes == 512);
    assert(staged2[2].request == &c && staged2[2].offset == 0 && staged2[2].at == window + 1024 &&
           staged2[2].bytes == 1024);
    // The whole chain; a window inside one request; a window that starts in a later request.
    staged2.clear();
    assert(ioswitchChainCopy(&a, 0, 25 * 512, window, record) && staged2.size() == 3 && staged2[2].bytes == 8192);
    staged2.clear();
    assert(ioswitchChainCopy(&a, 4096 + 512 + 1024, 512, window, record) && staged2.size() == 1);
    assert(staged2[0].request == &c && staged2[0].offset == 1024 && staged2[0].bytes == 512);
    // Past the chain's end: false (nothing staged beyond it); a stage that fails stops the walk.
    staged2.clear();
    assert(!ioswitchChainCopy(&a, 4096, 25 * 512, window, record));
    unsigned stages = 0;
    assert(!ioswitchChainCopy(&a, 0, 25 * 512, window, [&](Request *, uint32_t, uint8_t *, uint32_t) {
        return ++stages < 2;
    }));
    assert(stages == 2);

    puts("PASS: sd client copy");
    return 0;
}
