#!/usr/bin/env python3
"""A small install image as the Mac makes it, for install-ios.bin's host test (boot/installer/test).

    manifest_vectors.py MANIFEST SOURCE EXPECTED

MANIFEST and SOURCE (the parts, concatenated) are what install-ios.bin reads; EXPECTED is the partition it must leave,
written over STALE bytes. The image's boot files are BOOT_FILES, in FOLDER with the parts.
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
FOLDER = 'switchroot/ios/install/21H461'
BOOT_FILES = {'bl31.bin': 3000, 'bl33.bin': 9 * MIB + 5}


def boot_bytes(name: str) -> bytes:
    """Each boot file's bytes, which installer_test.cpp makes the same way."""
    size, seed = BOOT_FILES[name], sum(name.encode())
    period = bytes((i * 31 + seed) & 0xFF for i in range(256))
    return (period * (size // 256 + 1))[:size]


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


def chunk_hashes(data: bytes) -> list[bytes]:
    chunk = manifest.HASH_CHUNK
    return [hashlib.sha256(data[i : i + chunk]).digest() for i in range(0, len(data), chunk)]


def main() -> None:
    out_manifest, out_source, out_expected = sys.argv[1:4]
    data = source()
    hashes = chunk_hashes(data)
    parts = [manifest.Part(f'{FOLDER}/part-{i:02d}.bin', n) for i, n in enumerate(PARTS)]
    boot = [
        manifest.Boot(f'{FOLDER}/{name}', size, chunk_hashes(boot_bytes(name))) for name, size in BOOT_FILES.items()
    ]
    m = manifest.Manifest(PARTITION, BLOCK, CONTAINER, 'iPadOS 17.7.11 (21H461)', parts, EXTENTS, hashes, boot)
    Path(out_manifest).write_bytes(manifest.encode(m))
    Path(out_source).write_bytes(data)
    partition = bytearray([STALE]) * PARTITION
    for at, start, n in EXTENTS:
        partition[at : at + n] = bytes(n) if start == manifest.ZERO else data[start : start + n]
    Path(out_expected).write_bytes(bytes(partition))


if __name__ == '__main__':
    main()
