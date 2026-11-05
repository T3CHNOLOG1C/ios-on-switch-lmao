// AKSKeyWrap.h and AKSCmac.h against the published vectors, with CommonCrypto standing in for libkern's AES and SHA-256
// and for the Security Engine's one-block ECB.
#include <CommonCrypto/CommonCrypto.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../AKSCmac.h"
#include "../AKSKeyWrap.h"

namespace {
void encrypt(void *key, const uint8_t in[16], uint8_t out[16])
{
    size_t moved = 0;
    if (CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 32, nullptr, in, 16, out, 16, &moved) ||
        moved != 16)
        std::abort();
}
void decrypt(void *key, const uint8_t in[16], uint8_t out[16])
{
    size_t moved = 0;
    if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 32, nullptr, in, 16, out, 16, &moved) ||
        moved != 16)
        std::abort();
}
bool encrypt128(void *key, const uint8_t in[16], uint8_t out[16])
{
    size_t moved = 0;
    return !CCCrypt(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, 16, nullptr, in, 16, out, 16, &moved) &&
           moved == 16;
}
bool failing(void *, const uint8_t[16], uint8_t[16])
{
    return false;
}
void sha256(const void *const *parts, const size_t *lengths, size_t count, uint8_t digest[32])
{
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    for (size_t i = 0; i < count; ++i) CC_SHA256_Update(&c, parts[i], (CC_LONG)lengths[i]);
    CC_SHA256_Final(digest, &c);
}
void hex(const char *s, uint8_t *out)
{
    for (size_t i = 0; s[2 * i]; ++i) std::sscanf(s + 2 * i, "%2hhx", &out[i]);
}
}  // namespace

// RFC 3394 4.3 and 4.6: a 256-bit KEK wrapping 128- and 256-bit key data; unwrap restores it, a flipped bit fails.
TEST(AKSKeyWrap, Rfc3394Vectors)
{
    uint8_t kek[32];
    hex("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", kek);
    aks::Primitives p{kek, encrypt, decrypt};
    struct {
        const char *data, *wrapped;
    } vectors[] = {
        {"00112233445566778899AABBCCDDEEFF", "64E8C3F9CE0F5BA263E9777905818A2A93C8191E7D6E8AE7"},
        {"00112233445566778899AABBCCDDEEFF000102030405060708090A0B0C0D0E0F",
         "28C9F404C4B810F4CBCCB35CFB87F8263F5786E2D80ED326CBC7F0E71A99F43BFB988B9B7A02DD21"},
    };
    for (auto &v : vectors) {
        uint8_t data[32], want[40], got[40], back[32];
        size_t n = std::strlen(v.data) / 2;
        hex(v.data, data);
        hex(v.wrapped, want);
        EXPECT_TRUE(aks::wrap(p, data, n, got) && !std::memcmp(got, want, n + 8)) << v.data;
        EXPECT_TRUE(aks::unwrap(p, got, n + 8, back) && !std::memcmp(back, data, n)) << v.data;
        got[n + 7] ^= 1;
        EXPECT_FALSE(aks::unwrap(p, got, n + 8, back)) << v.data << ": tampered";
    }
}

// RFC 4231 test case 2.
TEST(AKSKeyWrap, HmacSha256Rfc4231)
{
    uint8_t mac[32], want[32];
    const char jefe[] = "Jefe", what[] = "what do ya want for nothing?";
    const void *parts[1] = {what};
    const size_t lengths[1] = {sizeof what - 1};
    aks::hmacSha256(sha256, (const uint8_t *)jefe, 4, parts, lengths, 1, mac);
    hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", want);
    EXPECT_EQ(std::memcmp(mac, want, 32), 0);
}

// RFC 4493 4: the subkeys and examples 1-4; a cipher that fails gives no tag (zeroed).
TEST(AKSCmac, Rfc4493)
{
    uint8_t key128[16], l[16], k1[16], k2[16], zero[16] = {}, want[16];
    hex("2b7e151628aed2a6abf7158809cf4f3c", key128);
    ASSERT_TRUE(encrypt128(key128, zero, l));
    aks::cmacDouble(l, k1);
    aks::cmacDouble(k1, k2);
    hex("fbeed618357133667c85e08f7236a8de", want);
    EXPECT_EQ(std::memcmp(k1, want, 16), 0) << "K1";
    hex("f7ddac306ae266ccf90bc11ee46d513b", want);
    EXPECT_EQ(std::memcmp(k2, want, 16), 0) << "K2";
    uint8_t message[64];
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b"
        "1"
        "7ad2b417be66c3710",
        message);
    struct {
        size_t length;
        const char *tag;
    } cmacs[] = {
        {0, "bb1d6929e95937287fa37d129b756746"},
        {16, "070a16b46b4d4144f79bdd9dd04a287c"},
        {40, "dfa66747de9ae63030ca32611497c827"},
        {64, "51f0bebf7e3b9d92fc49741779363cfe"},
    };
    for (auto &c : cmacs) {
        uint8_t tag[16];
        hex(c.tag, want);
        EXPECT_TRUE(aks::cmac(encrypt128, key128, message, c.length, tag) && !std::memcmp(tag, want, 16))
            << "length " << c.length;
    }
    uint8_t refused[16];
    std::memset(refused, 0xff, sizeof refused);
    EXPECT_TRUE(!aks::cmac(failing, nullptr, message, 40, refused) && !std::memcmp(refused, zero, 16));
}

// Class keys from one device key differ by class; every accepted length round trips under its class key and fails
// under another class's; lengths outside the accepted range are refused both ways.
TEST(AKSKeyWrap, ClassKeysRoundTripsAndRefusals)
{
    uint8_t device[32], classA[32], classB[32];
    for (int i = 0; i < 32; ++i) device[i] = (uint8_t)(i * 7 + 1);
    aks::classKey(sha256, device, 0, 11, classA);
    aks::classKey(sha256, device, 0, 6, classB);
    EXPECT_NE(std::memcmp(classA, classB, 32), 0);
    aks::Primitives a{classA, encrypt, decrypt}, b{classB, encrypt, decrypt};
    for (size_t n = aks::kMinKey; n <= aks::kMaxKey; n += aks::kBlock) {
        uint8_t data[aks::kMaxKey], blob[aks::kMaxKey + 8], back[aks::kMaxKey];
        for (size_t i = 0; i < n; ++i) data[i] = (uint8_t)(i ^ n);
        EXPECT_TRUE(aks::wrap(a, data, n, blob)) << n;
        EXPECT_TRUE(aks::unwrap(a, blob, n + 8, back) && !std::memcmp(back, data, n)) << n;
        EXPECT_FALSE(aks::unwrap(b, blob, n + 8, back)) << n << ": wrong class key";
    }
    uint8_t scratch[aks::kMaxKey + 16] = {};
    EXPECT_TRUE(!aks::wrap(a, scratch, 8, scratch) && !aks::wrap(a, scratch, 20, scratch) &&
                !aks::wrap(a, scratch, aks::kMaxKey + 8, scratch));
    EXPECT_TRUE(!aks::unwrap(a, scratch, 16, scratch) && !aks::unwrap(a, scratch, 28, scratch));
}
