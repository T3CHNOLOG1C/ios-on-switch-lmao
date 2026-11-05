// The kernel window staged in bl33.bin (boot/loader/stage.h): the loader's own decoder (copy.c) reads the exact
// window back from the composed file (make_window_vectors.py: the builder's compress_window, bl33_patch and
// stage_into_bl33 on the pinned bl33.bin) at the address it was built for, as hekate loads it at 0xAA000000.
#include <gtest/gtest.h>

#include <cstring>

#include "window_vectors.h"

extern "C" {
#include "../copy.h"
}

using window_vectors::Blocks;
using window_vectors::Bytes;
using window_vectors::Read;
using window_vectors::Table;
using window_vectors::Words;

TEST(LoaderBl33Window, DecodesTheWindowFromBl33)
{
    const Bytes image = Read("bl33_image.bin");
    const Bytes window = Read("bl33_window.bin");
    const std::vector<std::array<uint64_t, 4>> blocks = Blocks("bl33_blocks.bin");
    const std::vector<uint64_t> meta = Words("bl33_meta.bin");  // the stream's offset in bl33.bin, its bytes
    ASSERT_EQ(meta.size(), 2u);
    ASSERT_LE(meta[0] + meta[1], image.size);
    Bytes out{std::make_unique<unsigned char[]>(window.size), window.size};
    std::memset(out.data.get(), 0xa5, out.size);
    // The loader's source is STAGE_SOURCE, i.e. this many bytes into bl33.bin as hekate loads it.
    const unsigned char *source = image.data.get() + meta[0];
    ASSERT_EQ(loader_unpack((uint64_t)(uintptr_t)out.data.get(), window.size, (uint64_t)(uintptr_t)source, meta[1],
                            Table(blocks), (unsigned)blocks.size(), nullptr, 0),
              0);
    EXPECT_EQ(std::memcmp(out.data.get(), window.data.get(), window.size), 0);
}
