#!/usr/bin/env python3
"""A small install image as the Mac makes it, for install-ios.bin's host tests (boot/installer/test).

    manifest_vectors.py MANIFEST SOURCE EXPECTED

MANIFEST is the manifest at the head of IOS17.SII.001 and SOURCE the stream's body (what install-ios.bin reads from
BODY_OFFSET); EXPECTED is the MINIMUM-byte partition it must leave, written over STALE bytes. The stream is split at
PART_LIMIT, a test-sized FAT limit, and carries BOOT_FILES in its boot region.
"""

import hashlib
import struct
import sys
import uuid
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.install import apfs, manifest

BLOCK = 4096
MIB = 1 << 20
CONTAINER = 32 * MIB
MINIMUM = 16 * MIB
STALE = 0xA5
CONTAINER_UUID = uuid.UUID('50F8A67F-AFC1-4A1B-BCBB-82162674C67F')
EXTENTS = [(0, 0, 4 * MIB), (5 * MIB, manifest.ZERO, 64 << 10), (8 * MIB, 4 * MIB, 4 * MIB + 12 * 1024)]
BODY = 8 * MIB + 12 * 1024
PART_LIMIT = 80 * MIB
FAT_DIR = 'switchroot/ios'
BOOT_FILES = {'bl31.bin': (manifest.BOOT_OFFSET, 3000), 'bl33.bin': (manifest.BOOT_OFFSET + 4096, 9 * MIB + 5)}


def boot_bytes(name: str) -> bytes:
    """Each boot file's bytes, which the C++ tests make the same way (vectors.h)."""
    size, seed = BOOT_FILES[name][1], sum(name.encode())
    period = bytes((i * 31 + seed) & 0xFF for i in range(256))
    return (period * (size // 256 + 1))[:size]


def superblock() -> bytes:
    block = bytearray(BLOCK)
    block[32:36] = b'NXSB'
    struct.pack_into('<IQ', block, 36, BLOCK, CONTAINER // BLOCK)
    block[72:88] = CONTAINER_UUID.bytes
    return apfs.with_checksum(bytes(block))


def body() -> bytes:
    data = bytearray(BODY)
    for at in range(0, len(data), 4):
        struct.pack_into('<I', data, at, at * 2654435761 & 0xFFFFFFFF)
    data[:BLOCK] = superblock()
    return bytes(data)


def chunk_hashes(data: bytes) -> list[bytes]:
    chunk = manifest.HASH_CHUNK
    return [hashlib.sha256(data[i : i + chunk]).digest() for i in range(0, len(data), chunk)]


def main() -> None:
    out_manifest, out_source, out_expected = sys.argv[1:4]
    data = body()
    sizes = manifest.split(manifest.BODY_OFFSET + BODY, PART_LIMIT)
    parts = [manifest.Part(f'{FAT_DIR}/IOS17.SII.{i + 1:03d}', n) for i, n in enumerate(sizes)]
    boot = [
        manifest.Boot(name, offset, size, chunk_hashes(boot_bytes(name))) for name, (offset, size) in BOOT_FILES.items()
    ]
    label = 'iPadOS 17.7.11 (21H461)'
    m = manifest.Manifest(CONTAINER, MINIMUM, BLOCK, CONTAINER_UUID, label, parts, EXTENTS, chunk_hashes(data), boot)
    Path(out_manifest).write_bytes(manifest.encode(m))
    Path(out_source).write_bytes(data)
    partition = bytearray([STALE]) * MINIMUM
    for at, start, n in EXTENTS:
        partition[at : at + n] = bytes(n) if start == manifest.ZERO else data[start : start + n]
    Path(out_expected).write_bytes(bytes(partition))


if __name__ == '__main__':
    main()
