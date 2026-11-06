#include "codesign.h"

#include "sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define CSMAGIC_EMBEDDED_SIGNATURE 0xFADE0CC0u
#define CSMAGIC_CODEDIRECTORY      0xFADE0C02u
#define CSMAGIC_BLOBWRAPPER        0xFADE0B01u
#define CSSLOT_CODEDIRECTORY       0u
#define CSSLOT_ALTERNATE_FIRST     0x1000u
#define CSSLOT_ALTERNATE_END       0x1005u
#define CSSLOT_SIGNATURESLOT       0x10000u
#define CS_ADHOC                   0x2u
#define CS_HASHTYPE_SHA256         2u
#define LC_CODE_SIGNATURE          0x1Du

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static uint32_t le32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static int fail(char *err, size_t errlen, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

int a64x_cs_find(const uint8_t *slice, size_t size, uint64_t *sig_off, uint64_t *sig_size)
{
    if (size < 32 || le32(slice) != 0xFEEDFACFu)
        return -1;
    uint32_t ncmds = le32(slice + 16), sizeofcmds = le32(slice + 20);
    if (sizeofcmds > size - 32)
        return -1;
    const uint8_t *p = slice + 32, *end = p + sizeofcmds;
    for (uint32_t i = 0; i < ncmds && end - p >= 16; i++) {
        uint32_t cmdsize = le32(p + 4);
        if (le32(p) == LC_CODE_SIGNATURE) {
            *sig_off = le32(p + 8);
            *sig_size = le32(p + 12);
            return *sig_off <= size && *sig_size <= size - *sig_off ? 0 : -1;
        }
        if (cmdsize < 8 || cmdsize > (size_t)(end - p))
            return -1;
        p += cmdsize;
    }
    return -1;
}

struct cd {
    const uint8_t *base; /* the CodeDirectory blob */
    uint32_t length, hash_offset, ncode, flags;
    uint8_t hash_size, hash_type, page_shift;
    uint64_t code_limit;
};

static int read_cd(const uint8_t *blob, uint64_t room, struct cd *cd, char *err, size_t errlen)
{
    if (room < 44)
        return fail(err, errlen, "CodeDirectory past the signature");
    cd->base = blob;
    cd->length = be32(blob + 4);
    uint32_t version = be32(blob + 8);
    cd->flags = be32(blob + 12);
    cd->hash_offset = be32(blob + 16);
    cd->ncode = be32(blob + 28);
    cd->code_limit = be32(blob + 32);
    cd->hash_size = blob[36];
    cd->hash_type = blob[37];
    cd->page_shift = blob[39];
    if (cd->length > room)
        return fail(err, errlen, "CodeDirectory length %u past the signature", cd->length);
    if (version >= 0x20300 && cd->length >= 64 && be64(blob + 56))
        cd->code_limit = be64(blob + 56);
    if (cd->hash_type != CS_HASHTYPE_SHA256 || cd->hash_size != 32)
        return fail(err, errlen, "hash type %u (only SHA-256 CodeDirectories are re-hashed)", cd->hash_type);
    if (!(cd->flags & CS_ADHOC))
        return fail(err, errlen, "not ad hoc (flags 0x%x)", cd->flags);
    if (cd->page_shift < 12 || cd->page_shift > 16)
        return fail(err, errlen, "page size 2^%u", cd->page_shift);
    uint64_t pages = (cd->code_limit + (1ull << cd->page_shift) - 1) >> cd->page_shift;
    if (pages != cd->ncode)
        return fail(err, errlen, "%u code slots for %llu pages", cd->ncode, (unsigned long long)pages);
    if ((uint64_t)cd->hash_offset + (uint64_t)cd->ncode * 32 > cd->length)
        return fail(err, errlen, "code slots past the CodeDirectory");
    return 0;
}

/*
 * Calls fn for every CodeDirectory of the SuperBlob; the signature slot must be empty (ad hoc: no CMS).
 * Returns how many CodeDirectories there were, or -1.
 */
static int each_cd(const uint8_t *buf, size_t size, uint64_t sig_off, uint64_t sig_size, struct cd *cds, int max,
                   char *err, size_t errlen)
{
    if (sig_off > size || sig_size > size - sig_off || sig_size < 12)
        return fail(err, errlen, "signature at 0x%llx past the file", (unsigned long long)sig_off);
    const uint8_t *sb = buf + sig_off;
    if (be32(sb) != CSMAGIC_EMBEDDED_SIGNATURE)
        return fail(err, errlen, "no embedded signature at the signature");
    uint32_t count = be32(sb + 8);
    if (12 + (uint64_t)count * 8 > sig_size)
        return fail(err, errlen, "signature index past the blob");
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = be32(sb + 12 + 8 * i), off = be32(sb + 16 + 8 * i);
        if (off + 8ull > sig_size)
            return fail(err, errlen, "signature slot 0x%x past the blob", slot);
        uint32_t magic = be32(sb + off), length = be32(sb + off + 4);
        if (slot == CSSLOT_SIGNATURESLOT) {
            if (magic != CSMAGIC_BLOBWRAPPER || length != 8)
                return fail(err, errlen, "a CMS signature (%u bytes): not ad hoc", length);
            continue;
        }
        if (slot != CSSLOT_CODEDIRECTORY && (slot < CSSLOT_ALTERNATE_FIRST || slot >= CSSLOT_ALTERNATE_END))
            continue;
        if (magic != CSMAGIC_CODEDIRECTORY)
            return fail(err, errlen, "slot 0x%x is not a CodeDirectory", slot);
        if (n == max)
            return fail(err, errlen, "more than %d CodeDirectories", max);
        if (read_cd(sb + off, sig_size - off, &cds[n], err, errlen))
            return -1;
        if (cds[n].code_limit > sig_off)
            return fail(err, errlen, "code limit past the signature");
        n++;
    }
    if (!n)
        return fail(err, errlen, "no CodeDirectory");
    return n;
}

