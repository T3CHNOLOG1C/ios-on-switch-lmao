// 16 KiB VM pages over the 4 KiB granule (PAGE_RATIO 4): pmap.c's write_pte_fast in both trees, as cut out of them
// (BUILD.bazel).
#include <gtest/gtest.h>

#include <csetjmp>
#include <cstdint>

typedef uint64_t pt_entry_t;

static int PAGE_SHIFT_CONST = 14;
#define ARM_PGSHIFT             12
#define PAGE_SHIFT              PAGE_SHIFT_CONST
#define PAGE_RATIO              ((1 << PAGE_SHIFT) >> ARM_PGSHIFT)
#define TEST_PAGE_RATIO_4       (PAGE_RATIO == 4)
#define ARM_PTE_EMPTY           0x0000000000000000ULL
#define ARM_PTE_COMPRESSED      0x8000000000000000ULL
#define ARM_PTE_COMPRESSED_ALT  0x4000000000000000ULL
#define ARM_PTE_COMPRESSED_MASK 0xC000000000000000ULL
#ifndef __unreachable_ok_push
#define __unreachable_ok_push
#define __unreachable_ok_pop
#endif

static std::jmp_buf panicked;

[[noreturn]] static void panic(const char *, ...)
{
    std::longjmp(panicked, 1);
}

#define write_pte_fast write_pte_fast_10063
#include "write_pte_fast_10063.h"
#undef write_pte_fast
#define write_pte_fast write_pte_fast_12377
#include "write_pte_fast_12377.h"
#undef write_pte_fast

namespace {

using WritePte = void (*)(pt_entry_t *, pt_entry_t);
constexpr WritePte kTrees[] = {write_pte_fast_10063, write_pte_fast_12377};

// A valid 4 KiB leaf: type and AF, the attribute bits in both halves, and the physical frame.
constexpr pt_entry_t kAttributes = 0x0060000000000743ULL;

struct Table {
    alignas(32) pt_entry_t pte[8];
};

TEST(XnuPageRatio, AVmPageIsFourConsecutiveHardwareFrames)
{
    PAGE_SHIFT_CONST = 14;
    for (WritePte write : kTrees) {
        for (uint64_t pa = 0x80000000; pa < 0x180000000; pa += 0x1234000) {
            Table t = {};
            write(&t.pte[0], kAttributes | pa);
            for (int i = 0; i < 4; ++i)
                EXPECT_EQ(t.pte[i], kAttributes | (pa + 0x1000ULL * i)) << std::hex << pa << " PTE " << i;
            for (int i = 4; i < 8; ++i)
                EXPECT_EQ(t.pte[i], 0u) << "past the VM page";
        }
    }
}

TEST(XnuPageRatio, EmptyAndCompressedEntriesAreRepeatedNotIncremented)
{
    PAGE_SHIFT_CONST = 14;
    for (WritePte write : kTrees) {
        for (pt_entry_t value : {ARM_PTE_EMPTY, ARM_PTE_COMPRESSED, ARM_PTE_COMPRESSED | ARM_PTE_COMPRESSED_ALT}) {
            Table t;
            for (pt_entry_t &e : t.pte)
                e = ~0ULL;
            write(&t.pte[4], value);
            for (int i = 4; i < 8; ++i)
                EXPECT_EQ(t.pte[i], value);
            for (int i = 0; i < 4; ++i)
                EXPECT_EQ(t.pte[i], ~0ULL);
        }
    }
}

TEST(XnuPageRatio, OnlyTheFirstPteOfAVmPageMayBeWritten)
{
    PAGE_SHIFT_CONST = 14;
    for (WritePte write : kTrees) {
        Table t = {};
        volatile bool caught = false;
        if (setjmp(panicked) == 0)
            write(&t.pte[1], kAttributes | 0x80000000);
        else
            caught = true;
        EXPECT_TRUE(caught);
        for (pt_entry_t e : t.pte)
            EXPECT_EQ(e, 0u);
    }
}

TEST(XnuPageRatio, FourKibVmPagesWriteOnePte)
{
    PAGE_SHIFT_CONST = 12;
    for (WritePte write : kTrees) {
        Table t = {};
        write(&t.pte[1], kAttributes | 0x80001000);
        EXPECT_EQ(t.pte[1], kAttributes | 0x80001000);
        EXPECT_EQ(t.pte[0] | t.pte[2] | t.pte[3], 0u);
    }
    PAGE_SHIFT_CONST = 14;
}

}  // namespace
