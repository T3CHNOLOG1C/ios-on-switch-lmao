// What this package's source guards (*_source_test.cpp) need beyond //bazel/testing:source_text.h: positions (the order
// of two steps in a function), text after a marker, and regular expressions over a source (ECMAScript, which covers
// the guards' patterns: \w, \s, [^}]*, (?:...)).
#ifndef SWITCH_NATIVE_SWITCH_BLOCK_TEST_SOURCE_CHECKS_H
#define SWITCH_NATIVE_SWITCH_BLOCK_TEST_SOURCE_CHECKS_H

#include <gtest/gtest.h>

#include <cstddef>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace switch_ios::test {

// The first position of `needle` in `text` at or after `from`; a missing needle is a failure (and npos, so an order
// check on it fails too).
inline size_t Index(const std::string &text, std::string_view needle, size_t from = 0)
{
    const size_t at = text.find(needle, from);
    if (at == std::string::npos) ADD_FAILURE() << "not found: " << needle;
    return at;
}

// The last position of `needle` in `text`; a missing needle is a failure.
inline size_t RIndex(const std::string &text, std::string_view needle)
{
    const size_t at = text.rfind(needle);
    if (at == std::string::npos) ADD_FAILURE() << "not found: " << needle;
    return at;
}

// Everything after the first `start` (a missing `start` is a failure and gives "").
inline std::string After(const std::string &text, std::string_view start)
{
    const size_t at = text.find(start);
    if (at == std::string::npos) {
        ADD_FAILURE() << "not found: " << start;
        return {};
    }
    return text.substr(at + start.size());
}

// Whether `pattern` matches anywhere in `text`.
inline bool Search(const std::string &text, const std::string &pattern, bool ignoreCase = false)
{
    const auto flags = std::regex::ECMAScript | (ignoreCase ? std::regex::icase : std::regex::ECMAScript);
    return std::regex_search(text, std::regex(pattern, flags));
}

// The lines of `text` that `pattern` matches (a pattern anchored at ^ matches at each line's start).
inline std::vector<std::string> LinesMatching(const std::string &text, const std::string &pattern)
{
    const std::regex re(pattern);
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);)
        if (std::regex_search(line, re)) lines.push_back(line);
    return lines;
}

}  // namespace switch_ios::test

#endif  // SWITCH_NATIVE_SWITCH_BLOCK_TEST_SOURCE_CHECKS_H
