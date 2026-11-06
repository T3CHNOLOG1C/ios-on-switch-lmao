// A real program built for an A12, translated offline and run on this Mac next to the original: this kernel checks
// every translated page against the rewritten signature, dyld loads the marked image, and the stubs run for real.
#include <gtest/gtest.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include "../macho/pretranslate.h"
}

extern char **environ;

namespace {

const char *kTarget = "translate/arm64e/test/translated_target";

std::vector<uint8_t> slurp(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

std::string run(const std::string &path)
{
    const std::string out = path + ".out";
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char *argv[] = {const_cast<char *>(path.c_str()), nullptr};
    pid_t pid;
    int status = -1;
    if (posix_spawn(&pid, path.c_str(), &fa, nullptr, argv, environ) == 0)
        waitpid(pid, &status, 0);
    posix_spawn_file_actions_destroy(&fa);
    std::vector<uint8_t> text = slurp(out.c_str());
    return std::to_string(status) + ":" + std::string(text.begin(), text.end());
}

TEST(RunTranslated, PrintsWhatTheOriginalPrints)
{
    std::vector<uint8_t> in = slurp(kTarget);
    ASSERT_GT(in.size(), 4096u);
    a64x_macho m;
    char err[256];
    ASSERT_EQ(a64x_macho_parse(in.data(), in.size(), &m, err, sizeof(err)), 0) << err;
    std::vector<uint8_t> out(in.size());
    a64x_pt_result r;
    ASSERT_EQ(a64x_pretranslate(&m, out.data(), &r, err, sizeof(err)), 0) << err;
    ASSERT_EQ(r.status, A64X_PT_TRANSLATED) << r.why;
    EXPECT_GT(r.sites, 3u) << "the target holds LSE atomics";
    EXPECT_GT(r.stats.rewrite1[A64X_RCPC_LDAPR] + r.stats.stubs[A64X_FP16], 0u);
    a64x_pt_check v;
    ASSERT_GE(a64x_pt_verify(&m, out.data(), &v, err, sizeof(err)), 0) << err;
    EXPECT_EQ(v.left_baseline, 0u);

    const std::string dir = getenv("TEST_TMPDIR") ? getenv("TEST_TMPDIR") : "/tmp";
    const std::string original = dir + "/original", translated = dir + "/translated";
    for (const auto &[path, bytes] : {std::pair{original, &in}, std::pair{translated, &out}}) {
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(bytes->data()), bytes->size());
        chmod(path.c_str(), 0755);
    }
    const std::string want = run(original), got = run(translated);
    EXPECT_EQ(want.substr(0, 2), "0:") << want;
    EXPECT_EQ(got, want);
}

}  // namespace
