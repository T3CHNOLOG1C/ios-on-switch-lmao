// xnu_memcmp_harness.c's two halves: XNU's arm64 memcmp/bcmp against the byte loops they replace. Each returns how
// many cases differed (the first ten are printed) and adds the cases it ran to *cases.
#ifndef SWITCH_XNU_MEMCMP_HARNESS_H
#define SWITCH_XNU_MEMCMP_HARNESS_H

#ifdef __cplusplus
extern "C" {
#endif

// Every length up to 80 bytes at every misalignment, the first difference at every position and sign, both buffers
// ending right before a PROT_NONE page.
unsigned long xnu_memcmp_exhaustive(unsigned long *cases);
// `rounds` random cases from `seed`: lengths up to 3 pages (every other round up to 64 bytes), random alignments.
unsigned long xnu_memcmp_random(unsigned seed, int rounds, unsigned long *cases);

#ifdef __cplusplus
}
#endif

#endif  // SWITCH_XNU_MEMCMP_HARNESS_H
