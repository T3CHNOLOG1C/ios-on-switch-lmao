// boot/loader/boot_args.c: bootargs.txt's words appended to the kernel's command line in place.
#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include "../boot_args.h"
}

namespace {

struct Result {
    int status;
    std::string line;
    int verbose;
};

Result Append(const std::string &builtin, const std::string &file)
{
    std::vector<char> line(SWITCH_COMMAND_LINE_BYTES + 8, 'X');
    std::copy(builtin.begin(), builtin.end(), line.begin());
    line[builtin.size()] = 0;
    std::vector<char> page(SWITCH_BOOT_ARGS_BYTES, 0);
    std::copy(file.begin(), file.end(), page.begin());
    int verbose = -1;
    const int status = switch_boot_args_append(line.data(), page.data(), SWITCH_BOOT_ARGS_BYTES, &verbose);
    for (size_t i = SWITCH_COMMAND_LINE_BYTES; i < line.size(); ++i) {
        EXPECT_EQ(line[i], 'X') << "wrote past the command line";
    }
    return {status, std::string(line.data()), verbose};
}

TEST(LoaderBootArgs, NoFileKeepsTheBuiltInLine)
{
    const Result r = Append("debug=0x14e", "");
    EXPECT_EQ(r.status, SWITCH_BOOT_ARGS_OK);
    EXPECT_EQ(r.line, "debug=0x14e");
    EXPECT_EQ(r.verbose, 0);
}

TEST(LoaderBootArgs, WordsAppendOneSpaceApart)
{
    const Result r = Append("debug=0x14e", "  -v\tswitch-no-wlan\r\n\n  serial=3  \n");
    EXPECT_EQ(r.status, SWITCH_BOOT_ARGS_OK);
    EXPECT_EQ(r.line, "debug=0x14e -v switch-no-wlan serial=3");
    EXPECT_EQ(r.verbose, 1);
}

TEST(LoaderBootArgs, AnEmptyBuiltInLineGetsNoLeadingSpace)
{
    const Result r = Append("", "switch-no-bt\n");
    EXPECT_EQ(r.status, SWITCH_BOOT_ARGS_OK);
    EXPECT_EQ(r.line, "switch-no-bt");
}

TEST(LoaderBootArgs, CommentsRunToTheEndOfTheLine)
{
    const Result r = Append("", "# debug build\n-v # the text console\n#switch-no-wlan\n  # -v\nswitch-no-bt");
    EXPECT_EQ(r.status, SWITCH_BOOT_ARGS_OK);
    EXPECT_EQ(r.line, "-v switch-no-bt");
    EXPECT_EQ(r.verbose, 1);
}

TEST(LoaderBootArgs, OnlyTheWholeWordIsVerbose)
{
    EXPECT_EQ(Append("", "-vx --v v").verbose, 0);
    EXPECT_EQ(Append("-v", "").verbose, 0);
}

TEST(LoaderBootArgs, AByteOrderMarkIsSkipped)
{
    const Result r = Append("", "\xef\xbb\xbf-v\n");
    EXPECT_EQ(r.status, SWITCH_BOOT_ARGS_OK);
    EXPECT_EQ(r.line, "-v");
}

TEST(LoaderBootArgs, NonAsciiWordsAreRefusedAndTheLineIsKept)
{
    for (const char *file : {"-v caf\xc3\xa9",
                             "a\x01"
                             "b",
                             "a\x7f"}) {
        const Result r = Append("debug=0x14e", file);
        EXPECT_EQ(r.status, SWITCH_BOOT_ARGS_NOT_ASCII) << file;
        EXPECT_EQ(r.line, "debug=0x14e");
    }
    EXPECT_EQ(Append("", "# caf\xc3\xa9\n-v").status, SWITCH_BOOT_ARGS_OK);
}

TEST(LoaderBootArgs, TheLineHoldsAtMost1023Bytes)
{
    const std::string builtin(1000, 'a');
    EXPECT_EQ(Append(builtin, std::string(22, 'b')).line.size(), 1023u);
    const Result r = Append(builtin, std::string(23, 'b'));
    EXPECT_EQ(r.status, SWITCH_BOOT_ARGS_TOO_LONG);
    EXPECT_EQ(r.line, builtin);
    EXPECT_EQ(Append(std::string(1023, 'a'), "").status, SWITCH_BOOT_ARGS_OK);
    EXPECT_EQ(Append(std::string(1023, 'a'), "b").status, SWITCH_BOOT_ARGS_TOO_LONG);
}

TEST(LoaderBootArgs, AFileFillingThePageStopsAtItsEnd)
{
    std::vector<char> line(SWITCH_COMMAND_LINE_BYTES, 0);
    std::vector<char> page(16, 'w');
    int verbose = 0;
    EXPECT_EQ(switch_boot_args_append(line.data(), page.data(), page.size(), &verbose), SWITCH_BOOT_ARGS_OK);
    EXPECT_EQ(std::string(line.data()), std::string(16, 'w'));
}

}  // namespace
