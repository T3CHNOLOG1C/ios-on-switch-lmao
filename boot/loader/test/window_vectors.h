// The build-time window vectors (make_window_vectors.py, :window_vectors) as the decoder tests read them.
#ifndef SWITCH_TEST_WINDOW_VECTORS_H
#define SWITCH_TEST_WINDOW_VECTORS_H

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

namespace window_vectors {

// A file's bytes in a heap block of exactly its size, so AddressSanitizer reports a read one byte past it.
struct Bytes {
    std::unique_ptr<unsigned char[]> data;
    size_t size = 0;
};

inline Bytes Read(const char *name)
{
    const std::string text = switch_ios::test::SourceText(name);
    Bytes bytes{std::make_unique<unsigned char[]>(text.size()), text.size()};
    std::memcpy(bytes.data.get(), text.data(), text.size());
    return bytes;
}

// Little-endian u64s (the vectors are written on, and read on, little-endian machines).
inline std::vector<uint64_t> Words(const char *name)
{
    const std::string text = switch_ios::test::SourceText(name);
    std::vector<uint64_t> words(text.size() / 8);
    std::memcpy(words.data(), text.data(), words.size() * 8);
    return words;
}

// The block table: {window offset, length, stream offset, stream bytes | LOADER_BLOCK_STORED}.
inline std::vector<std::array<uint64_t, 4>> Blocks(const char *name)
{
    const std::vector<uint64_t> words = Words(name);
    std::vector<std::array<uint64_t, 4>> blocks(words.size() / 4);
    for (size_t i = 0; i < blocks.size(); ++i)
        for (size_t j = 0; j < 4; ++j) blocks[i][j] = words[4 * i + j];
    return blocks;
}

inline const uint64_t (*Table(const std::vector<std::array<uint64_t, 4>> &blocks))[4]
{
    return reinterpret_cast<const uint64_t (*)[4]>(blocks.data());
}

}  // namespace window_vectors

#endif  // SWITCH_TEST_WINDOW_VECTORS_H
