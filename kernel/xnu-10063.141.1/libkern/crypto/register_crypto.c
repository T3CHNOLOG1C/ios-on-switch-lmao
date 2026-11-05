/*
 * Copyright (c) 2012 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#include <libkern/libkern.h>
#include <libkern/crypto/register_crypto.h>
#include <libkern/crypto/crypto_internal.h>
#include <libkern/section_keywords.h>

SECURITY_READ_ONLY_LATE(bool) crypto_init = false;
SECURITY_READ_ONLY_LATE(crypto_functions_t) g_crypto_funcs = NULL;

#if defined(XNU_IOSWITCH_VERIFIED_ROOT) && XNU_IOSWITCH_VERIFIED_ROOT
/*
 * IOSwitch (r17-crypto): XNU's own hashing (SHA1Update, SHA256_Update, SHA384_Update, SHA512_Update: code-signing
 * page and blob hashes, AMFI's callers through libkern, everything else in the kernel proper) goes through copies of
 * corecrypto's SHA-1/256/384/512 digest descriptors whose compress function first offers the blocks to the Tegra
 * Security Engine (drivers/TegraSE, which sets ioswitch_digest_compress_hook once its known answers and state-load
 * check pass). The hook returns 0 when it has advanced the state exactly as corecrypto's compress would, anything
 * else and corecrypto's own compress runs; init, padding, final and the digest values are corecrypto's.
 */
#include <corecrypto/ccdigest.h>

int (*volatile ioswitch_digest_compress_hook)(const struct ccdigest_info *original, ccdigest_state_t state,
    size_t nblocks, const void *data);
static SECURITY_READ_ONLY_LATE(struct crypto_functions) ioswitch_crypto_funcs;
static SECURITY_READ_ONLY_LATE(struct ccdigest_info) ioswitch_digests[4];
SECURITY_READ_ONLY_LATE(const struct ccdigest_info *) ioswitch_digest_originals[4]; /* TegraSE: the CPU reference */

#define IOSWITCH_COMPRESS(n) \
	static void \
	ioswitch_compress_##n(ccdigest_state_t state, size_t nblocks, const void *data) \
	{ \
		const struct ccdigest_info *original = ioswitch_digest_originals[n]; \
		int (*hook)(const struct ccdigest_info *, ccdigest_state_t, size_t, const void *) = \
		    ioswitch_digest_compress_hook; \
		if (hook == NULL || hook(original, state, nblocks, data) != 0) { \
			original->compress(state, nblocks, data); \
		} \
	}
IOSWITCH_COMPRESS(0)
IOSWITCH_COMPRESS(1)
IOSWITCH_COMPRESS(2)
IOSWITCH_COMPRESS(3)

static const struct ccdigest_info *
ioswitch_wrap_digest(unsigned n, const struct ccdigest_info *original)
{
	static void (*const compress[4])(ccdigest_state_t, size_t, const void *) = {
		ioswitch_compress_0, ioswitch_compress_1, ioswitch_compress_2, ioswitch_compress_3,
	};
	if (original == NULL || original->compress == NULL) {
		return original;
	}
	ioswitch_digest_originals[n] = original;
	ioswitch_digests[n] = *original;
	ioswitch_digests[n].compress = compress[n];
	return &ioswitch_digests[n];
}
#endif

int
register_crypto_functions(const crypto_functions_t funcs)
{
	if (g_crypto_funcs) {
		return -1;
	}

#if defined(XNU_IOSWITCH_VERIFIED_ROOT) && XNU_IOSWITCH_VERIFIED_ROOT
	ioswitch_crypto_funcs = *funcs;
	ioswitch_crypto_funcs.ccsha1_di = ioswitch_wrap_digest(0, funcs->ccsha1_di);
	ioswitch_crypto_funcs.ccsha256_di = ioswitch_wrap_digest(1, funcs->ccsha256_di);
	ioswitch_crypto_funcs.ccsha384_di = ioswitch_wrap_digest(2, funcs->ccsha384_di);
	ioswitch_crypto_funcs.ccsha512_di = ioswitch_wrap_digest(3, funcs->ccsha512_di);
	g_crypto_funcs = &ioswitch_crypto_funcs;
#else
	g_crypto_funcs = funcs;
#endif
	crypto_init = true;

	return 0;
}
