// Guards on what NativeIOSwitchBlock.cpp's command-queue path may and may not do, and on the engine's (IOSwitchCq.h)
// cache steps the model card cannot tell apart (sd_cq_test.cpp runs the engine against the model; the driver is not
// built for the host). r14-sdcq, r16-sdcq.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <iterator>
#include <regex>
#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using ioswitch::test::Between;
using ioswitch::test::Body;
using ioswitch::test::Count;
using ioswitch::test::Index;
using ioswitch::test::Search;
using ioswitch::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
const std::string &driver()
{
    static const std::string source = SourceText("../NativeIOSwitchBlock.cpp");
    return source;
}
std::string body(const char *signature)
{
    return Body(driver(), signature);
}
const std::string &engine()
{
    static const std::string source = SourceText("../IOSwitchCq.h");
    return source;
}
// One of the engine's member functions (indented four spaces in its class).
std::string member(const char *signature)
{
    return Between(engine(), signature, "\n    }\n");
}
}  // namespace

// What the driver's command-queue path may and may not do. (The values CqBus::control lets through,
// ioswitchSdPerfWriteAddress's table in IOSwitchSdCard.h, are tested value by value in sd_card_test's
// ExtensionRegisters, and the model card refuses any other; the queue's enable value 03h likewise.)
TEST(SdCqSource, DriverGuards)
{
    const std::string &source = driver();
    // The only extension-register writes: r16-sdcq: Cache Enable 00h/01h, Flush Cache 01h and the CQ control byte
    // 00h/03h, through CqBus::control, which refuses (panics on) anything else before the command is issued.
    const std::string control = body("int NativeIOSwitchBlock::CqBus::control(");
    EXPECT_THAT(control, HasSubstr("cqAt != ioswitchSdCqControlAddress(self->extInfo.perf)"));
    EXPECT_THAT(control, HasSubstr("write ? ioswitchSdPerfWriteAddress(self->extInfo.perf, byte, *value)"));
    EXPECT_THAT(control, HasSubstr("address != cqAt - (kSdPerfCqControl - byte)"));
    EXPECT_LT(Index(control, "panic("), Index(control, "cqCommand(write ? 49 : 48"));
    EXPECT_THAT(control, HasSubstr("ioswitchSdExtArgument(self->extInfo.perf.fno, address, 1)"));
    const std::regex cmd49(R"(cqCommand\(write \? 49|cqCommand\(49|cardCommand\(49)");
    EXPECT_EQ(std::distance(std::sregex_iterator(source.begin(), source.end(), cmd49), std::sregex_iterator()), 1);
    // Interrupts, never polling: cqCommand waits by waitSignal (a lost interrupt panics); the only timed wait in the
    // queue's code is the engine's settle() between read-backs of a card slow to leave CQ mode.
    const std::string command = body("int NativeIOSwitchBlock::cqCommand(");
    EXPECT_THAT(command, HasSubstr("waitSignal(awaited,"));
    EXPECT_THAT(command, HasSubstr("panic(\"IOSwitchSD: SDMMC1 interrupt lost"));
    EXPECT_THAT(command, Not(HasSubstr("IOSleep")));
    EXPECT_THAT(command, Not(HasSubstr("IODelay")));
    // r16-sdcq: a CMD49's busy (the byte applied, a flush done) ends in transfer complete; DAT0 confirms it before the
    // read-back; the limit covers the spec's 1 s of busy or data access.
    EXPECT_THAT(command, HasSubstr("data == kRegWrite && !waitIdle(true, 1000000000)"));
    EXPECT_THAT(command, HasSubstr("limit = 1500000000ull"));
    EXPECT_THAT(body("Batch *NativeIOSwitchBlock::runGroup("), Not(HasSubstr("13, ")));  // no CMD13 task-status polling
    EXPECT_EQ(Count(engine(), "bus.settle()"), 1u);
    // r16-sdcq: every control byte read back lazily (bounded; kReadBacks is checked in sd_cq_test), the enable's value
    // kept for the line.
    const std::string enter = member("bool enter(Bus &bus)");
    EXPECT_THAT(enter, HasSubstr("readBack(bus, kSdPerfCqControl, 3, kEnabled)"));
    EXPECT_THAT(enter, HasSubstr("cqRead = lastRead[2];"));
    // The cache only inside a CQ session: on right before 03h, off right after 00h, off with the queue for good.
    EXPECT_LT(Index(enter, "enableCache(bus)"), Index(enter, "bus.control(kSdPerfCqControl, true"));
    const std::string leave = member("bool leave(Bus &bus)");
    EXPECT_LT(Index(leave, "if (!off(bus))"), Index(leave, "if (useCache && !disableCache(bus)) useCache = false;"));
    const std::string fail = member("bool fail(Bus &bus, const char *reason, bool maybeOn = false)");
    EXPECT_THAT(fail, HasSubstr("if (out) (void)disableCache(bus);"));
    EXPECT_THAT(fail, HasSubstr("useCache = false;"));
    // Off is proven only by a CMD49 the card answered; no flush on the way (nothing completed is held).
    const std::string disable = member("bool disableCache(Bus &bus)");
    EXPECT_THAT(disable, HasSubstr("if (bus.control(kSdPerfCacheControl, true, &value)) return false;"));
    EXPECT_THAT(disable, Not(HasSubstr("flush(")));
    EXPECT_THAT(engine(), HasSubstr("bool commitWrite(Bus &bus) { return !cacheOn || flush(bus); }"));
    EXPECT_THAT(member("void powerCycled()"), HasSubstr("cacheOn = false;"));  // Cache Enable is 0 after power-up
    // Reads only: a task write panics before anything is issued.
    const std::string issue = body("int NativeIOSwitchBlock::issue(");
    EXPECT_THAT(issue, HasSubstr("if (write && task != kNoTask) panic("));
    EXPECT_LT(Index(issue, "if (write && task != kNoTask) panic("), Index(issue, "w16(COMMAND"));
    const std::string group = body("unsigned NativeIOSwitchBlock::groupLocked(");
    EXPECT_THAT(group, HasSubstr("first->write || sectors > kCqTaskSectors"));
    EXPECT_THAT(group, HasSubstr("ioswitchNextIndependentRead(head, kCqTaskSectors, kMergeScan)"));
    EXPECT_THAT(group, HasSubstr("if (payload != sectors && gapProofs) return 0;"));
    // Every task's blocks inside the APFS partition, checked before the group is assigned.
    const std::string run = body("Batch *NativeIOSwitchBlock::runGroup(");
    EXPECT_LT(Index(run, "outside the APFS partition"), Index(run, "cq.submit(cqBus, tasks, n)"));
    // The first tasks of every boot are held and compared with legacy reads (out of CQ mode) before completing.
    EXPECT_THAT(run, HasSubstr("const bool proving = cqProofs != 0;"));
    EXPECT_LT(Index(run, "if (!cq.leave(cqBus))"), Index(run, "compareChain(group[i].first"));
    EXPECT_THAT(run, HasSubstr("if (!proving) pending = &b;"));
    // A proof that differs stops the queue and the whole group is read again the legacy way (from task 0).
    EXPECT_TRUE(Search(run, R"(if \(proof == kChainDiffers\) \{[^}]*why = cqWhy;)"));
    EXPECT_THAT(run, HasSubstr("for (unsigned i = proving ? 0 : done; i < n; ++i) {"));
    EXPECT_THAT(body("bool NativeIOSwitchBlock::setup()"), HasSubstr("cqProofs = kCqProofs;"));
    // A failure stops the queue first, then the chains not completed run the legacy way.
    EXPECT_LT(Index(run, "cqStop(why);"), Index(run, "beginBatch(b);"));
    // A legacy command never finds the card in CQ mode: the worker leaves it first, and when its queue runs dry.
    const std::string worker = body("void NativeIOSwitchBlock::worker(");
    EXPECT_LT(Index(worker, "if (next) self->cqLeave();"), Index(worker, "if (next) self->beginBatch(*next);"));
    EXPECT_THAT(worker, HasSubstr("self->cq.state == CqEngine::kOn) {"));
    EXPECT_THAT(body("void NativeIOSwitchBlock::suspend()"),
                HasSubstr("if (cq.state == CqEngine::kOn) (void)cq.leave(cqBus);"));
    EXPECT_THAT(body("void NativeIOSwitchBlock::resume()"), HasSubstr("cq.powerCycled();"));
    // A card that will not leave CQ mode is power-cycled the way system sleep does it, never polled forever.
    EXPECT_THAT(body("void NativeIOSwitchBlock::cqStop("), HasSubstr("resetCard();"));
    const std::string reset = body("void NativeIOSwitchBlock::resetCard()");
    for (const char *step : {"powerOffCard();", "powerOnController();", "reinitialize(", "memcmp(again, gptHeader"})
        EXPECT_THAT(reset, HasSubstr(step));
    // Setup: only a card that offers a queue with its control byte off, and only after its test group matched.
    const std::string check = body("const char *NativeIOSwitchBlock::checkCq(");
    EXPECT_THAT(check, HasSubstr("if (perf.cqDepth < 2) return \"the card offers no queue\";"));
    EXPECT_THAT(check, HasSubstr("if (perf.cqControl & kSdCqEnable)"));
    EXPECT_THAT(check, HasSubstr("memcmp(bounce + i * kBytes, checkBuffer + i * kBytes, kBytes)"));
    EXPECT_LT(Index(check, "cq.start(perf.cqDepth, perf.cache);"), Index(check, "cq.enter(cqBus)"));
    // r16-sdcq: sessions with the card's cache (enter turns it on first, 4.19.1); a cache found on goes off before any
    // write; a failure says what read back.
    EXPECT_THAT(check, HasSubstr("cq.start(perf.cqDepth, perf.cache);"));
    EXPECT_LT(Index(check, "cq.cacheOn = true;"), Index(check, "(void)cq.disableCache(cqBus);"));
    EXPECT_LT(Index(check, "cq.fail(cqBus, why)"), Index(check, "describeCq(detail, sizeof(detail));"));
}

