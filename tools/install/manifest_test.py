"""The manifest round-trips, and every record the installer would refuse is refused on the Mac first."""

import hashlib
import struct
import unittest
import uuid

from tools.install import manifest

CONTAINER = uuid.UUID('50F8A67F-AFC1-4A1B-BCBB-82162674C67F')
DIR = 'switchroot/ios'
MIB = 1 << 20


def boot(name: str, offset: int, size: int) -> manifest.Boot:
    return manifest.Boot(name, offset, size, [hashlib.sha256(name.encode()).digest()] * manifest.chunks(size))


def sample(**overrides: object) -> manifest.Manifest:
    m = manifest.Manifest(1 << 40, 32 << 30, 4096, CONTAINER, 'iPadOS 17.7.11 (21H461)')
    m.extents = [(0, 0, MIB), (8 * MIB, manifest.ZERO, 64 << 10), (1 << 30, MIB, 2 * MIB)]
    m.hashes = [hashlib.sha256(b'chunk 0').digest()]
    m.parts = [manifest.Part(f'{DIR}/IOS17.SII.001', manifest.BODY_OFFSET + 3 * MIB)]
    m.boot = [
        boot('bl31.bin', manifest.BOOT_OFFSET, 45655),
        boot('bl33.bin', manifest.BOOT_OFFSET + 49152, 12 * MIB + 1),
    ]
    for key, value in overrides.items():
        setattr(m, key, value)
    return m


class ManifestTest(unittest.TestCase):
    def test_round_trip(self):
        raw = manifest.encode(sample())
        self.assertEqual(len(raw), 128 + 3 * 256 + 3 * 24 + 4 * 32)
        self.assertEqual(manifest.decode(raw + bytes(100)), sample())

    def test_header_layout(self):
        raw = manifest.encode(sample())
        self.assertEqual(raw[:8], b'SWIMAGE1')
        self.assertEqual(
            struct.unpack_from('<IIQIIQQ', raw, 8), (3, 1, 1 << 40, 4096, 3, 3 * MIB, 3 * MIB + (64 << 10))
        )
        self.assertEqual(raw[48:64], CONTAINER.bytes)
        self.assertEqual(raw[72:112].rstrip(b'\0'), b'iPadOS 17.7.11 (21H461)')
        self.assertEqual(struct.unpack_from('<QII', raw, 112), (32 << 30, 2, 0))
        self.assertEqual(struct.unpack_from('<200sQQ', raw, 128 + 256)[1:], (45655, manifest.BOOT_OFFSET))
        self.assertEqual(raw[-32:], hashlib.sha256(b'bl33.bin').digest())

    def test_crc_catches_a_flipped_byte(self):
        raw = bytearray(manifest.encode(sample()))
        raw[-1] ^= 1
        with self.assertRaisesRegex(manifest.ManifestError, 'CRC'):
            manifest.decode(bytes(raw))

    def test_refuses_earlier_versions(self):
        raw = bytearray(manifest.encode(sample()))
        raw[8] = 2
        with self.assertRaisesRegex(manifest.ManifestError, 'version 3'):
            manifest.decode(bytes(raw))

    def test_boot_files_only_land_on_the_boot_entry(self):
        refused = {
            'none': [],
            'user file': [boot('bootargs.txt', manifest.BOOT_OFFSET, 1)],
            'manifest': [boot('manifest.bin', manifest.BOOT_OFFSET, 1)],
            'a folder': [boot('a/bl31.bin', manifest.BOOT_OFFSET, 1)],
            'twice': [boot('bl31.bin', manifest.BOOT_OFFSET, 1), boot('bl31.bin', manifest.BOOT_OFFSET + 4096, 1)],
            'over the head': [boot('bl31.bin', manifest.BOOT_OFFSET - 4096, 1)],
            'into the body': [boot('bl33.bin', manifest.BODY_OFFSET - 4096, 8192)],
            'overlapping': [boot('a', manifest.BOOT_OFFSET, 8192), boot('b', manifest.BOOT_OFFSET + 4096, 1)],
        }
        for what, files in refused.items():
            with self.subTest(what), self.assertRaises(manifest.ManifestError):
                manifest.encode(sample(boot=files))

    def test_parts_are_the_stream_in_one_folder(self):
        whole = manifest.BODY_OFFSET + 3 * MIB
        refused = {
            'short': [manifest.Part(f'{DIR}/IOS17.SII.001', whole - 4096)],
            'two folders': [manifest.Part(f'{DIR}/a', 80 * MIB), manifest.Part('other/b', whole - 80 * MIB)],
            'head split': [manifest.Part(f'{DIR}/a', 8 * MIB), manifest.Part(f'{DIR}/b', whole - 8 * MIB)],
            'no folder': [manifest.Part('IOS17.SII.001', whole)],
            'unaligned': [
                manifest.Part(f'{DIR}/a', 80 * MIB + 4096),
                manifest.Part(f'{DIR}/b', whole - 80 * MIB - 4096),
            ],
        }
        for what, parts in refused.items():
            with self.subTest(what), self.assertRaises(manifest.ManifestError):
                manifest.encode(sample(parts=parts))

    def test_extents_stay_below_the_minimum(self):
        with self.assertRaises(manifest.ManifestError):
            manifest.encode(sample(minimum_bytes=1 << 30))
        with self.assertRaises(manifest.ManifestError):
            manifest.encode(sample(minimum_bytes=(2 << 40)))

    def test_split_keeps_parts_under_fat32_limit(self):
        sizes = manifest.split(9 << 30)
        self.assertEqual(sum(sizes), 9 << 30)
        self.assertTrue(all(0 < s <= manifest.PART_LIMIT for s in sizes))
        self.assertEqual(len(sizes), 3)


if __name__ == '__main__':
    unittest.main()
