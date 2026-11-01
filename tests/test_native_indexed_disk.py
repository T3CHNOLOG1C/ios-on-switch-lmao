"""Bounded cluster maps: complete validation once, random data reads without FAT I/O."""
import ctypes
import tempfile
import unittest

try:
    from .test_native_fat32 import (ARG, CHAIN, IO, LIMIT, MIRROR, NOT_FOUND, OK,
                                   RANGE, READ512, FatImage, compile_library, dirent)
    from .test_native_split_disk import Part
except ImportError:
    from test_native_fat32 import (ARG, CHAIN, IO, LIMIT, MIRROR, NOT_FOUND, OK,
                                  RANGE, READ512, FatImage, compile_library, dirent)
    from test_native_split_disk import Part


U32 = ctypes.c_uint32
GUARD = 0xa5a55a5a


class NativeIndexedDiskTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.lib = compile_library(cls.temp.name)
        cls.lib.test_split_index_count.argtypes = [ctypes.POINTER(Part), U32,
                                                   ctypes.c_uint64, ctypes.POINTER(U32)]
        cls.lib.test_split_indexed_open.argtypes = [U32, ctypes.POINTER(Part), U32,
                                                    ctypes.c_uint64, ctypes.POINTER(U32), U32]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.image = FatImage()
        self.pools = []
        self.mount()

    def mount(self):
        self.callback = READ512(self.image.callback)
        self.assertEqual(self.lib.test_mount(self.callback, self.image.capacity, 0), OK)

    def pool(self, count):
        allocation = (U32 * (count + 2))(*([GUARD] * (count + 2)))
        self.pools.append(allocation)
        return allocation, ctypes.cast(ctypes.byref(allocation, 4), ctypes.POINTER(U32))

    def check_guards(self):
        for pool in self.pools:
            self.assertEqual(pool[0], GUARD)
            self.assertEqual(pool[-1], GUARD)
        self.assertFalse(self.image.out_of_bounds)

    def count(self, size, expected=OK):
        before = len(self.image.reads)
        entries = U32(GUARD)
        self.assertEqual(self.lib.test_index_count(size, ctypes.byref(entries)), expected)
        self.assertEqual(len(self.image.reads), before)
        if expected != OK:
            self.assertEqual(entries.value, 0)
        return entries.value

    def open_file(self, path=b"IOSWITCH/VERIFY.BIN", size=1200, capacity=None, expected=OK):
        if capacity is None:
            capacity = self.count(size)
        pool, pointer = self.pool(capacity)
        self.assertEqual(self.lib.test_indexed_open(path, pointer, capacity), expected)
        self.check_guards()
        return pool

    def read(self, offset, length, expected=OK, disk=None, data_only=True):
        output = ctypes.create_string_buffer(b"\xaa" * (length + 2), length + 2)
        before = len(self.image.reads)
        if disk is None:
            rc = self.lib.test_indexed_read(offset, ctypes.byref(output, 1), length)
        else:
            rc = self.lib.test_split_indexed_read(disk, offset, ctypes.byref(output, 1), length)
        self.assertEqual(rc, expected)
        self.assertEqual(output.raw[0], 0xaa)
        self.assertEqual(output.raw[-1], 0xaa)
        if data_only:
            self.assertFalse([lba for lba in self.image.reads[before:]
                              if self.image.fat_start <= lba < self.image.data_start])
        self.check_guards()
        return output.raw[1:-1]

    def split_count(self, parts, total, expected=OK):
        array = (Part * len(parts))(*parts)
        entries = U32(GUARD)
        before = len(self.image.reads)
        self.assertEqual(self.lib.test_split_index_count(array, len(parts), total,
                                                         ctypes.byref(entries)), expected)
        self.assertEqual(len(self.image.reads), before)
        if expected != OK:
            self.assertEqual(entries.value, 0)
        return entries.value

    def open_split(self, parts, total, disk=0, capacity=None, expected=OK):
        if capacity is None:
            capacity = self.split_count(parts, total)
        pool, pointer = self.pool(capacity)
        array = (Part * len(parts))(*parts)
        self.assertEqual(self.lib.test_split_indexed_open(disk, array, len(parts),
                                                          total, pointer, capacity), expected)
        self.check_guards()
        return pool

    def small_split(self):
        self.image.entries.update({5: 7, 7: 0x0fffffff, 6: 130,
                                   130: 0x0fffffff, 8: 0x0fffffff})
        self.image.directory(4, [dirent("PART0000.BIN", 5, 1024),
                                dirent("PART0001.BIN", 6, 1024),
                                dirent("PART0002.BIN", 8, 512)])
        self.mount()
        return [Part(b"IOSWITCH/OS/PART0000.BIN", 0, 1024),
                Part(b"IOSWITCH/OS/PART0001.BIN", 1024, 1024),
                Part(b"IOSWITCH/OS/PART0002.BIN", 2048, 512)]

    def test_exact_counts_zero_rounding_and_uint32_max_without_io(self):
        for size, count in [(0, 0), (1, 1), (511, 1), (512, 1), (513, 2), (1200, 3)]:
            self.assertEqual(self.count(size), count)
        self.assertEqual(self.count(0xffffffff, LIMIT), 0)
        self.image = FatImage(spc=128, clusters=131100)
        self.mount()
        for size, count in [(65535, 1), (65536, 1), (65537, 2), (0xffffffff, 65536)]:
            self.assertEqual(self.count(size), count)
        self.assertEqual(self.lib.test_index_count(0, None), ARG)

    def test_empty_file_needs_no_map_and_checks_lifetime_even_for_zero_reads(self):
        self.assertEqual(self.lib.test_indexed_open(b"IOSWITCH/EMPTY.BIN", None, 0), OK)
        self.assertEqual(self.lib.test_indexed_count(), 0)
        self.assertEqual(self.read(0, 0), b"")
        self.assertEqual(self.read(1, 0, RANGE), b"")
        self.mount()
        self.read(0, 0, ARG)

    def test_insufficient_capacity_invalidates_handle_without_scanning_or_fallback(self):
        self.open_file()
        pool = self.open_file(capacity=2, expected=LIMIT)
        self.assertEqual(list(pool), [GUARD] * 4)
        before = len(self.image.reads)
        self.assertEqual(self.read(0, 512, ARG), b"\xaa" * 512)
        self.assertEqual(len(self.image.reads), before)
        self.assertEqual(self.lib.test_indexed_open(b"IOSWITCH/VERIFY.BIN", None, 0), LIMIT)
        before = len(self.image.reads)
        self.assertEqual(self.lib.test_indexed_open(b"IOSWITCH/VERIFY.BIN", None, 3), ARG)
        self.assertEqual(len(self.image.reads), before)
        pool = self.open_file(capacity=7)
        self.assertEqual(list(pool)[1:4], [5, 7, 6])
        self.assertEqual(list(pool)[4:-1], [GUARD] * 4)

    def test_fragmented_map_and_partial_sector_reads_issue_only_data_io(self):
        pool = self.open_file()
        self.assertEqual(list(pool)[1:-1], [5, 7, 6])
        self.image.failed_lbas = set(range(self.image.fat_start, self.image.data_start))
        expected = b"".join(self.image.payload(self.image.lba(c)) for c in [5, 7, 6])[:1200]
        self.assertEqual(self.read(507, 693), expected[507:])
        self.assertEqual(self.read(0, 1200), expected)
        self.assertEqual(self.read(1200, 0), b"")

    def test_builds_map_in_same_validation_pass_as_existing_open(self):
        before = len(self.image.reads)
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), OK)
        unindexed_reads = self.image.reads[before:]
        self.mount()
        before = len(self.image.reads)
        self.open_file()
        self.assertEqual(self.image.reads[before:], unindexed_reads)

    def test_widely_fragmented_random_reads_never_consult_fat_after_open(self):
        chain = [100, 10000, 200, 20000, 300, 30000, 400, 40000, 500, 50000, 600, 60000]
        for current, following in zip(chain, chain[1:] + [0x0fffffff]):
            self.image.entries[current] = following
        size = len(chain) * 512 - 37
        self.image.directory(3, [dirent("SCATTER.BIN", chain[0], size)])
        self.mount()
        self.assertEqual(list(self.open_file(b"IOSWITCH/SCATTER.BIN", size))[1:-1], chain)
        self.image.failed_lbas = set(range(self.image.fat_start, self.image.data_start))
        expected = b"".join(self.image.payload(self.image.lba(c)) for c in chain)[:size]
        for offset, length in [(size - 513, 513), (511, 1025), (3072, 512), (0, size)]:
            self.assertEqual(self.read(offset, length), expected[offset:offset + length])

    def test_bad_short_long_cyclic_and_divergent_mirror_chains_never_publish(self):
        cases = [({5: 0}, CHAIN), ({5: 0x0ffffff7}, CHAIN),
                 ({5: 0x0ffffff0}, CHAIN), ({5: 65527}, CHAIN),
                 ({5: 0x0fffffff}, CHAIN), ({6: 8, 8: 0x0fffffff}, CHAIN),
                 ({6: 7}, CHAIN)]
        for entries, expected in cases:
            with self.subTest(entries=entries):
                self.image = FatImage()
                self.image.entries.update(entries)
                self.mount()
                self.open_file(expected=expected)
                before = len(self.image.reads)
                self.read(0, 1, ARG)
                self.assertEqual(len(self.image.reads), before)
        self.image = FatImage()
        self.image.fat_overrides[1, 5] = 6
        self.mount()
        self.open_file(expected=MIRROR)
        self.read(0, 0, ARG)

    def test_high_reserved_bits_masked_and_nonmirrored_active_fat_supported(self):
        self.image.entries[5] |= 0xf0000000
        self.mount()
        self.assertEqual(list(self.open_file())[1:-1], [5, 7, 6])
        self.image.bpb[40] = 0x81
        self.image.fat_overrides[0, 5] = 0
        self.mount()
        self.assertEqual(list(self.open_file())[1:-1], [5, 7, 6])
        self.read(0, 1200)

    def test_invalid_read_ranges_do_not_touch_output_or_call_device(self):
        self.open_file()
        before = len(self.image.reads)
        for offset, length in [(1201, 0), (1200, 1), (1199, 2), (2**64 - 1, 1)]:
            self.assertEqual(self.read(offset, length, RANGE), b"\xaa" * length)
        self.assertEqual(self.lib.test_indexed_read(0, None, 1), ARG)
        self.assertEqual(len(self.image.reads), before)

    def test_io_failure_and_invalid_map_entry_preserve_error_output_contract(self):
        self.image.failed_lbas.add(self.image.fat_start)
        self.open_file(expected=IO)
        self.read(0, 0, ARG)
        self.image.failed_lbas.clear()
        self.mount()
        pool = self.open_file()
        self.image.failed_lbas.add(self.image.lba(7))
        result = self.read(0, 1024, IO)
        self.assertEqual(result[:512], self.image.payload(self.image.lba(5)))
        self.assertEqual(result[512:], b"\xaa" * 512)
        self.image.failed_lbas.clear()
        # Caller mutation is unsupported; an invalid cluster is still bounded.
        pool[2] = self.image.clusters + 2
        result = self.read(512, 512, CHAIN)
        self.assertEqual(result, b"\xaa" * 512)

    def test_successful_and_failed_remounts_invalidate_old_indexed_handles(self):
        self.open_file()
        self.mount()
        self.read(0, 0, ARG)
        self.read(0, 1, ARG)
        self.open_file()
        self.assertEqual(self.lib.test_mount(self.callback, self.image.capacity, 4), ARG)
        self.read(0, 0, ARG)
        self.assertEqual(self.count(512, ARG), 0)

    def test_indexed_data_reads_still_obey_finite_sector_budget(self):
        size = 33000 * 512
        self.image.runs = [(100, 33000)]
        self.image.directory(3, [dirent("LARGE.BIN", 100, size)])
        self.image.synthetic_sector = lambda lba: b"x" * 512
        self.mount()
        self.open_file(b"IOSWITCH/LARGE.BIN", size)
        before = len(self.image.reads)
        result = self.read(0, size, LIMIT)
        self.assertEqual(len(self.image.reads) - before, 32768)
        self.assertEqual(result[:512], b"x" * 512)
        self.assertEqual(result[-512:], b"\xaa" * 512)

    def test_large_file_repeated_random_tail_reads_have_no_prefix_walk(self):
        size = 4_000_000_000
        self.image = FatImage(spc=128, clusters=131100)
        self.image.runs = [(100, (size + 65535) // 65536)]
        self.image.directory(3, [dirent("LARGE.BIN", 100, size)])
        self.mount()
        self.open_file(b"IOSWITCH/LARGE.BIN", size)
        for offset in [size - 4096, size - 8192, 2**31, size - 4096, 0]:
            before = len(self.image.reads)
            result = self.read(offset, 4096)
            self.assertEqual(len(self.image.reads) - before, 8)
            expected = b"".join(self.image.payload(self.image.lba(100) + offset // 512 + n)
                                for n in range(8))
            self.assertEqual(result, expected)

    def test_split_counts_reject_invalid_descriptors_and_insufficient_pool_before_io(self):
        parts = self.small_split()
        self.assertEqual(self.split_count(parts, 2560), 5)
        before = len(self.image.reads)
        pool = self.open_split(parts, 2560, capacity=4, expected=LIMIT)
        self.assertEqual(list(pool), [GUARD] * 6)
        self.assertEqual(len(self.image.reads), before)
        self.read(0, 1, ARG, disk=0)
        bad = [([], 512, ARG), (parts, 2**64 - 512, RANGE),
               ([Part(parts[0].path, 0, 513)], 1024, RANGE),
               ([Part(parts[0].path, 512, 1024)], 1024, RANGE),
               ([parts[0], Part(parts[1].path, 1536, 1024)], 2560, RANGE),
               ([parts[0], Part(parts[1].path, 512, 1024)], 2048, RANGE),
               ([Part(None, 0, 1024)], 1024, RANGE),
               ([Part(parts[0].path, i * 512, 512) for i in range(17)], 8704, ARG)]
        for descriptors, total, expected in bad:
            with self.subTest(total=total, count=len(descriptors)):
                self.split_count(descriptors, total, expected)
                self.open_split(descriptors, total, capacity=8, expected=expected)
                self.assertEqual(len(self.image.reads), before)

    def test_split_pool_count_exposes_aggregate_over_budget_without_allocating(self):
        self.image = FatImage(spc=8, clusters=1048600)
        self.mount()
        size = 0xfffffe00
        parts = [Part(b"IOSWITCH/OS/PART0000.BIN", i * size, size) for i in range(16)]
        entries = self.split_count(parts, size * 16)
        self.assertEqual(entries, 16 * 1048576)
        self.assertGreater(entries, (16 << 20) // ctypes.sizeof(U32))

    def test_split_fragmentation_unaligned_crossings_and_pool_lifetime(self):
        parts = self.small_split()
        pool = self.open_split(parts, 2560)
        self.assertEqual(list(pool)[1:-1], [5, 7, 6, 130, 8])
        del parts  # Neither descriptors nor path pointers are retained.
        expected = b"".join(self.image.payload(self.image.lba(c)) for c in [5, 7, 6, 130, 8])
        self.assertEqual(self.read(500, 1800, disk=0), expected[500:2300])
        self.assertEqual(self.read(0, 2560, disk=0), expected)
        self.assertEqual(self.read(2560, 0, disk=0), b"")

    def test_split_missing_wrong_size_and_failed_second_part_invalidate_disk(self):
        parts = self.small_split()
        self.open_split(parts, 2560)
        parts[1].path = b"IOSWITCH/OS/MISSING.BIN"
        self.open_split(parts, 2560, expected=NOT_FOUND)
        self.read(0, 0, ARG, disk=0)
        parts = self.small_split()
        self.image.directory(4, [dirent("PART0000.BIN", 5, 1024),
                                dirent("PART0001.BIN", 6, 512),
                                dirent("PART0002.BIN", 8, 512)])
        self.image.entries[6] = 0x0fffffff
        self.mount()
        self.open_split(parts, 2560, expected=RANGE)
        self.read(0, 1, ARG, disk=0)
        parts = self.small_split()
        self.image.entries[130] = 6
        self.mount()
        self.open_split(parts, 2560, expected=CHAIN)
        self.read(0, 0, ARG, disk=0)

    def test_split_second_part_read_failure_and_stale_generation(self):
        parts = self.small_split()
        self.open_split(parts, 2560)
        self.image.failed_lbas.add(self.image.lba(6))
        result = self.read(1000, 100, IO, disk=0)
        self.assertEqual(result[:24], self.image.payload(self.image.lba(7))[-24:])
        self.assertEqual(result[24:], b"\xaa" * 76)
        before = len(self.image.reads)
        for offset, length in [(2561, 0), (2560, 1), (2559, 2), (2**64 - 1, 1)]:
            self.assertEqual(self.read(offset, length, RANGE, disk=0), b"\xaa" * length)
        self.assertEqual(len(self.image.reads), before)
        self.mount()
        self.read(0, 0, ARG, disk=0)

    def test_two_large_disks_interleave_random_and_decimal_and_four_gib_crossings(self):
        self.image = FatImage(spc=128, clusters=200000)
        sizes = [4_000_000_000, 2_077_580_800, 3_303_014_400]
        starts = [100, 70000, 110000]
        for start, size in zip(starts, sizes):
            self.image.runs.append((start, (size + 65535) // 65536))
        self.image.directory(3, [dirent("OS", 4, attr=0x10),
                                dirent("SYSTEMOS", 10, attr=0x10)])
        self.image.entries[10] = 0x0fffffff
        self.image.directory(4, [dirent("PART0000.BIN", starts[0], sizes[0]),
                                dirent("PART0001.BIN", starts[1], sizes[1])])
        self.image.directory(10, [dirent("PART0000.BIN", starts[2], sizes[2])])
        self.mount()
        os_parts = [Part(b"IOSWITCH/OS/PART0000.BIN", 0, sizes[0]),
                    Part(b"IOSWITCH/OS/PART0001.BIN", sizes[0], sizes[1])]
        system_parts = [Part(b"IOSWITCH/SYSTEMOS/PART0000.BIN", 0, sizes[2])]
        self.open_split(os_parts, sum(sizes[:2]), disk=0)
        self.open_split(system_parts, sizes[2], disk=1)
        for disk, offset, length in [(0, sizes[0] - 257, 1024), (1, sizes[2] - 4096, 4096),
                                     (0, 2**32 - 257, 1024), (1, 1 << 30, 4096),
                                     (0, sum(sizes[:2]) - 513, 513), (1, 0, 4096)]:
            with self.subTest(disk=disk, offset=offset):
                expected = bytearray()
                for pos in range(offset, offset + length):
                    part = 2 if disk else int(pos >= sizes[0])
                    local = pos - (sizes[0] if part == 1 else 0)
                    lba = self.image.lba(starts[part]) + local // 512
                    expected.append(self.image.payload(lba)[local % 512])
                self.assertEqual(self.read(offset, length, disk=disk), expected)


if __name__ == "__main__":
    unittest.main()
