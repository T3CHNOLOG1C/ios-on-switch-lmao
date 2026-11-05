// The stubs against the instructions they replace, executed side by side on Apple silicon (which has LSE, FP16, FCMA,
// JSCVT and RDM). Each harness keeps process-wide state (JIT pages, signal handlers, counters), so each run gets a
// process of its own.
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

extern "C" {
int lse_semantics_main(int argc, char **argv);
int fp_semantics_main(int argc, char **argv);
void fp_semantics_counts(uint64_t out[4]);
#include "../a64x/a64x.h"
}

namespace {

int runLse(std::initializer_list<const char *> args)
{
    char *argv[8] = {const_cast<char *>("lse_semantics")};
    int argc = 1;
    for (const char *a : args) argv[argc++] = const_cast<char *>(a);
    return lse_semantics_main(argc, argv);
}

int runFp(std::initializer_list<const char *> args)
{
    char *argv[8] = {const_cast<char *>("fp_semantics")};
    int argc = 1;
    for (const char *a : args) argv[argc++] = const_cast<char *>(a);
    return fp_semantics_main(argc, argv);
}

TEST(LseStubs, SystematicEncodingsMatchTheInstruction)
{
    switch_ios::test::ExpectPassesInFreshProcess([] { EXPECT_EQ(0, runLse({"--gen", "6"})); });
}

TEST(LseStubs, RandomEncodingsMatchTheInstruction)
{
    switch_ios::test::ExpectPassesInFreshProcess([] { EXPECT_EQ(0, runLse({"--random", "3000", "4"})); });
}

TEST(LseStubs, ACorruptedStubIsCaught)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        setenv("A64X_TEST_BREAK", "1", 1);
        EXPECT_NE(0, runLse({"--gen", "1"}));
    });
}

// Every sampled form of the class has a stub that matches the instruction; for these classes the classifier is exact
// (no form it flags is unallocated).
void expectExact(enum a64x_class cls, const char *samples)
{
    switch_ios::test::ExpectPassesInFreshProcess([cls, samples] {
        char c[8];
        snprintf(c, sizeof(c), "%d", static_cast<int>(cls));
        EXPECT_EQ(0, runFp({"300", "--gen", samples, c}));
        uint64_t n[4];
        fp_semantics_counts(n);
        EXPECT_GT(n[0], 0u);
        EXPECT_EQ(0u, n[1]) << "mismatching words";
        EXPECT_EQ(0u, n[2]) << "words without a stub";
        EXPECT_EQ(0u, n[3]) << "flagged words this host does not execute";
    });
}

TEST(FpStubs, FcmaFormsMatchTheInstruction)
{
    expectExact(A64X_FCMA, "3000000");
}
TEST(FpStubs, JscvtMatchesTheInstruction)
{
    expectExact(A64X_JSCVT, "3000000");
}

// FP16 and RDM: the classifier also flags unallocated encodings in these groups (valid code never holds them; data
// in __text is kept away by the island pass). Every form the host executes must match.
void expectMatches(enum a64x_class cls, const char *samples)
{
    switch_ios::test::ExpectPassesInFreshProcess([cls, samples] {
        char c[8];
        snprintf(c, sizeof(c), "%d", static_cast<int>(cls));
        EXPECT_EQ(0, runFp({"200", "--gen", samples, c}));
        uint64_t n[4];
        fp_semantics_counts(n);
        EXPECT_GT(n[0], 50u);
        EXPECT_EQ(0u, n[1]) << "mismatching words";
        EXPECT_EQ(0u, n[2]) << "words without a stub";
    });
}

TEST(FpStubs, Fp16FormsMatchTheInstruction)
{
    expectMatches(A64X_FP16, "3000000");
}
TEST(FpStubs, RdmFormsMatchTheInstruction)
{
    expectMatches(A64X_RDM, "3000000");
}

// The min forms of the FP16 across-lanes and scalar pairwise reductions differ from the max forms only in bit 23.
TEST(FpStubs, Fp16MinReductionsMatchTheInstruction)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        // FMINV h, FMINNMV h, FMINP h, FMINNMP h
        EXPECT_EQ(0, runFp({"2000", "0eb0fa66", "4eb0c8d3", "5eb0f97a", "5eb0c927"}));
        uint64_t n[4];
        fp_semantics_counts(n);
        EXPECT_EQ(4u, n[0]);
    });
}

}  // namespace
