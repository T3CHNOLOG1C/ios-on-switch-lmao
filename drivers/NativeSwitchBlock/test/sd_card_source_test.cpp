// Guards on NativeSwitchBlock.cpp's use of the SD card's registers (sd_card_test.cpp tests the decoders and encodings;
// the driver is not built for the host): identification reads the card's registers once at boot and writes nothing,
// and auto CMD23 is used only where it is proven and the card lists it (r14-sdcq).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::Body;
using switch_ios::test::Index;
using switch_ios::test::Search;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
const std::string &driver()
{
    static const std::string source = SourceText("../NativeSwitchBlock.cpp");
    return source;
}

std::string body(const char *signature)
{
    return Body(driver(), signature);
}
}  // namespace

// identify() reads the card's registers once at boot and writes nothing: no CMD49, no data written.
TEST(SdCardSource, IdentificationIsReadOnly)
{
    const std::string identify = body("void NativeSwitchBlock::identify()");
    for (const char *command : {"appCommand(51,", "appCommand(13,", "extRead(0, 0, kSector, regs)"})
        EXPECT_THAT(identify, HasSubstr(command));
    EXPECT_THAT(identify, Not(HasSubstr("cardCommand(49")));
    EXPECT_THAT(identify, Not(HasSubstr("w16(COMMAND")));
    const std::string ext = body("bool NativeSwitchBlock::extRead(");
    EXPECT_THAT(ext, HasSubstr("cardCommand(48, switchSdExtArgument(fno, address, length)"));
    EXPECT_THAT(ext, HasSubstr("if (!switchSdExtRange(address, length)) return false;"));
    // Once at boot, right after the bus is up (so the identification lines come first), never on resume.
    const std::string setup = body("bool NativeSwitchBlock::setup()");
    EXPECT_LT(Index(setup, "raiseBusSpeed()"), Index(setup, "identify();"));
    EXPECT_LT(Index(setup, "identify();"), Index(setup, "checkAdma("));
    EXPECT_THAT(body("void NativeSwitchBlock::resume()"), Not(HasSubstr("identify();")));
    const std::string reinit = body("bool NativeSwitchBlock::reinitialize(");
    EXPECT_THAT(reinit, HasSubstr("if (!resume && !cardCommand(9, rca << 16, RSP_R2, nullptr))"));
    EXPECT_LT(Index(reinit, "cardCommand(9, rca << 16"), Index(reinit, "cardCommand(7, rca << 16"));
}

// Auto CMD23 only for multi-block ADMA2 commands, after setup()'s A/B reads match and the SCR lists CMD23; retries and
// every SDMA command (bounce path, proofs, setup reads) stop by CMD12 as before; CMD23 writes are read back.
TEST(SdCardSource, Cmd23FramingIsProvenAndGated)
{
    const std::string &source = driver();
    const std::string issue = body("int NativeSwitchBlock::issue(");
    EXPECT_THAT(issue, HasSubstr("switchSdBlockFrame(sectors, write, cmd23)"));
    // Argument 2 before the command that uses it, and the partition guard still first.
    EXPECT_LT(Index(issue, "outside the APFS partition"), Index(issue, "w32(ARGUMENT2, frame.argument2)"));
    EXPECT_LT(Index(issue, "w32(ARGUMENT2, frame.argument2)"), Index(issue, "w16(COMMAND, frame.command)"));
    const std::string complete = body("int NativeSwitchBlock::complete(");
    EXPECT_THAT(complete, HasSubstr("if (r16(AUTO_CMD_ERROR) & 0x9f) return 8;"));
    EXPECT_THAT(complete, HasSubstr("if (!cmd23 && (r32(STOP_RESPONSE) & R1_STOP_ERRORS)) return 9;"));
    // The data path: direct (ADMA2) multi-block commands only, per direction.
    EXPECT_THAT(body("void NativeSwitchBlock::beginBatch("),
                HasSubstr("b.cmd23 = b.direct && sectors > 1 && (b.write ? cmd23Writes : cmd23Reads);"));
    // Retries go back to CMD12 (command()'s default), as do transfer()/run() (the bounce path and the proofs).
    EXPECT_THAT(body("int NativeSwitchBlock::retry("),
                HasSubstr("step = command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status);"));
    EXPECT_THAT(body("int NativeSwitchBlock::run("),
                HasSubstr("command(static_cast<uint32_t>(lba), sectors, write, descriptors, &status);"));
    EXPECT_TRUE(Search(source, R"(int command\(uint32_t lba, uint32_t sectors, bool write, unsigned descriptors, )"
                               R"(uint32_t \*status, bool cmd23 = false\);)"));
    // setup(): gated on the SCR and on the A/B check; writes only with their read-backs.
    const std::string setup = body("bool NativeSwitchBlock::setup()");
    EXPECT_THAT(setup, HasSubstr("if (!haveScr || !(scr.cmdSupport & kSdCmd23))"));
    EXPECT_LT(Index(setup, "checkCmd23(&withNs, &withoutNs)"), Index(setup, "cmd23Reads = true;"));
    EXPECT_THAT(setup, HasSubstr("cmd23Writes = admaWrites;"));
    const std::string check = body("const char *NativeSwitchBlock::checkCmd23(");
    EXPECT_THAT(check, HasSubstr("memcmp(bounce, bounce + kBytes, kBytes)"));
    EXPECT_THAT(check, HasSubstr("memset(bounce + at, with ? 0x00 : 0xff, kBytes);"));
    const std::string end = body("void NativeSwitchBlock::endBatch(");
    EXPECT_THAT(end, HasSubstr("(b.cmd23 && cmd23WriteChecks)"));
    EXPECT_THAT(end, HasSubstr("!proveWrite(first, sectors, b.cmd23)"));
    EXPECT_TRUE(Search(body("bool NativeSwitchBlock::proveWrite("), R"(if \(cmd23\) \{[^}]*cmd23Writes = false;)"));
    EXPECT_THAT(body("void NativeSwitchBlock::releaseChecks()"),
                HasSubstr("(admaWrites && cmd23Writes && cmd23WriteChecks)"));
}
