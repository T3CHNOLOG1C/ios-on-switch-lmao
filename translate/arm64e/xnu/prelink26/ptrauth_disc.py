#!/usr/bin/env python3
"""ptrauth string discriminators (clang's __builtin_ptrauth_string_discriminator), computed as clang does
(llvm/lib/Support/SipHash.cpp getPointerAuthStableSipHash): SipHash-2-4 of the string under a fixed key, folded to
1..0xFFFF. Pure Python, so the prelink runs on a Mac without an arm64e compiler; ptrauth_disc_test.py checks it
against clang."""
import sys

_KEY = bytes.fromhex("b5d4c9eb79104a796fec8b1b428781d4")
_MASK = (1 << 64) - 1


def _rotl(x, b):
    return ((x << b) | (x >> (64 - b))) & _MASK


def _siphash_2_4(data, key):
    k0 = int.from_bytes(key[:8], "little")
    k1 = int.from_bytes(key[8:], "little")
    v = [k0 ^ 0x736F6D6570736575, k1 ^ 0x646F72616E646F6D, k0 ^ 0x6C7967656E657261, k1 ^ 0x7465646279746573]

    def rounds(n):
        for _ in range(n):
            v[0] = (v[0] + v[1]) & _MASK
            v[1] = _rotl(v[1], 13) ^ v[0]
            v[0] = _rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & _MASK
            v[3] = _rotl(v[3], 16) ^ v[2]
            v[0] = (v[0] + v[3]) & _MASK
            v[3] = _rotl(v[3], 21) ^ v[0]
            v[2] = (v[2] + v[1]) & _MASK
            v[1] = _rotl(v[1], 17) ^ v[2]
            v[2] = _rotl(v[2], 32)

    tail = len(data) & 7
    for i in range(0, len(data) - tail, 8):
        m = int.from_bytes(data[i:i + 8], "little")
        v[3] ^= m
        rounds(2)
        v[0] ^= m
    b = ((len(data) & 0xFF) << 56) | int.from_bytes(data[len(data) - tail:], "little")
    v[3] ^= b
    rounds(2)
    v[0] ^= b
    v[2] ^= 0xFF
    rounds(4)
    return v[0] ^ v[1] ^ v[2] ^ v[3]


def string_discriminator(name):
    return _siphash_2_4(name.encode(), _KEY) % 0xFFFF + 1


def discriminators(names):
    return {n: string_discriminator(n) for n in names}


if __name__ == "__main__":
    for name in sys.argv[1:]:
        print(name, string_discriminator(name))
