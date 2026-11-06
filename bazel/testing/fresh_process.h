// A scenario in a process of its own, for host tests of code that keeps boot-wide state (a driver's statics, a model's
// globals): the scenario sees a fresh process, as at boot, whatever ran before it in the test binary.
//
//   TEST(SwitchTouch, Drag) { switch_ios::test::ExpectPassesInFreshProcess([] { scenarioDrag(); }); }
//   TEST(SwitchUSB, Lost) { switch_ios::test::ExpectExitInFreshProcess([] { scenarioLost(); }, 3, "panic: .*lost"); }
//
// GoogleTest's death-test machinery runs it: the "threadsafe" style re-executes this binary for just this test, which
// runs the body and exits 1 if any EXPECT/ASSERT/REQUIRE of the body failed, after printing those failures to its stderr,
// which the parent shows in its own failure ("Actual msg"). The parent never runs the body itself.
#ifndef SWITCH_BAZEL_TESTING_FRESH_PROCESS_H
#define SWITCH_BAZEL_TESTING_FRESH_PROCESS_H

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include "bazel/testing/require.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace switch_ios::test {

namespace detail {
// In the child: runs the body with every assertion result captured, prints the failures to stderr (the parent shows
// the child's stderr when the test fails; GoogleTest prints nothing of a death-test child's own), and exits 1 if there
// were any. A REQUIRE that stopped the body has recorded its failure already.
template <class Body> [[noreturn]] void runAndExit(Body &&body)
{
    ::testing::TestPartResultArray results;
    {
        ::testing::ScopedFakeTestPartResultReporter reporter(
            ::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ALL_THREADS, &results);
        try {
            std::forward<Body>(body)();
        } catch (const RequireFailed &) {
        }
    }
    int failures = 0;
    for (int i = 0; i < results.size(); ++i) {
        const ::testing::TestPartResult &result = results.GetTestPartResult(i);
        if (!result.failed())
            continue;
        ++failures;
        std::fprintf(stderr, "%s:%d: Failure\n%s\n", result.file_name() ? result.file_name() : "(unknown file)",
                     result.line_number(), result.message());
    }
    std::fflush(stdout);
    std::fflush(stderr);
    std::exit(failures ? 1 : 0);
}
}  // namespace detail

// Runs `body` in a fresh process; the test fails unless every assertion in it passed and it returned normally.
template <class Body> void ExpectPassesInFreshProcess(Body &&body)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(detail::runAndExit(std::forward<Body>(body)), ::testing::ExitedWithCode(0), "");
}

// Runs `body` in a fresh process and expects it to exit with `code` after printing something matching `stderr_regex`
// to stderr (a stand-in's panic(), say, that prints its line and exits).
template <class Body> void ExpectExitInFreshProcess(Body &&body, int code, const std::string &stderr_regex)
{
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(detail::runAndExit(std::forward<Body>(body)), ::testing::ExitedWithCode(code), stderr_regex);
}

}  // namespace switch_ios::test

#endif  // SWITCH_BAZEL_TESTING_FRESH_PROCESS_H
