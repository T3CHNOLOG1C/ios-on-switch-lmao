// loader_unpack (boot/loader/copy.c: the loader's LZ4 decode under its cacheable regime) against the builder's own
// compress_window (make_window_vectors.py at build time), under ASan/UBSan: the exact window, then damaged streams
// and tables that must be refused without a stray access.
#include <gtest/gtest.h>

#include <cstdlib>
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

namespace {
unsigned gCalls;
void progress(uint64_t, uint64_t, uint64_t)
{
    ++gCalls;
}

class LoaderUnpack : public testing::Test {
protected:
    Bytes window = Read("unpack_window.bin");
    Bytes stream = Read("unpack_stream.bin");
    std::vector<std::array<uint64_t, 4>> blocks = Blocks("unpack_blocks.bin");
    Bytes out{std::make_unique<unsigned char[]>(window.size), window.size};

    int run(const unsigned char *source)
    {
        std::memset(out.data.get(), 0xa5, out.size);
        return loader_unpack((uint64_t)(uintptr_t)out.data.get(), window.size, (uint64_t)(uintptr_t)source, stream.size,
                             Table(blocks), (unsigned)blocks.size(), nullptr, 0);
    }
};
}  // namespace

TEST_F(LoaderUnpack, DecodesTheExactWindow)
{
    // 8 MiB: blocks of 4 MiB and a short tail, gaps, short periods and incompressible pages.
    ASSERT_EQ(window.size, 2048u * 4096u);
    EXPECT_LT(stream.size, window.size) << "the window compresses";
    EXPECT_GT(blocks.size(), Words("unpack_meta.bin").at(0)) << "more LZ4 blocks than nonzero runs (4 MiB blocks)";
    gCalls = 0;
    std::memset(out.data.get(), 0xa5, out.size);
    ASSERT_EQ(loader_unpack((uint64_t)(uintptr_t)out.data.get(), window.size, (uint64_t)(uintptr_t)stream.data.get(),
                            stream.size, Table(blocks), (unsigned)blocks.size(), progress, 0),
              0);
    EXPECT_EQ(std::memcmp(out.data.get(), window.data.get(), window.size), 0);
    EXPECT_EQ(gCalls, blocks.size() + 1) << "progress after each block and at the end";
}

// Damaged streams: refused, or (a flipped literal byte) decoded wrongly, but never outside the buffers (ASan).
TEST_F(LoaderUnpack, DamagedStreamsStayInBounds)
{
    Bytes copy{std::make_unique<unsigned char[]>(stream.size), stream.size};
    unsigned refused = 0;
    std::srand(1104);
    for (unsigned trial = 0; trial < 400; ++trial) {
        std::memcpy(copy.data.get(), stream.data.get(), stream.size);
        const unsigned at = (unsigned)std::rand() % stream.size;
        copy.data[at] ^= (unsigned char)(1u << (std::rand() % 8));
        if (trial % 4 == 0) std::memset(copy.data.get() + at, 0xff, stream.size - at > 64 ? 64 : stream.size - at);
        refused += run(copy.data.get()) != 0;
    }
    EXPECT_GT(refused, 0u);
}

// Tables: overlap, misaligned, outside the window, a gap in the stream, a short stream, stored with a wrong size.
TEST_F(LoaderUnpack, DamagedTablesRefused)
{
    const std::vector<std::array<uint64_t, 4>> saved = blocks;
    const size_t count = blocks.size();
    ASSERT_GT(count, 1u);
    for (unsigned kind = 0; kind < 6; ++kind) {
        blocks = saved;
        switch (kind) {
        case 0: blocks[1][0] = blocks[0][0]; break;
        case 1: blocks[0][0] += 512; break;
        case 2: blocks[count - 1][1] += window.size; break;
        case 3: blocks[count - 1][2] += 1; break;
        case 4: blocks[count - 1][3] -= 1; break;
        case 5: blocks[0][3] |= LOADER_BLOCK_STORED; break;
        }
        EXPECT_NE(run(stream.data.get()), 0) << "damaged table kind " << kind;
    }
}
