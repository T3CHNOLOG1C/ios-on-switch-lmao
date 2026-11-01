"""Test real FAT-backed split reads, including the 4,000,000,000-byte boundary."""
import ctypes
import tempfile
import unittest

try:
    from .test_native_fat32 import (ARG, CHAIN, IO, LIMIT, NOT_FOUND, OK, RANGE,
                                   READ512, FatImage, compile_library, dirent)
except ImportError:
    from test_native_fat32 import (ARG, CHAIN, IO, LIMIT, NOT_FOUND, OK, RANGE,
                                  READ512, FatImage, compile_library, dirent)


class Part(ctypes.Structure):
    _fields_ = [("path", ctypes.c_char_p), ("offset", ctypes.c_uint64),
                ("size", ctypes.c_uint32)]


class NativeSplitDiskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.lib = compile_library(cls.temp.name)
        cls.lib.test_split_open.argtypes = [ctypes.POINTER(Part), ctypes.c_uint32,
                                           ctypes.c_uint64]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.image = FatImage()
        self.image.entries.update({5: 7, 7: 0x0fffffff, 6: 130,
                                   130: 0x0fffffff, 8: 0x0fffffff})
        self.image.directory(4, [dirent("PART0000.BIN", 5, 1024),
                                dirent("PART0001.BIN", 6, 1024),
                                dirent("PART0002.BIN", 8, 512)])
        self.parts = [Part(b"/IOSWITCH/OS/PART0000.BIN", 0, 1024),
                      Part(b"/IOSWITCH/OS/PART0001.BIN", 1024, 1024),
                      Part(b"/IOSWITCH/OS/PART0002.BIN", 2048, 512)]
        self.total = 2560
        self.mount()

    def mount(self):
        self.callback = READ512(self.image.callback)
        self.assertEqual(self.lib.test_mount(self.callback, self.image.capacity, 0), OK)

    def open(self, expected=OK):
        descriptors = (Part * len(self.parts))(*self.parts)
        self.assertEqual(self.lib.test_split_open(descriptors, len(self.parts), self.total), expected)

    def read(self, offset, count, expected=OK):
        output = ctypes.create_string_buffer(b"\xaa" * (count + 2), count + 2)
        self.assertEqual(self.lib.test_split_read(offset, ctypes.byref(output, 1), count), expected)
        self.assertEqual(output.raw[0], 0xaa)
        self.assertEqual(output.raw[-1], 0xaa)
        self.assertFalse(self.image.out_of_bounds)
        return output.raw[1:-1]

    def test_unaligned_read_across_three_fragmented_parts(self):
        self.open()
        expected = b"".join(self.image.payload(self.image.lba(c)) for c in [5, 7, 6, 130, 8])
        self.assertEqual(self.read(500, 1800), expected[500:2300])
        self.assertEqual(self.read(0, self.total), expected)
        self.assertEqual(self.read(1024, 512), expected[1024:1536])
        self.assertEqual(self.read(self.total, 0), b"")

    def test_mapping_rejects_gaps_overlaps_unaligned_sizes_totals_and_excess_parts(self):
        original = [(p.path, p.offset, p.size) for p in self.parts]
        cases = [([(original[0][0], 0, 0)], 0, ARG),
                 ([(original[0][0], 0, 513)], 513, ARG),
                 ([(original[0][0], 0, 513)], 1024, RANGE),
                 ([(original[0][0], 512, 1024)], 1024, RANGE),
                 ([original[0], (original[1][0], 1536, 1024)], 2560, RANGE),
                 ([original[0], (original[1][0], 512, 1024)], 2048, RANGE),
                 (original, self.total + 512, RANGE),
                 (original, self.total - 512, RANGE),
                 (original, 2**64 - 512, RANGE),
                 ([(None, 0, 1024)], 1024, RANGE),
                 ([(original[0][0], i * 1024, 1024) for i in range(17)], 17 * 1024, ARG),
                 ([], 512, ARG)]
        for parts, total, expected in cases:
            with self.subTest(parts=parts, total=total):
                before = len(self.image.reads)
                self.parts = [Part(*p) for p in parts]
                self.total = total
                self.open(expected)
                self.assertEqual(len(self.image.reads), before)

    def test_missing_wrong_size_and_bad_chain_parts_invalidate_disk(self):
        self.open()
        self.parts[1].path = b"IOSWITCH/OS/MISSING.BIN"
        self.open(NOT_FOUND)
        self.read(0, 1, ARG)
        self.parts[1].path = b"IOSWITCH/OS/PART0001.BIN"
        self.parts[1].size = 512
        self.parts[2].offset = 1536
        self.total = 2048
        self.open(RANGE)
        self.parts[1].size = 1024
        self.parts[2].offset = 2048
        self.total = 2560
        self.image.entries[130] = 6
        self.mount()
        self.open(CHAIN)

    def test_range_checks_no_reads_or_output_changes(self):
        self.open()
        before = len(self.image.reads)
        for offset, length in [(2561, 0), (2560, 1), (2559, 2), (2**64 - 1, 1)]:
            self.assertEqual(self.read(offset, length, RANGE), b"\xaa" * length)
        self.assertEqual(len(self.image.reads), before)

    def test_part_two_io_failure_propagates_with_valid_prefix_only(self):
        self.open()
        self.image.failed_lbas.add(self.image.lba(6))
        result = self.read(1000, 100, IO)
        self.assertEqual(result[:24], self.image.payload(self.image.lba(7))[-24:])
        self.assertEqual(result[24:], b"\xaa" * 76)

    def test_remount_invalidates_split_handles_even_for_empty_reads(self):
        self.open()
        self.mount()
        self.read(0, 0, ARG)
        self.read(0, 1, ARG)

    def test_real_part_sizes_cross_decimal_boundary_and_four_gib_offsets(self):
        self.image = FatImage(spc=128, clusters=131100)
        sizes = [4_000_000_000, 2_077_580_800]
        starts = [100, 70000]
        for first, size in zip(starts, sizes):
            self.image.runs.append((first, (size + 65535) // 65536))
        self.image.directory(4, [dirent("PART0000.BIN", starts[0], sizes[0]),
                                dirent("PART0001.BIN", starts[1], sizes[1])])
        self.parts = [Part(b"IOSWITCH/OS/PART0000.BIN", 0, sizes[0]),
                      Part(b"IOSWITCH/OS/PART0001.BIN", sizes[0], sizes[1])]
        self.total = sum(sizes)
        self.mount()
        self.open()
        for offset, length in [(sizes[0] - 257, 1024), (2**32 - 257, 1024),
                               (self.total - 513, 513)]:
            with self.subTest(offset=offset):
                expected = bytearray()
                for pos in range(offset, offset + length):
                    index = int(pos >= sizes[0])
                    local = pos - (sizes[0] if index else 0)
                    lba = self.image.lba(starts[index]) + local // 512
                    expected.append(self.image.payload(lba)[local % 512])
                self.assertEqual(self.read(offset, length), expected)


if __name__ == "__main__":
    unittest.main()
