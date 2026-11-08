// SPDX-License-Identifier: MIT
// A compute state's pack key at one threadgroup shape, as the pack's generator (pack/probecompute.mm) and the facade
// (RawMetalCompute.mm) both make it: what m2v compiles a kernel's program from. Its function by its AIR (raw_metallib.h: the
// same bitcode whatever library or path it came by), the function constants as the journal writes them, the descriptor's
// thread limits, the threads per threadgroup (m2v's local size), and the threadgroup memory lengths (m2v compiles each into
// the program, as NAK sizes shared memory by it).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include "raw_metallib.h"
#include "raw_pack.h"

namespace raw {

inline std::string computeKey(const AirHash &air, const std::string &constants, uint32_t maxThreads,
                              bool multipleOfWidth, const uint32_t threads[3], const std::string &threadgroupMemory)
{
    std::string key;
    char b[96];
    for (uint8_t x : air) {
        snprintf(b, sizeof b, "%02x", x);
        key += b;
    }
    snprintf(b, sizeof b, " | max=%u tgm=%d | tg=%u,%u,%u | tgmem=", maxThreads, multipleOfWidth ? 1 : 0, threads[0],
             threads[1], threads[2]);
    return key + " | " + constants + b + threadgroupMemory;
}

inline uint64_t computeKeyHash(const std::string &key)
{
    return Pack::fnv(key.data(), key.size());
}

}  // namespace raw
