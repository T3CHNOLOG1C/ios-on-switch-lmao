"""The manifest round-trips, and every record the installer would refuse is refused on the Mac first."""

import hashlib
import struct
import unittest
import uuid

from tools.install import manifest, pack

CONTAINER = uuid.UUID('50F8A67F-AFC1-4A1B-BCBB-82162674C67F')


def sample(**overrides: object):
    m = manifest.Manifest(64 << 30, 4096, CONTAINER, 'ios-on-switch-lmao 1.0.0 21H461')
    m.parts = [manifest.Part('switchroot/ios/install/part-00.bin', 3 << 20)]
    m.extents = [(0, 0, 1 << 20), (8 << 20, manifest.ZERO, 64 << 10), (1 << 30, 1 << 20, 2 << 20)]
    m.hashes = [hashlib.sha256(b'chunk 0').digest()]
    for key, value in overrides.items():
        setattr(m, key, value)
    return m


class ManifestTest(unittest.TestCase):
    def test_round_trip(self):
        raw = manifest.encode(sample())
        self.assertEqual(len(raw), 128 + 256 + 3 * 24 + 32)
        back = manifest.decode(raw)
        self.assertEqual(back, sample())
        self.assertEqual(back.write_bytes, (3 << 20) + (64 << 10))
        self.assertEqual(back.copy_bytes, back.source_bytes)

    def test_header_layout(self):
        raw = manifest.encode(sample())
        self.assertEqual(raw[:8], b'SWIMAGE1')
        self.assertEqual(
            struct.unpack_from('<IIQIIQQ', raw, 8), (1, 1, 64 << 30, 4096, 3, 3 << 20, (3 << 20) + (64 << 10))
        )
        self.assertEqual(raw[48:64], CONTAINER.bytes)
        self.assertEqual(struct.unpack_from('<I', raw, 68), (manifest.HASH_CHUNK,))

    def test_crc_catches_a_flipped_byte(self):
        raw = bytearray(manifest.encode(sample()))
        raw[-1] ^= 1
        with self.assertRaisesRegex(manifest.ManifestError, 'CRC'):
            manifest.decode(bytes(raw))

    def test_refuses_bad_extents(self):
        bad = {
            'unaligned': [(0, 0, 1 << 20), (4097, 1 << 20, 2 << 20)],
            'overlap': [(0, 0, 2 << 20), (1 << 20, 2 << 20, 1 << 20)],
            'beyond the partition': [(0, 0, 1 << 20), ((64 << 30) - 4096, 1 << 20, 2 << 20)],
            'zero first': [(0, manifest.ZERO, 1 << 20), (1 << 20, 0, 3 << 20)],
            'source gap': [(0, 0, 1 << 20), (1 << 30, 2 << 20, 2 << 20)],
            'short sources': [(0, 0, 1 << 20)],
        }
        for what, extents in bad.items():
            with self.subTest(what), self.assertRaises(manifest.ManifestError):
                manifest.encode(sample(extents=extents))

    def test_refuses_oversized_parts_and_paths(self):
        with self.assertRaises(manifest.ManifestError):
            manifest.encode(sample(parts=[manifest.Part('p', 4 << 30)]))
        with self.assertRaises(manifest.ManifestError):
            manifest.encode(sample(parts=[manifest.Part('x' * 200, 3 << 20)]))
        with self.assertRaises(manifest.ManifestError):
            manifest.encode(sample(hashes=[]))
        with self.assertRaisesRegex(manifest.ManifestError, 'part a'):
            manifest.encode(sample(parts=[manifest.Part('a', 1 << 20), manifest.Part('b', 2 << 20)]))

    def test_split_keeps_parts_under_fat32_limit(self):
        extents = [(0, 0, 9 << 30), (10 << 30, manifest.ZERO, 4096)]
        sizes = manifest.split(extents)
        self.assertEqual(sum(sizes), 9 << 30)
        self.assertTrue(all(0 < s <= manifest.PART_LIMIT and s % 4096 == 0 for s in sizes))
        self.assertEqual(len(sizes), 3)


class ChunkHashTest(unittest.TestCase):
    def test_chunks_span_pieces(self):
        data = bytes(range(256)) * ((manifest.HASH_CHUNK * 2 + 4096) // 256)
        hashes = pack.ChunkHashes()
        for at in range(0, len(data), 3 << 20):
            hashes.update(data[at : at + (3 << 20)])
        chunk = manifest.HASH_CHUNK
        self.assertEqual(
            hashes.finish(), [hashlib.sha256(data[i : i + chunk]).digest() for i in range(0, len(data), chunk)]
        )


if __name__ == '__main__':
    unittest.main()
