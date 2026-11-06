// GoogleTest's end of c_check.h: C test code's checks become failures at its own file and line.
#include "bazel/testing/c_check.h"

#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdio>

extern "C" int switch_test_check(int ok, const char *expression, const char *file, int line)
{
    if (!ok)
        ADD_FAILURE_AT(file, line) << "CHECK(" << expression << ") failed";
    return ok;
}

extern "C" void switch_test_fail(const char *file, int line, const char *format, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    ADD_FAILURE_AT(file, line) << text;
}