static void page_hash(const uint8_t *buf, const struct cd *cd, uint32_t i, uint8_t out[32])
{
    uint64_t lo = (uint64_t)i << cd->page_shift, hi = lo + (1ull << cd->page_shift);
    a64x_sha256(buf + lo, (size_t)((hi < cd->code_limit ? hi : cd->code_limit) - lo), out);
}

static void cdhash_of(const struct cd *cd, uint8_t cdhash[A64X_CDHASH_SIZE])
{
    uint8_t d[32];
    a64x_sha256(cd->base, cd->length, d);
    memcpy(cdhash, d, A64X_CDHASH_SIZE);
}

int a64x_cs_rehash(uint8_t *buf, size_t size, uint64_t sig_off, uint64_t sig_size, uint8_t cdhash[A64X_CDHASH_SIZE],
                   char *err, size_t errlen)
{
    struct cd cds[4];
    int n = each_cd(buf, size, sig_off, sig_size, cds, 4, err, errlen);
    if (n < 0)
        return -1;
    for (int c = 0; c < n; c++) {
        uint8_t *slots = (uint8_t *)cds[c].base + cds[c].hash_offset;
        for (uint32_t i = 0; i < cds[c].ncode; i++)
            page_hash(buf, &cds[c], i, slots + 32ull * i);
    }
    cdhash_of(&cds[0], cdhash);
    return 0;
}

int a64x_cs_check(const uint8_t *buf, size_t size, uint64_t sig_off, uint64_t sig_size,
                  uint8_t cdhash[A64X_CDHASH_SIZE], char *err, size_t errlen)
{
    struct cd cds[4];
    int n = each_cd(buf, size, sig_off, sig_size, cds, 4, err, errlen);
    if (n < 0)
        return -1;
    for (int c = 0; c < n; c++) {
        const uint8_t *slots = cds[c].base + cds[c].hash_offset;
        for (uint32_t i = 0; i < cds[c].ncode; i++) {
            uint8_t h[32];
            page_hash(buf, &cds[c], i, h);
            if (memcmp(h, slots + 32ull * i, 32))
                return fail(err, errlen, "page %u (0x%llx) does not match its hash", i,
                            (unsigned long long)i << cds[c].page_shift);
        }
    }
    cdhash_of(&cds[0], cdhash);
    return 0;
}
