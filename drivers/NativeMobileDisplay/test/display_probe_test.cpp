// drivers/NativeMobileDisplay/DisplaySmmuProbe.h on the host, against a model of DC A (window A's START_ADDR with an assembly
// copy, the active copy and ACT_REQ; a frame boundary takes the request), its CRC unit (a hash of the frame window A reads,
// latched at the frame end) and the MC SMMU (bypass, or translation through a TegraSMMUDomain: an unmapped page or an address
// outside DRAM reads as zeros). The I/O is what NativeLegacyDisplay does: the same TegraSMMUDomain layout (scan surfaces 1:1,
// one IOVA slot mapped onto the scanned buffer). Checked: it passes on a working unit and leaves everything as it was; each
// way it can go wrong fails at its own stage, and every failure leaves window A on the physical address, the swgroup in
// bypass, the CRC unit off and the pixel restored.
#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../DisplaySmmuProbe.h"
#include "../../TegraSMMU/TegraSMMUDomain.h"

namespace {
__attribute__((format(printf, 1, 2))) std::string message(const char *format, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    return text;
}
// Every CHECK is a GoogleTest expectation with a printf-formatted message.
#define CHECK(cond, ...) EXPECT_TRUE(cond) << message(__VA_ARGS__)

constexpr uint64_t kDram = 0x80000000ull, kFrameBytes = 3 * 4096;  // a small frame: three pages
constexpr uint64_t kBufferPa = 0xf5a00000ull, kOtherPa = 0xc8000000ull, kSlot = 0x10000000ull;

enum Fault {
    kNone,
    kDeadCrc,
    kNoTranslation,
    kWrongSlot,
    kNoIdentity,
    kUnderflow,
    kNoLatch,
    kAttachFails,
    kStuckCrc,
    kNoisyCrc,  // a different CRC every frame of a still picture
};

struct Model {
    Fault fault;
    std::map<uint64_t, uint32_t> memory;  // words by physical address
    uint64_t assembly = kBufferPa, active = kBufferPa;
    bool request = false, translated = false, crcOn = false, pixelChanged = false;
    uint32_t latched = 0, savedPixel = 0, previous = 0;
    bool lag = false;  // the latched CRC trails by a frame
    // the Switch's unit: the register reads 0 for this many frame ends after the unit went on (the frame the enable waits for, and
    // the latch trailing the first CRC'd frame)
    unsigned silent = 0, boundaries = 0;
    bool partialFirst = false;  // the first CRC the unit latches is of a part of a frame (another value)
    TegraSMMUDomain domain;
    std::vector<uint32_t> tables = std::vector<uint32_t>(16 * 1024);
    unsigned attaches = 0;

