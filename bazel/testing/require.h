// REQUIRE(condition): a check that stops the scenario, like assert() but never compiled out (tests also run under
// --config=release, where NDEBUG empties assert()) and reported by GoogleTest. Use it where code after the check
// relies on it (an index, a pointer, a length); otherwise prefer EXPECT_* so one run reports every difference.
//
//   REQUIRE(result.count == 3);              // a GoogleTest failure at this line, then the TEST (or scenario) ends
//
// It works in any function (ASSERT_* only returns from void ones): the failure is recorded, then RequireFailed is
// thrown; GoogleTest ends the TEST there, and a fresh-process scenario (fresh_process.h) exits failed.
#ifndef IOSWITCH_BAZEL_TESTING_REQUIRE_H
#define IOSWITCH_BAZEL_TESTING_REQUIRE_H

#include <gtest/gtest.h>

namespace ioswitch::test {
// Not a std::exception: a model's `catch (const std::exception &)` must not swallow a failed check.
struct RequireFailed {};
}  // namespace ioswitch::test

#define REQUIRE(...)                                                                                                   \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            ADD_FAILURE() << "REQUIRE(" #__VA_ARGS__ ") failed";                                                       \
            throw ::ioswitch::test::RequireFailed();                                                                   \
        }                                                                                                              \
    } while (0)

#endif  // IOSWITCH_BAZEL_TESTING_REQUIRE_H