// r16-sdcq: write-through, nothing panics. The card's cache is on only inside a CQ session (reads only), so every write
// finds it off and completes on flash as today, synchronize has nothing to flush; a cache that would not go off has
// each write flushed before it completes, and a failed flush fails that write; and the queue only runs where its own
// measurement says it is faster.
TEST(SdCqSource, CacheContract)
{
    const std::string &source = driver();
    EXPECT_THAT(
        source,
        HasSubstr("IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }"));
    EXPECT_THAT(source, HasSubstr("IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }"));
    EXPECT_THAT(source,
                Not(HasSubstr("kIOStorageOptionForceUnitAccess")));  // every completed write is on flash already
    // Every write's last step, after every path that wrote it (direct, bounce, a proof's rewrite).
    const std::string end = body("void NativeIOSwitchBlock::endBatch(");
    EXPECT_THAT(end, HasSubstr("if (write && !result && !commitWrite()) result = kIOReturnIOError;"));
    EXPECT_LT(Index(end, "bounceChain(first, sectors)"), Index(end, "!commitWrite()"));
    EXPECT_LT(Index(end, "!commitWrite()"), Index(end, "b.result = result;"));
    const std::string commit = body("bool NativeIOSwitchBlock::commitWrite()");
    EXPECT_THAT(commit, HasSubstr("if (!cq.cacheOn) return true;"));
    EXPECT_THAT(commit, HasSubstr("cq.commitWrite(cqBus)"));
    EXPECT_THAT(commit, Not(HasSubstr("panic(")));
    // No panic and no flush on the way to sleep or a power cycle: nothing completed is held in the cache.
    for (const char *name : {"void NativeIOSwitchBlock::suspend()", "void NativeIOSwitchBlock::resetCard()",
                             "void NativeIOSwitchBlock::cqStop(", "void NativeIOSwitchBlock::resume()"})
        EXPECT_THAT(body(name), Not(HasSubstr("flush("))) << name;
    EXPECT_THAT(body("void NativeIOSwitchBlock::suspend()"), Not(HasSubstr("panic(")));
    EXPECT_FALSE(Search(source, R"(panic\([^;]*(cache|flush))", true));
    // The queue only where it is faster, and from legacy mode only for groups that pay for the way in and out.
    const std::string setup = body("bool NativeIOSwitchBlock::setup()");
    EXPECT_THAT(setup, HasSubstr("if (!cqOff && (!saving || need > cq.depth))"));
    EXPECT_THAT(setup, HasSubstr("(void)cq.fail(cqBus, \"no faster\");"));
    EXPECT_THAT(setup, HasSubstr("cqMinChains = need < 2 ? 2 : static_cast<unsigned>(need);"));
    EXPECT_THAT(body("unsigned NativeIOSwitchBlock::groupLocked("),
                HasSubstr("n >= (cq.state == CqEngine::kOn ? 1u : cqMinChains)"));
}