    explicit Model(Fault f) : fault(f)
    {
        for (uint64_t at = 0; at < kFrameBytes; at += 4) {
            memory[kBufferPa + at] = 0xff000000u | (uint32_t)(at * 2654435761u >> 8);
            memory[kOtherPa + at] = 0xff00ff00u ^ (uint32_t)at;
        }
    }
    uint32_t read(uint64_t address) const
    {
        uint64_t pa = address;
        if (translated) {
            pa = domain.translate(address);
            if (pa == UINT64_MAX) return 0;  // SMMU fault: the read returns nothing
        }
        if (pa < kDram) return 0;  // not DRAM: no data
        auto it = memory.find(pa);
        return it == memory.end() ? 0 : it->second;
    }
    uint32_t frameCrc() const
    {
        uint32_t h = 2166136261u;
        for (uint64_t at = 0; at < kFrameBytes; at += 4) h = (h ^ read(active + at)) * 16777619u;
        return h;
    }
    // A frame boundary, then the frame-end interrupt: the CRC of the frame that just ended.
    bool boundary()
    {
        const uint32_t crc = frameCrc();  // the frame scanned before this boundary
        if (request && fault != kNoLatch) {
            active = assembly;
            request = false;
        }
        if (crcOn) {
            latched = fault == kDeadCrc    ? 0
                      : fault == kStuckCrc ? 0x1234567u
                      : fault == kNoisyCrc ? crc ^ (0x9e3779b9u * (boundaries + 1))
                      : lag                ? previous
                                           : crc;
            if (boundaries < silent)
                latched = 0;
            else if (partialFirst && boundaries == silent && latched)
                latched ^= 0x5a5a5a5au;
            ++boundaries;
        }
        previous = crc;
        return fault == kUnderflow && translated && active == kSlot;
    }
    // NativeLegacyDisplay's side.
    void tweakPixel()
    {
        savedPixel = memory[kBufferPa];
        memory[kBufferPa] = savedPixel ^ 0x00010101u;
        pixelChanged = true;
    }
    void restorePixel()
    {
        memory[kBufferPa] = savedPixel;
        pixelChanged = false;
    }
    bool attach()
    {
        ++attaches;
        if (fault == kAttachFails) return false;
        domain.init(tables.data(), 0xe6000000ull, 16);
        if (fault != kNoIdentity) domain.identity(kBufferPa, kFrameBytes, false);
        domain.identity(kOtherPa, kFrameBytes, false);
        uint64_t pages[3];
        for (unsigned i = 0; i < 3; ++i) pages[i] = (fault == kWrongSlot ? kOtherPa : kBufferPa) + i * 4096ull;
        domain.map(kSlot, pages, 3, false);
        translated = fault != kNoTranslation;
        return true;
    }
    void scan(bool iova)
    {
        assembly = iova ? kSlot : kBufferPa;
        request = true;
    }
    void detach() { translated = false; }
    void crcOff() { crcOn = false; }
};

DisplaySmmuProbe run(Model &m)
{
    DisplaySmmuProbe probe;
    m.crcOn = true;
    probe.start();
    for (unsigned frame = 0; frame < 60 && probe.active(); ++frame) {
        const bool underflow = m.boundary();
        if (probe.perform(probe.frameEnd(m.latched, m.request, underflow), m)) break;
    }
    // what the driver leaves behind: the last request taken at the next boundary
    m.boundary();
    return probe;
}

struct Latch {
    bool lag = false;
    unsigned silent = 0;
    bool partialFirst = false;
};

void expect(Fault f, DisplaySmmuProbe::Stage stage, const char *what, Latch latch = {})
{
    Model m(f);
    m.lag = latch.lag;
    m.silent = latch.silent;
    m.partialFirst = latch.partialFirst;
    const uint32_t before = m.memory[kBufferPa];
    DisplaySmmuProbe p = run(m);
    if (f == kNone) {
        CHECK(p.stage == DisplaySmmuProbe::kDone, "%s: ended %s (%s)", what, DisplaySmmuProbe::name(p.stage), p.why);
        // within the direct probe's swap hold (NativeLegacyDisplay kProbeHoldMs 600: 36 frames at 60 Hz)
        CHECK(p.frames <= 30, "%s: %u frame ends", what, p.frames);
    } else
        CHECK(p.stage == DisplaySmmuProbe::kFailed && p.failedAt == stage,
              "%s: ended %s at %s (%s), wanted a failure at %s", what, DisplaySmmuProbe::name(p.stage),
              DisplaySmmuProbe::name(p.failedAt), p.why, DisplaySmmuProbe::name(stage));
    // (a controller that never takes a request still has the last one out: it is for the physical address too)
    CHECK(m.active == kBufferPa && (!m.request || m.assembly == kBufferPa),
          "%s: window A left at %#llx (request %d to %#llx)", what, (unsigned long long)m.active, m.request,
          (unsigned long long)m.assembly);
    CHECK(!m.translated && !m.crcOn && !m.pixelChanged && m.memory[kBufferPa] == before,
          "%s: left translated %d crc %d pixel %d", what, m.translated, m.crcOn, m.pixelChanged);
    std::printf("%-34s %2u frame ends, %s%s%s\n", what, p.frames,
                p.stage == DisplaySmmuProbe::kDone ? "passed" : "failed at ",
                p.stage == DisplaySmmuProbe::kDone ? "" : DisplaySmmuProbe::name(p.failedAt),
                p.stage == DisplaySmmuProbe::kDone ? "" : (std::string(": ") + p.why).c_str());
}
}  // namespace

