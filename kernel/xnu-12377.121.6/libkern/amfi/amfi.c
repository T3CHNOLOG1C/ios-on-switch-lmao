#include <libkern/libkern.h>
#include <libkern/section_keywords.h>
#include <libkern/amfi/amfi.h>

SECURITY_READ_ONLY_LATE(const amfi_t*) amfi = NULL;
SECURITY_READ_ONLY_LATE(const CEKernelAPI_t*) libCoreEntitlements = NULL;

/*
 * Darwin 26 AMFI registers TrustCache interface version 5, which adds one
 * function after getUUID, so every later member of amfi_t sits one pointer
 * further. The members are type-discriminated without address diversity, so
 * a copy in this kernel's layout keeps them valid.
 */
#define AMFI_DARWIN26_TRUST_CACHE_VERSION 5
static SECURITY_READ_ONLY_LATE(amfi_t) amfi_darwin26;

void
amfi_interface_register(const amfi_t *mfi)
{
	if (amfi) {
		panic("AppleMobileFileIntegrity interface already set");
	}
	if (mfi->TrustCache.version >= AMFI_DARWIN26_TRUST_CACHE_VERSION) {
		const size_t split = __builtin_offsetof(amfi_t, OSEntitlements);

		memcpy(&amfi_darwin26, mfi, split);
		memcpy((char *)&amfi_darwin26 + split, (const char *)mfi + split + sizeof(void *),
		    sizeof(amfi_t) - split);
		mfi = &amfi_darwin26;
	}
	amfi = mfi;
}

void
amfi_core_entitlements_register(const CEKernelAPI_t *implementation)
{
	if (libCoreEntitlements) {
		panic("libCoreEntitlements interface already set");
	}
	libCoreEntitlements = implementation;
}
