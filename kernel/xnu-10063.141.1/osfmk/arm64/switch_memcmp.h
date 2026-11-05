/*
 * Switch (r17-launch): the first differing byte of two buffers, 8 bytes per step, for osfmk/device/subrs.c's memcmp
 * and bcmp on arm64.
 *
 * XNU's portable memcmp/bcmp compare one byte per iteration, and arm64 has no assembly version. CoreEntitlements'
 * entitlement-key lookups (a binary search whose keys share long "com.apple.private." prefixes) reach memcmp through
 * AMFI's import stub; on the Switch's A57s that byte loop was kprof's largest kernel function after the interrupt
 * unmask: 723 samples (2.9 s) per boot on r7v, 3204 over three boots. Comparing a word at a time returns exactly what
 * the byte loop returns (same first difference, same sign and value) and loads only bytes inside [0, n) of both
 * buffers: 8-byte loads while at least 8 bytes remain, then one overlapping 8-byte load ending at n (the bytes it
 * re-reads are already known equal), and a byte loop only for n < 8. Loads may be unaligned (normal memory, as
 * bcopy/memmove's).
 *
 * Self-contained so the host test (tests/test_xnu_memcmp.py) compiles this exact text from the XNU patch.
 */
#ifndef _ARM64_SWITCH_MEMCMP_H_
#define _ARM64_SWITCH_MEMCMP_H_

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the lowest differing byte is the first one");

static inline __attribute__((always_inline)) uint64_t
switch_cmp_load64(const unsigned char *p)
{
	uint64_t v;

	__builtin_memcpy(&v, p, sizeof(v));
	return v;
}

/* The index of the first byte at which a and b differ, or n if their first n bytes are equal. */
static inline __attribute__((always_inline)) size_t
switch_cmp_first_difference(const unsigned char *a, const unsigned char *b, size_t n)
{
	size_t i = 0;

	if (n < 8) {
		while (i < n && a[i] == b[i]) {
			i++;
		}
		return i;
	}
	for (; n - i >= 8; i += 8) {
		const uint64_t d = switch_cmp_load64(a + i) ^ switch_cmp_load64(b + i);

		if (d != 0) {
			return i + ((size_t)__builtin_ctzll(d) >> 3);
		}
	}
	if (i < n) {
		const uint64_t d = switch_cmp_load64(a + n - 8) ^ switch_cmp_load64(b + n - 8);

		if (d != 0) {
			return n - 8 + ((size_t)__builtin_ctzll(d) >> 3);
		}
	}
	return n;
}

#endif /* _ARM64_SWITCH_MEMCMP_H_ */
