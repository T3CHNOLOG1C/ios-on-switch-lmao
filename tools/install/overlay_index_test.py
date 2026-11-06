"""The kernel's redirect index for iOS 27's translated System files: the byte layout r20's reader takes."""

import struct
import unittest

from tools.install import overlay_index


class OverlayIndexTest(unittest.TestCase):
    def test_layout(self):
        raw = overlay_index.encode(['/usr/libexec/b', '/System/Library/a', '/usr/libexec/a'])
        magic, version, count, size = struct.unpack_from('<4sIII', raw)
        self.assertEqual((magic, version, count), (b'SWOV', 1, 3))
        offsets = struct.unpack_from('<3I', raw, 16)
        strings = raw[28:]
        self.assertEqual(size, len(strings))
        self.assertEqual(strings, b'/System/Library/a\0/usr/libexec/a\0/usr/libexec/b\0')
        self.assertEqual(offsets, (0, 18, 33))

    def test_round_trip(self):
        paths = ['/a', '/b/c', '/b/c d', '/z']
        self.assertEqual(overlay_index.decode(overlay_index.encode(paths)), paths)

    def test_refuses_paths_the_kernel_would_not_take(self):
        for bad in ['relative', '/trailing/', '/a//b', '/a/./b', '/a/../b', '/' + 'x' * 1024]:
            with self.subTest(bad), self.assertRaises(overlay_index.OverlayIndexError):
                overlay_index.encode([bad])
        with self.assertRaises(overlay_index.OverlayIndexError):
            overlay_index.encode(['/a', '/a'])

    def test_decode_refuses_unsorted_or_loose_tables(self):
        raw = bytearray(overlay_index.encode(['/a', '/b']))
        raw[24:28] = struct.pack('<I', 0)
        with self.assertRaises(overlay_index.OverlayIndexError):
            overlay_index.decode(bytes(raw))
        swapped = struct.pack('<4sIII', b'SWOV', 1, 2, 6) + struct.pack('<2I', 0, 3) + b'/b\0/a\0'
        with self.assertRaises(overlay_index.OverlayIndexError):
            overlay_index.decode(swapped)


if __name__ == '__main__':
    unittest.main()
