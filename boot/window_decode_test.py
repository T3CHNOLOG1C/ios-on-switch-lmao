"""The staged kernel window of this build decodes to the layout image: every block of runs.h (the table the loader
compiles in) taken from kernel-window.bin, LZ4 blocks through lz4 -d (each wrapped in a one-block LZ4 frame), stored
blocks as they are, laid at their offsets in an empty window, gives exactly the bytes the layout made (plan.json's
image: its size and SHA-256). So the compression (its level, its block size, its shards) changes the staged bytes,
never the kernel the loader copies into place.

usage: window_decode_test.py LZ4 KERNEL-WINDOW-FILES...   (//boot:package's kernel_window output group)
"""

import hashlib
import json
import re
import struct
import subprocess
import sys
import unittest
from pathlib import Path

STORED = 1 << 63
LZ4_MAGIC = b'\x04\x22\x4d\x18'


def xxh32(data: bytes, seed: int = 0) -> int:
    """XXH32 of a short input (fewer than 16 bytes: the LZ4 frame descriptor's header checksum)."""
    p1, p2, p3, p4, p5 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D, 0x27D4EB2F, 0x165667B1
    mask = 0xFFFFFFFF

    def rotl(x, r):
        return ((x << r) | (x >> (32 - r))) & mask

    if len(data) >= 16:
        raise ValueError('xxh32 here takes fewer than 16 bytes')
    h = (seed + p5 + len(data)) & mask
    at = 0
    while at + 4 <= len(data):
        h = (rotl((h + struct.unpack_from('<I', data, at)[0] * p3) & mask, 17) * p4) & mask
        at += 4
    for byte in data[at:]:
        h = (rotl((h + byte * p5) & mask, 11) * p1) & mask
    h ^= h >> 15
    h = (h * p2) & mask
    h ^= h >> 13
    h = (h * p3) & mask
    return h ^ (h >> 16)


def one_block_frame(block: bytes) -> bytes:
    """An LZ4 frame holding one compressed block: version 1, independent blocks, no checksums, 4 MiB maximum."""
    descriptor = bytes([0x60, 0x70])
    header = LZ4_MAGIC + descriptor + bytes([(xxh32(descriptor) >> 8) & 0xFF])
    return header + struct.pack('<I', len(block)) + block + struct.pack('<I', 0)


def runs_table(text: str) -> list[tuple[int, int, int, int]]:
    count = int(re.search(r'#define LOAD_BLOCK_COUNT (\d+)u', text).group(1))
    rows = re.findall(r'\{UINT64_C\((0x[0-9a-f]+)\), UINT64_C\((0x[0-9a-f]+)\), UINT64_C\((0x[0-9a-f]+)\), '
                      r'UINT64_C\((0x[0-9a-f]+)\)\}', text)
    if len(rows) != count:
        raise ValueError(f'runs.h: {len(rows)} rows for LOAD_BLOCK_COUNT {count}')
    return [tuple(int(v, 16) for v in row) for row in rows]


class WindowDecodeTest(unittest.TestCase):
    def test_window_decodes_to_the_layout_image(self):
        files = {Path(f).name: Path(f) for f in FILES}
        plan = json.loads(files['plan.json'].read_text())
        constants = json.loads(files['constants.json'].read_text())
        table = runs_table(files['runs.h'].read_text())
        staged = files['kernel-window.bin'].read_bytes()
        self.assertEqual(len(staged), constants['PACKED_BYTES'])
        self.assertEqual(int.from_bytes(hashlib.sha256(staged).digest()[:8], 'little'), constants['STAGE_ID'])
        self.assertEqual([(at, length) for at, length, _, _ in table], [tuple(b) for b in plan['blocks']])
        window = bytearray(constants['BYTES'])
        read = 0
        for at, length, source, size in table:
            stored, size = bool(size & STORED), size & ~STORED
            self.assertEqual(source, read)
            body = staged[source:source + size]
            if stored:
                decoded = body
            else:
                decoded = subprocess.run([LZ4, '-d', '-c', '-q'], input=one_block_frame(body), capture_output=True,
                                         check=True).stdout
            self.assertEqual(len(decoded), length, f'block at {at:#x}')
            window[at:at + length] = decoded
            read += size
        self.assertEqual(read, constants['STREAM_BYTES'])
        self.assertFalse(any(staged[read:]), 'the staged window is padded with zeros')
        self.assertEqual(len(window), plan['image']['bytes'])
        self.assertEqual(hashlib.sha256(window).hexdigest(), plan['image']['sha256'])


if __name__ == '__main__':
    from python.runfiles import runfiles

    found = runfiles.Create()
    LZ4, *FILES = [found.Rlocation(a) or a for a in sys.argv[1:]]
    unittest.main(argv=sys.argv[:1])
