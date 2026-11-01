"""Check AKSKeyWrap.h on the host against the published vectors, with CommonCrypto standing in for libkern's AES-256 and SHA-256.

RFC 3394 4.3 and 4.6 (256-bit KEK, 128- and 256-bit key data), RFC 4231 test case 2 (HMAC-SHA256), then round trips of every
length the stand-in accepts, rejection of a tampered blob, of the wrong class key and of lengths outside the accepted range.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = r'''
#include <CommonCrypto/CommonCrypto.h>
#include <cstdio>
#include <cstring>
#include "AKSKeyWrap.h"

static void encrypt(void *key, const uint8_t in[16], uint8_t out[16]) {
    size_t moved = 0;
    if (CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 32, nullptr, in, 16, out, 16, &moved) || moved != 16) abort();
}
static void decrypt(void *key, const uint8_t in[16], uint8_t out[16]) {
    size_t moved = 0;
    if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 32, nullptr, in, 16, out, 16, &moved) || moved != 16) abort();
}
static void sha256(const void *const *parts, const size_t *lengths, size_t count, uint8_t digest[32]) {
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    for (size_t i = 0; i < count; ++i) CC_SHA256_Update(&c, parts[i], (CC_LONG)lengths[i]);
    CC_SHA256_Final(digest, &c);
}
static void hex(const char *s, uint8_t *out) {
    for (size_t i = 0; s[2 * i]; ++i) sscanf(s + 2 * i, "%2hhx", &out[i]);
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
    uint8_t kek[32];
    hex("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", kek);
    aks::Primitives p{kek, encrypt, decrypt};
    struct { const char *data, *wrapped; } vectors[] = {
        {"00112233445566778899AABBCCDDEEFF", "64E8C3F9CE0F5BA263E9777905818A2A93C8191E7D6E8AE7"},
        {"00112233445566778899AABBCCDDEEFF000102030405060708090A0B0C0D0E0F",
         "28C9F404C4B810F4CBCCB35CFB87F8263F5786E2D80ED326CBC7F0E71A99F43BFB988B9B7A02DD21"},
    };
    for (auto &v : vectors) {
        uint8_t data[32], want[40], got[40], back[32];
        size_t n = strlen(v.data) / 2;
        hex(v.data, data); hex(v.wrapped, want);
        CHECK(aks::wrap(p, data, n, got) && !memcmp(got, want, n + 8));
        CHECK(aks::unwrap(p, got, n + 8, back) && !memcmp(back, data, n));
        got[n + 7] ^= 1;
        CHECK(!aks::unwrap(p, got, n + 8, back));
    }
    uint8_t mac[32], want[32];
    const char jefe[] = "Jefe", what[] = "what do ya want for nothing?";
    const void *parts[1] = {what};
    const size_t lengths[1] = {sizeof what - 1};
    aks::hmacSha256(sha256, (const uint8_t *)jefe, 4, parts, lengths, 1, mac);
    hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", want);
    CHECK(!memcmp(mac, want, 32));

    uint8_t device[32], classA[32], classB[32];
    for (int i = 0; i < 32; ++i) device[i] = (uint8_t)(i * 7 + 1);
    aks::classKey(sha256, device, 0, 11, classA);
    aks::classKey(sha256, device, 0, 6, classB);
    CHECK(memcmp(classA, classB, 32));
    aks::Primitives a{classA, encrypt, decrypt}, b{classB, encrypt, decrypt};
    for (size_t n = aks::kMinKey; n <= aks::kMaxKey; n += aks::kBlock) {
        uint8_t data[aks::kMaxKey], blob[aks::kMaxKey + 8], back[aks::kMaxKey];
        for (size_t i = 0; i < n; ++i) data[i] = (uint8_t)(i ^ n);
        CHECK(aks::wrap(a, data, n, blob));
        CHECK(aks::unwrap(a, blob, n + 8, back) && !memcmp(back, data, n));
        CHECK(!aks::unwrap(b, blob, n + 8, back));
    }
    uint8_t scratch[aks::kMaxKey + 16] = {};
    CHECK(!aks::wrap(a, scratch, 8, scratch) && !aks::wrap(a, scratch, 20, scratch) && !aks::wrap(a, scratch, aks::kMaxKey + 8, scratch));
    CHECK(!aks::unwrap(a, scratch, 16, scratch) && !aks::unwrap(a, scratch, 28, scratch));
    puts("AKSKeyWrap: RFC 3394 4.3/4.6, RFC 4231 2, round trips, tamper and wrong-class rejection ok");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    work = Path(directory)
    (work / 'test.cpp').write_text(source)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-I', str(root),
                    str(work / 'test.cpp'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
