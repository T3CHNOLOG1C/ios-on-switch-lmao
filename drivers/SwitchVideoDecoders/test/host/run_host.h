// Runs a C test host's main() (renamed by its host_library) with its stdout captured, and echoes what it printed into
// the test's log.
#ifndef SWITCH_SWITCH_VIDEO_DECODERS_TEST_HOST_RUN_HOST_H
#define SWITCH_SWITCH_VIDEO_DECODERS_TEST_HOST_RUN_HOST_H

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

struct HostRun {
    int code;
    std::string out;
};

template <class Main> HostRun RunHost(Main main, std::vector<std::string> args = {})
{
    std::vector<char *> argv;
    for (std::string &arg : args)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    testing::internal::CaptureStdout();
    const int code = main(static_cast<int>(args.size()), argv.data());
    HostRun run{code, testing::internal::GetCapturedStdout()};
    std::fputs(run.out.c_str(), stdout);
    return run;
}

#endif  // SWITCH_SWITCH_VIDEO_DECODERS_TEST_HOST_RUN_HOST_H
