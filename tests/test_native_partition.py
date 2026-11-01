"""GPT qualification against independent sparse images and Python CRC32."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest
import uuid
import zlib

ROOT = Path(__file__).resolve().parents[1]
OK, ARG, IO, MBR, HEADER, HEADER_CRC, GEOMETRY, ENTRIES, ARRAY_CRC, MISMATCH, NOT_FOUND, OVERLAP, DUPLICATE = range(13)
READ512 = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64,
                          ctypes.POINTER(ctypes.c_uint8))
TARGET = uuid.UUID("2FBBA66A-A356-00C1-CD9F-1DCBB51C2CA9").bytes_le
BASIC = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7").bytes_le
ESP = uuid.UUID("C12A7328-F81F-11D2-BA4B-00A0C93EC93B").bytes_le


def header_crc(data):
    struct.pack_into("<I", data, 16, 0)
    struct.pack_into("<I", data, 16, zlib.crc32(data[:92]))


class GptImage:
    def __init__(self, entry_size=128, entries=128, capacity=2_000_000_000, target_index=2):
        self.capacity = capacity
        self.target_index = target_index
        self.entry_size, self.entries = entry_size, entries
        self.length = entry_size * entries
        self.blocks = (self.length + 511) // 512
        reserved_blocks = max(32, self.blocks)
        self.array_lbas = [2, capacity - 1 - reserved_blocks]
        self.first_usable, self.last_usable = 2 + reserved_blocks, self.array_lbas[1] - 1
        self.arrays = [bytearray(self.length), bytearray(self.length)]
        self.mbr = bytearray(512)
        self.mbr[450] = 0xee
        struct.pack_into("<II", self.mbr, 454, 1, min(capacity - 1, 2**32 - 1))
        self.mbr[510:] = b"\x55\xaa"
        self.headers = []
        disk_guid = uuid.UUID("45d021ce-459b-41b1-ac7f-a84269d4fb7a").bytes_le
        for i in range(2):
            data = bytearray(512)
            data[:8] = b"EFI PART"
            struct.pack_into("<IIIIQQQQ", data, 8, 0x10000, 92, 0, 0,
                             capacity - 1 if i else 1, 1 if i else capacity - 1,
                             self.first_usable, self.last_usable)
            data[56:72] = disk_guid
            struct.pack_into("<QIII", data, 72, self.array_lbas[i], entries, entry_size, 0)
            self.headers.append(data)
        self.set_partition(0, ESP, uuid.UUID(int=100).bytes_le, 1000, 2000)
        self.set_partition(target_index, BASIC, TARGET, 32768, 32768 + 886121168896 // 512 - 1)
        self.finish_arrays()
        self.reads, self.out_of_bounds = [], []
        self.failed = set()
        self.padding = [0, 0]

    def set_partition(self, index, type_guid, unique_guid, first, last):
        entry = bytearray(self.entry_size)
        entry[:16], entry[16:32] = type_guid, unique_guid
        struct.pack_into("<QQQ", entry, 32, first, last, 0)
        entry[56:64] = "TEST".encode("utf-16le")
        for array in self.arrays:
            array[index * self.entry_size:(index + 1) * self.entry_size] = entry

    def finish_arrays(self):
        for i in range(2):
            struct.pack_into("<I", self.headers[i], 88, zlib.crc32(self.arrays[i]))
            header_crc(self.headers[i])

    def field(self, index, offset, fmt, value):
        struct.pack_into("<" + fmt, self.headers[index], offset, value)
        header_crc(self.headers[index])

    def sector(self, lba):
        if lba == 0:
            return self.mbr
        if lba == 1:
            return self.headers[0]
        if lba == self.capacity - 1:
            return self.headers[1]
        for i in range(2):
            if self.array_lbas[i] <= lba < self.array_lbas[i] + self.blocks:
                start = (lba - self.array_lbas[i]) * 512
                return self.arrays[i][start:start + 512].ljust(512, bytes([self.padding[i]]))
        raise AssertionError(f"Unexpected GPT read at {lba}")

    def callback(self, context, lba, output):
        del context
        self.reads.append(lba)
        if lba >= self.capacity:
            self.out_of_bounds.append(lba)
            return 1
        if lba in self.failed:
            ctypes.memset(output, 0xee, 512)
            return 1
        ctypes.memmove(output, bytes(self.sector(lba)), 512)
        return 0


class Slice(ctypes.Structure):
    _fields_ = [("first", ctypes.c_uint64), ("count", ctypes.c_uint64),
                ("index", ctypes.c_uint32), ("guid", ctypes.c_uint8 * 16)]


def compile_library(directory, extra_flags=()):
    compiler = shutil.which("clang")
    if not compiler:
        raise unittest.SkipTest("C compiler unavailable")
    source = Path(directory) / "wrapper.c"
    source.write_text('''
#include "partition.h"
static struct iosw_gpt_workspace workspace;
int find_partition(iosw_fat_read512 cb,uint64_t capacity,const uint8_t *guid,
                   struct iosw_gpt_slice *output) {
    return iosw_gpt_find(cb,0,capacity,guid,&workspace,output);
}
unsigned workspace_size(void) {return sizeof workspace;}
''')
    library = Path(directory) / "partition.so"
    subprocess.run([compiler, "-shared", "-fPIC", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    *extra_flags, "-I", str(ROOT / "native/storage"), str(source),
                    str(ROOT / "native/storage/partition.c"), "-o", str(library)], check=True,
                   capture_output=True, text=True)
    lib = ctypes.CDLL(str(library))
    lib.find_partition.argtypes = [READ512, ctypes.c_uint64, ctypes.c_void_p, ctypes.POINTER(Slice)]
    return lib


class NativePartitionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.lib = compile_library(cls.temp.name)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.image = GptImage()

    def find(self, expected=OK, guid=TARGET, capacity=None):
        callback = READ512(self.image.callback)
        output = Slice()
        ctypes.memset(ctypes.byref(output), 0xaa, ctypes.sizeof(output))
        before = bytes(output)
        self.assertEqual(self.lib.find_partition(callback,
                         self.image.capacity if capacity is None else capacity,
                         guid, ctypes.byref(output)), expected)
        if expected:
            self.assertEqual(bytes(output), before)
        else:
            self.assertEqual((output.first, output.count, output.index),
                             (32768, 886121168896 // 512, self.image.target_index))
            self.assertEqual(bytes(output.guid), TARGET)
        self.assertFalse(self.image.out_of_bounds)
        return output

    def test_valid_primary_backup_exact_guid_and_hybrid_mbr(self):
        self.find()
        self.assertEqual(len(self.image.reads), 67)
        self.assertEqual(self.image.reads[:3], [0, 1, self.image.capacity - 1])
        self.assertLess(self.lib.workspace_size(), 6000)
        self.image.mbr[466] = 0x0c
        self.image.mbr[462] = 0x80  # A hybrid entry is not interpreted by GPT selection.
        self.find()

    def test_larger_entries_and_partial_last_sector_crc_coverage(self):
        for entry_size, count in [(256, 64), (256, 65), (512, 32), (512, 128)]:
            with self.subTest(entry_size=entry_size, count=count):
                self.image = GptImage(entry_size, count)
                self.image.padding = [0x11, 0x99]
                self.find()
                self.assertEqual(len(self.image.reads), 3 + 2 * self.image.blocks)
                self.assertLessEqual(len(self.image.reads), 259)

    def test_all_bounded_counts_and_sizes_with_exact_declared_crc_bytes(self):
        for entry_size in (128, 256, 512):
            for count in range(1, 129):
                with self.subTest(entry_size=entry_size, count=count):
                    self.image = GptImage(entry_size, count, target_index=0)
                    # These differing trailing bytes are not declared entries
                    # and must not be included in CRCs or copy comparisons.
                    self.image.padding = [0x11, 0x99]
                    self.find()
                    self.assertEqual(len(self.image.reads), 3 + 2 * self.image.blocks)
                    self.assertLessEqual(len(self.image.reads), 259)

    def test_hekate_created_entry_count_and_full_reserved_table(self):
        # Pinned Hekate formatter declares gpt_idx entries, CRCs only those
        # bytes, writes 128 entries, and places backup array at capacity-33.
        # Eleven is a synthetic example, not an observed card-header count.
        self.image = GptImage(entries=11, capacity=0x7731c000, target_index=0)
        self.image.last_usable = self.image.capacity - 0x800 - 1
        for index in range(2):
            self.image.field(index, 48, "Q", self.image.last_usable)
        self.assertEqual(self.image.array_lbas, [2, self.image.capacity - 33])
        self.find()
        self.assertEqual(len(self.image.reads), 9)

    def test_short_table_retains_reservation_integrity_and_entry_bounds(self):
        cases = [
            ("short_primary_reservation", lambda im: im.field(0, 72, "Q", 3), GEOMETRY),
            ("short_backup_reservation", lambda im: im.field(1, 72, "Q", im.capacity - 32), GEOMETRY),
            ("bad_header_crc", lambda im: im.headers[0].__setitem__(16, im.headers[0][16] ^ 1), HEADER_CRC),
            ("different_array_bytes", lambda im: im.arrays[1].__setitem__(56, im.arrays[1][56] ^ 1), MISMATCH),
            ("zero_count", lambda im: im.field(0, 80, "I", 0), ENTRIES),
            ("oversized_count", lambda im: im.field(0, 80, "I", 129), ENTRIES),
            ("unsupported_entry_size", lambda im: im.field(0, 84, "I", 136), ENTRIES),
            ("header_count_mismatch", lambda im: im.field(1, 80, "I", 12), MISMATCH),
            ("used_entry_out_of_bounds", lambda im: im.set_partition(0, BASIC, TARGET, 1, 100), ENTRIES),
        ]
        for name, mutate, expected in cases:
            with self.subTest(name=name):
                self.image = GptImage(entries=11, target_index=0)
                mutate(self.image)
                self.find(expected)
        self.image = GptImage(entries=11, target_index=0)
        for array in self.image.arrays:
            array[56] ^= 1
        self.find(ARRAY_CRC)  # Agreeing copies cannot bypass their CRCs.

    def test_zero_unknown_guid_and_wrong_target_type(self):
        self.find(ARG, guid=bytes(16))
        self.find(ARG, guid=None)
        self.find(NOT_FOUND, guid=uuid.UUID(int=444).bytes_le)
        self.image.set_partition(2, ESP, TARGET, 32768, 32768 + 886121168896 // 512 - 1)
        self.image.finish_arrays()
        self.find(NOT_FOUND)

    def test_protective_signature_and_type_required(self):
        self.image.mbr[510] = 0
        self.find(MBR)
        self.image.mbr[510] = 0x55
        self.image.mbr[450] = 0x0c
        self.find(MBR)
        self.assertEqual(self.image.reads, [0, 0])

    def test_header_crc_and_format_both_copies(self):
        for index in range(2):
            for offset, value, expected in [(0, 0, HEADER), (8, 2, HEADER), (12, 91, HEADER),
                                            (20, 1, HEADER), (92, 1, HEADER), (16, 0, HEADER_CRC)]:
                with self.subTest(index=index, offset=offset):
                    self.image = GptImage()
                    self.image.headers[index][offset] = value
                    self.find(expected)

    def test_header_geometry_and_array_size_limits(self):
        cases = [(24, "Q", 123, GEOMETRY), (32, "Q", 123, GEOMETRY),
                 (40, "Q", 33, GEOMETRY), (40, "Q", 2**64 - 1, GEOMETRY),
                 (48, "Q", 2**64 - 1, GEOMETRY), (48, "Q", 32, GEOMETRY),
                 (80, "I", 0, ENTRIES), (80, "I", 129, ENTRIES),
                 (80, "I", 2**32 - 1, ENTRIES), (80, "I", 127, MISMATCH),
                 (84, "I", 0, ENTRIES), (84, "I", 136, ENTRIES),
                 (84, "I", 1024, ENTRIES), (84, "I", 2**32 - 1, ENTRIES)]
        for index in range(2):
            for offset, fmt, value, expected in cases:
                with self.subTest(index=index, offset=offset, value=value):
                    self.image = GptImage()
                    self.image.field(index, offset, fmt, value)
                    self.find(expected)
        for index, value in [(0, 0), (0, 1), (0, 3), (0, 34),
                             (0, 2**64 - 1), (1, self.image.last_usable),
                             (1, self.image.capacity - 1), (1, self.image.capacity - 2)]:
            with self.subTest(index=index, entry_lba=value):
                self.image = GptImage()
                self.image.field(index, 72, "Q", value)
                self.find(GEOMETRY)
        self.find(ARG, capacity=67)

    def test_header_pair_disagreement_and_zero_disk_guid(self):
        for offset, fmt, value in [(40, "Q", 35), (48, "Q", self.image.last_usable - 1),
                                   (56, "I", 123), (88, "I", 123)]:
            with self.subTest(offset=offset):
                self.image = GptImage()
                self.image.field(1, offset, fmt, value)
                self.find(MISMATCH)
        self.image = GptImage()
        self.image.headers[0][56:72] = bytes(16)
        header_crc(self.image.headers[0])
        self.find(GEOMETRY)

    def test_array_byte_disagreement_and_independent_array_crc_validation(self):
        self.image.arrays[1][56] ^= 1
        self.find(MISMATCH)
        self.image = GptImage()
        for array in self.image.arrays:
            array[56] ^= 1  # Both copies agree, but stored CRC is stale.
        self.find(ARRAY_CRC)
        self.assertEqual(len(self.image.reads), 67)

    def test_every_used_entry_bounds_guid_uniqueness_and_no_overlap(self):
        for first, last, expected in [(0, 100, ENTRIES), (1000, 999, ENTRIES),
                                      (2000, 2001, OVERLAP), (32768, 32768, OVERLAP),
                                      (self.image.last_usable, self.image.last_usable + 1, ENTRIES),
                                      (2**64 - 1, 2**64 - 1, ENTRIES)]:
            with self.subTest(first=first, last=last):
                self.image = GptImage()
                self.image.set_partition(127, ESP, uuid.UUID(int=200).bytes_le, first, last)
                self.image.finish_arrays()
                self.find(expected)
        self.image = GptImage()
        self.image.set_partition(127, ESP, TARGET, 3000, 4000)
        self.image.finish_arrays()
        self.find(DUPLICATE)
        self.image = GptImage()
        self.image.set_partition(127, ESP, bytes(16), 3000, 4000)
        self.image.finish_arrays()
        self.find(ENTRIES)

    def test_partition_adjacency_is_allowed_and_extended_reserved_bytes_are_zero(self):
        self.image.set_partition(127, ESP, uuid.UUID(int=200).bytes_le, 2001, 32767)
        self.image.finish_arrays()
        self.find()
        self.image = GptImage(256, 64)
        for array in self.image.arrays:
            array[128] = 1
        self.image.finish_arrays()
        self.find(ENTRIES)

    def test_callback_failure_at_every_metadata_sector_leaves_output_uncommitted(self):
        lbas = [0, 1, self.image.capacity - 1]
        for base in self.image.array_lbas:
            lbas.extend(range(base, base + self.image.blocks))
        for lba in lbas:
            with self.subTest(lba=lba):
                self.image.failed = {lba}
                self.find(IO)
                self.assertEqual(self.image.reads[-1], lba)

    def test_capacity_and_backup_lba_are_64bit(self):
        self.image = GptImage(capacity=2**40 + 12345)
        self.find()
        self.assertIn(self.image.capacity - 1, self.image.reads)
        self.image = GptImage(capacity=2**64 - 1)
        self.find()
        self.assertIn(2**64 - 2, self.image.reads)


if __name__ == "__main__":
    unittest.main()
