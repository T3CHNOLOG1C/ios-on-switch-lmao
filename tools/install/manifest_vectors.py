#!/usr/bin/env python3
"""A small install image as the Mac makes it, for install-ios.bin's host test (boot/installer/test).

    manifest_vectors.py MANIFEST SOURCE EXPECTED

MANIFEST and SOURCE (the parts, concatenated) are what install-ios.bin reads; EXPECTED is the partition it must leave,
written over STALE bytes.
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
PARTITION = 16 << 20
STALE = 0xA5
CONTAINER = uuid.UUID('50F8A67F-AFC1-4A1B-BCBB-82162674C67F')
MIB = 1 << 20
EXTENTS = [(0, 0, 4 * MIB), (5 * MIB, manifest.ZERO, 64 << 10), (8 * MIB, 4 * MIB, 4 * MIB + 12 * 1024)]
PARTS = [8 * MIB, 12 * 1024]


def superblock() -> bytes:
    block = bytearray(BLOCK)
    block[32:36] = b'NXSB'
    struct.pack_into('<IQ', block, 36, BLOCK, PARTITION // BLOCK)
    block[72:88] = CONTAINER.bytes
    return apfs.with_checksum(bytes(block))


def source() -> bytes:
    data = bytearray(sum(PARTS))
    for at in range(0, len(data), 4):
        struct.pack_into('<I', data, at, at * 2654435761 & 0xFFFFFFFF)
    data[:BLOCK] = superblock()
    return bytes(data)


def main() -> None:
    out_manifest, out_source, out_expected = sys.argv[1:4]
    data = source()
    hashes = [
        hashlib.sha256(data[i : i + manifest.HASH_CHUNK]).digest() for i in range(0, len(data), manifest.HASH_CHUNK)
    ]
    parts = [manifest.Part(f'switchroot/ios/install/part-{i:02d}.bin', n) for i, n in enumerate(PARTS)]
    m = manifest.Manifest(PARTITION, BLOCK, CONTAINER, 'iPadOS 17.7.11 (21H461)', parts, EXTENTS, hashes)
    Path(out_manifest).write_bytes(manifest.encode(m))
    Path(out_source).write_bytes(data)
    partition = bytearray([STALE]) * PARTITION
    for at, start, n in EXTENTS:
        partition[at : at + n] = bytes(n) if start == manifest.ZERO else data[start : start + n]
    Path(out_expected).write_bytes(bytes(partition))


if __name__ == '__main__':
    main()