TEST(DisplaySmmuProbe, WorkingUnitPasses)
{
    expect(kNone, DisplaySmmuProbe::kIdle, "working unit");
}
TEST(DisplaySmmuProbe, DeadCrcUnit)
{
    expect(kDeadCrc, DisplaySmmuProbe::kCrc, "CRC unit reads 0");
}
TEST(DisplaySmmuProbe, StuckCrcUnit)
{
    expect(kStuckCrc, DisplaySmmuProbe::kPixel, "CRC unit stuck");
}
TEST(DisplaySmmuProbe, AttachRefused)
{
    expect(kAttachFails, DisplaySmmuProbe::kAttached, "attach refused");
}
TEST(DisplaySmmuProbe, ScannedBufferNotIdentityMapped)
{
    expect(kNoIdentity, DisplaySmmuProbe::kAttached, "scanned buffer not identity-mapped");
}
TEST(DisplaySmmuProbe, SwgroupNotTranslated)
{
    expect(kNoTranslation, DisplaySmmuProbe::kIova, "swgroup not translated");
}
TEST(DisplaySmmuProbe, SlotMappedToOtherPages)
{
    expect(kWrongSlot, DisplaySmmuProbe::kIova, "slot mapped to other pages");
}
TEST(DisplaySmmuProbe, UnderflowThroughTheSmmu)
{
    expect(kUnderflow, DisplaySmmuProbe::kIova, "underflow through the SMMU");
}
TEST(DisplaySmmuProbe, StartAddressNeverTaken)
{
    expect(kNoLatch, DisplaySmmuProbe::kIova, "start address never taken");
}

// A latched CRC one frame behind: still a pass on a working unit, still caught at the slot.
TEST(DisplaySmmuProbe, CrcAFrameLate)
{
    expect(kNone, DisplaySmmuProbe::kIdle, "working unit, CRC a frame late", {true});
    expect(kNoTranslation, DisplaySmmuProbe::kIova, "untranslated, CRC a frame late", {true});
    expect(kWrongSlot, DisplaySmmuProbe::kIova, "wrong slot, CRC a frame late", {true});
}

// On the Switch the register reads 0 at the second frame end after the unit went on and the CRC from the third; a 0 taken
// as the base would fail a working unit as "not stable". A 0 is never a sample: the probe reads on at each frame end until
// two agree, and every fault is still caught at its own stage.
TEST(DisplaySmmuProbe, FirstLatchLate)
{
    for (unsigned silent : {2u, 3u, 5u}) {
        const Latch late = {true, silent};
        expect(kNone, DisplaySmmuProbe::kIdle, "working unit, first CRC late", late);
        expect(kStuckCrc, DisplaySmmuProbe::kPixel, "stuck unit, first CRC late", late);
        expect(kNoIdentity, DisplaySmmuProbe::kAttached, "no identity, first CRC late", late);
        expect(kNoTranslation, DisplaySmmuProbe::kIova, "untranslated, first CRC late", late);
        expect(kWrongSlot, DisplaySmmuProbe::kIova, "wrong slot, first CRC late", late);
    }
}

TEST(DisplaySmmuProbe, NeverAZeroSample)
{
    // a unit that never latches fails at the first stage, saying so, with no CRC taken as the base
    Model m(kDeadCrc);
    DisplaySmmuProbe p = run(m);
    CHECK(p.stage == DisplaySmmuProbe::kFailed && p.failedAt == DisplaySmmuProbe::kCrc && p.base == 0 && !p.firstLatch,
          "dead unit: ended %s at %s (%s), base %#x", DisplaySmmuProbe::name(p.stage),
          DisplaySmmuProbe::name(p.failedAt), p.why, p.base);
    CHECK(std::string(p.why).find("reads 0") != std::string::npos, "dead unit: why %s", p.why);
    // and the working unit's base is the frame's CRC, never the 0 before the first latch
    Model w(kNone);
    w.silent = 3;
    DisplaySmmuProbe q = run(w);
    CHECK(q.stage == DisplaySmmuProbe::kDone && q.base != 0 && q.base == w.frameCrc() && q.firstLatch == 4,
          "late unit: ended %s, base %#x (frame %#x), first latch at frame end %u", DisplaySmmuProbe::name(q.stage),
          q.base, w.frameCrc(), q.firstLatch);
}

// The first CRC latched after the enable covers part of a frame: it does not repeat, so it is not the base.
TEST(DisplaySmmuProbe, PartialFirstFrame)
{
    expect(kNone, DisplaySmmuProbe::kIdle, "working unit, partial first CRC", {false, 2, true});
    expect(kNone, DisplaySmmuProbe::kIdle, "working unit, partial first CRC, late", {true, 2, true});
}

// A CRC that moves on a still frame never settles: the unit is not trusted.
TEST(DisplaySmmuProbe, NoisyCrcUnit)
{
    expect(kNoisyCrc, DisplaySmmuProbe::kCrc, "CRC unit noisy");
}
