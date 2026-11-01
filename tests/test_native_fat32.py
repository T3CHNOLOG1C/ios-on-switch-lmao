"""Sparse FAT images exercise the C reader without a device or a mounted disk."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OK, ARG, IO, FORMAT, RANGE, NOT_FOUND, NOT_FILE, CHAIN, LIMIT, MIRROR = range(10)
READ512 = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64,
                          ctypes.POINTER(ctypes.c_uint8))


def put16(buf, offset, value):
    struct.pack_into("<H", buf, offset, value)


def put32(buf, offset, value):
    struct.pack_into("<I", buf, offset, value)


def dirent(name, first, size=0, attr=0x20):
    result = bytearray(32)
    base, dot, ext = name.partition(".")
    result[:11] = (base.ljust(8) + ext.ljust(3)).encode("ascii")
    result[11] = attr
    put16(result, 20, first >> 16)
    put16(result, 26, first & 65535)
    put32(result, 28, size)
    return result


class FatImage:
    """Independent on-disk fixture; sparse sectors and computed large FAT runs."""

    def __init__(self, spc=1, clusters=65525):
        self.first = 2048
        self.spc = spc
        self.clusters = clusters
        self.reserved = 32
        self.fats = 2
        self.fat_sectors = (clusters + 2 + 127) // 128
        self.total = self.reserved + self.fats * self.fat_sectors + clusters * spc
        self.capacity = self.first + self.total + 4096
        self.mbr = bytearray(512)
        self.mbr[446 + 4] = 0x0c
        put32(self.mbr, 446 + 8, self.first)
        put32(self.mbr, 446 + 12, self.total)
        self.mbr[510:] = b"\x55\xaa"
        self.bpb = bytearray(512)
        self.bpb[:3] = b"\xeb\x58\x90"
        put16(self.bpb, 11, 512)
        self.bpb[13] = spc
        put16(self.bpb, 14, self.reserved)
        self.bpb[16] = self.fats
        self.bpb[21] = 0xf8
        put32(self.bpb, 28, self.first)
        put32(self.bpb, 32, self.total)
        put32(self.bpb, 36, self.fat_sectors)
        put32(self.bpb, 44, 2)
        self.bpb[510:] = b"\x55\xaa"
        self.entries = {0: 0x0ffffff8, 1: 0x0fffffff, 2: 0x0fffffff,
                        3: 0x0fffffff, 4: 0x0fffffff,
                        5: 7, 7: 6, 6: 0x0fffffff}
        self.fat_overrides = {}
        self.sectors = {}
        self.runs = []
        self.reads = []
        self.failed_lbas = set()
        self.out_of_bounds = []
        self.synthetic_sector = None
        self.directory(2, [dirent("IOSWITCH", 3, attr=0x10)])
        self.directory(3, [dirent("VERIFY.BIN", 5, 1200),
                           dirent("OS", 4, attr=0x10),
                           dirent("EMPTY.BIN", 0)])
        self.directory(4, [dirent("PART0000.BIN", 5, 1200)])

    @property
    def fat_start(self):
        return self.first + self.reserved

    @property
    def data_start(self):
        return self.fat_start + self.fats * self.fat_sectors

    def lba(self, cluster, sector=0):
        return self.data_start + (cluster - 2) * self.spc + sector

    def directory(self, cluster, entries):
        data = b"".join(entries)
        assert len(data) <= 512
        self.sectors[self.lba(cluster)] = data.ljust(512, b"\0")

    def fat(self, index, cluster):
        if (index, cluster) in self.fat_overrides:
            return self.fat_overrides[index, cluster]
        if cluster in self.entries:
            return self.entries[cluster]
        for first, count in self.runs:
            if first <= cluster < first + count:
                return cluster + 1 if cluster + 1 < first + count else 0x0fffffff
        return 0

    @staticmethod
    def payload(lba):
        # Incorporate high LBA bits to expose 32-bit byte-offset truncation.
        return bytes((lba * 17 + (lba >> 16) + i * 13) & 255 for i in range(512))

    def sector(self, lba):
        if lba == 0:
            return self.mbr
        if lba == self.first:
            return self.bpb
        if self.fat_start <= lba < self.data_start:
            index, local = divmod(lba - self.fat_start, self.fat_sectors)
            return struct.pack("<128I", *(self.fat(index, local * 128 + i)
                                          for i in range(128)))
        if lba in self.sectors:
            return self.sectors[lba]
        if self.synthetic_sector is not None:
            value = self.synthetic_sector(lba)
            if value is not None:
                return value
        return self.payload(lba)

    def callback(self, context, lba, output):
        del context
        self.reads.append(lba)
        if lba >= self.capacity or (lba != 0 and not self.first <= lba < self.first + self.total):
            self.out_of_bounds.append(lba)
            return 1
        if lba in self.failed_lbas:
            # Failure is allowed to scribble into the transport destination.
            ctypes.memset(output, 0xee, 512)
            return 1
        data = self.sector(lba)
        ctypes.memmove(output, bytes(data), 512)
        return 0


def compile_library(directory, extra_flags=()):
    compiler = shutil.which("clang")
    if not compiler:
        raise unittest.SkipTest("C compiler unavailable")
    wrapper = Path(directory) / "wrapper.c"
    wrapper.write_text(r'''
#include "fat32.h"
#include "split_disk.h"
static struct iosw_fat_volume volume;
static struct iosw_fat_file file;
static struct iosw_split_disk disk;
static struct iosw_fat_indexed_file indexed_file;
static struct iosw_split_indexed_disk indexed_disks[2];
int test_mount(iosw_fat_read512 cb, uint64_t cap, uint32_t partition) {
    return iosw_fat_mount(&volume,cb,0,cap,partition);
}
int test_mount_slice(iosw_fat_read512 cb,uint64_t cap,uint64_t first,uint64_t count) {
    return iosw_fat_mount_slice(&volume,cb,0,cap,first,count);
}
int test_open(const char *p) {return iosw_fat_open(&volume,p,&file);}
int test_read(uint64_t offset, void *out, uint32_t n) {
    return iosw_fat_read(&volume,&file,offset,out,n);
}
uint32_t test_size(void) {return file.size;}
uint32_t test_clusters(void) {return volume.cluster_count;}
int test_split_open(const struct iosw_split_part *parts,uint32_t n,uint64_t size) {
    return iosw_split_open(&disk,&volume,parts,n,size);
}
int test_split_read(uint64_t offset,void *out,uint32_t n) {
    return iosw_split_read(&disk,offset,out,n);
}
int test_index_count(uint32_t size,uint32_t *entries) {
    return iosw_fat_index_count(&volume,size,entries);
}
int test_indexed_open(const char *path,uint32_t *map,uint32_t capacity) {
    return iosw_fat_open_indexed(&volume,path,&indexed_file,map,capacity);
}
int test_indexed_read(uint64_t offset,void *out,uint32_t n) {
    return iosw_fat_read_indexed(&volume,&indexed_file,offset,out,n);
}
uint32_t test_indexed_count(void) {return indexed_file.count;}
int test_split_index_count(const struct iosw_split_part *parts,uint32_t count,
                           uint64_t total,uint32_t *entries) {
    return iosw_split_index_count(&volume,parts,count,total,entries);
}
int test_split_indexed_open(uint32_t which,const struct iosw_split_part *parts,
                            uint32_t count,uint64_t total,uint32_t *map,
                            uint32_t capacity) {
    if(which>=2)return IOSW_FAT_ARGUMENT;
    return iosw_split_open_indexed(&indexed_disks[which],&volume,parts,count,
                                   total,map,capacity);
}
int test_split_indexed_read(uint32_t which,uint64_t offset,void *out,uint32_t n) {
    if(which>=2)return IOSW_FAT_ARGUMENT;
    return iosw_split_read_indexed(&indexed_disks[which],offset,out,n);
}
''')
    library = Path(directory) / "reader.so"
    subprocess.run([compiler, "-std=c11", "-shared", "-fPIC", "-O2", *extra_flags, "-Wall",
                    "-Wextra", "-Werror", "-I", str(ROOT / "native/storage"),
                    str(wrapper), str(ROOT / "native/storage/fat32.c"),
                    str(ROOT / "native/storage/split_disk.c"), "-o", str(library)],
                   check=True, capture_output=True, text=True)
    result = ctypes.CDLL(str(library))
    result.test_mount.argtypes = [READ512, ctypes.c_uint64, ctypes.c_uint32]
    result.test_mount_slice.argtypes = [READ512, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64]
    result.test_open.argtypes = [ctypes.c_char_p]
    result.test_read.argtypes = [ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint32]
    result.test_size.restype = ctypes.c_uint32
    result.test_clusters.restype = ctypes.c_uint32
    result.test_split_read.argtypes = result.test_read.argtypes
    result.test_index_count.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    result.test_indexed_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32]
    result.test_indexed_read.argtypes = result.test_read.argtypes
    result.test_indexed_count.restype = ctypes.c_uint32
    result.test_split_indexed_read.argtypes = [ctypes.c_uint32, *result.test_read.argtypes]
    return result


class NativeFat32Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.lib = compile_library(cls.temp.name)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.image = FatImage()
        self.callback = READ512(self.image.callback)

    def mount(self, expected=OK, capacity=None, partition=0):
        self.assertEqual(self.lib.test_mount(self.callback,
                         self.image.capacity if capacity is None else capacity,
                         partition), expected)
        self.assertFalse(self.image.out_of_bounds)

    def read(self, offset, count, expected=OK):
        buf = ctypes.create_string_buffer(b"\xaa" * (count + 2), count + 2)
        self.assertEqual(self.lib.test_read(offset, ctypes.byref(buf, 1), count), expected)
        self.assertEqual(buf.raw[0], 0xaa)
        self.assertEqual(buf.raw[-1], 0xaa)
        self.assertFalse(self.image.out_of_bounds)
        return buf.raw[1:-1]

    def test_fragmented_partial_sector_exact_file_and_casefolded_path(self):
        self.mount()
        self.assertEqual(self.lib.test_clusters(), 65525)
        self.assertEqual(self.lib.test_open(b"/ioswitch/verify.bin"), OK)
        expected = b"".join(self.image.payload(self.image.lba(c)) for c in [5, 7, 6])[:1200]
        self.assertEqual(self.read(499, 600), expected[499:1099])
        self.assertEqual(self.read(0, 1200), expected)
        self.assertEqual(self.read(1200, 0), b"")
        self.assertEqual(self.lib.test_size(), 1200)

    def test_invalid_reads_do_not_touch_output_or_read_device(self):
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), OK)
        before = len(self.image.reads)
        for offset, n in [(1201, 0), (1200, 1), (1199, 2), (2**64 - 1, 1)]:
            self.assertEqual(self.read(offset, n, RANGE), b"\xaa" * n)
        self.assertEqual(len(self.image.reads), before)

    def test_empty_file_and_non_file(self):
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/EMPTY.BIN"), OK)
        self.read(0, 0)
        self.read(0, 1, RANGE)
        self.assertEqual(self.lib.test_open(b"IOSWITCH/OS"), NOT_FILE)
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN/X"), NOT_FILE)

    def test_paths_are_bounded_and_83_only(self):
        self.mount()
        invalid = [b"", b"/", b"//IOSWITCH", b"IOSWITCH/", b"IOSWITCH//VERIFY.BIN",
                   b"../VERIFY.BIN", b"IOSWITCH/./VERIFY.BIN", b"IOSWITCH/VERIFY.",
                   b"IOSWITCH/PART-0000.BIN", b"IOSWITCH/VERI*.BIN", b"A" * 105]
        before = len(self.image.reads)
        for path in invalid:
            with self.subTest(path=path):
                self.assertEqual(self.lib.test_open(path), ARG)
        self.assertEqual(self.lib.test_open(b"A/A/A/A/A/A/A/A/A"), LIMIT)
        self.assertEqual(len(self.image.reads), before)

    def test_mbr_capacity_and_signature_checks(self):
        self.mount(ARG, partition=4)
        self.mount(ARG, capacity=0)
        self.mount(RANGE, capacity=self.image.first + self.image.total - 1)
        for offset, value, expected in [(510, 0, FORMAT), (450, 0x83, FORMAT), (446, 1, FORMAT)]:
            original = self.image.mbr[offset]
            self.image.mbr[offset] = value
            self.mount(expected)
            self.image.mbr[offset] = original
        put32(self.image.mbr, 454, 0)
        self.mount(RANGE)

    def test_trusted_slice_and_64bit_partition_addressing(self):
        self.image.first = 2**32 + 12345
        self.image.capacity = self.image.first + self.image.total + 4096
        # Rebuild directory locations after moving the partition above 2TiB.
        self.image.sectors = {}
        self.image.directory(2, [dirent("IOSWITCH", 3, attr=0x10)])
        self.image.directory(3, [dirent("VERIFY.BIN", 5, 1200)])
        self.assertEqual(self.lib.test_mount_slice(self.callback, self.image.capacity,
                                                  self.image.first, self.image.total), OK)
        self.assertEqual(self.image.reads, [self.image.first])  # MBR not consulted.
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), OK)
        self.assertEqual(self.read(0, 512), self.image.payload(self.image.lba(5)))
        for first, count in [(0, 10), (self.image.first, 0),
                             (self.image.capacity, 1), (2**64 - 1, 2),
                             (self.image.first, self.image.total - 1),
                             (self.image.first, 2**64 - 1)]:
            with self.subTest(first=first, count=count):
                self.assertEqual(self.lib.test_mount_slice(self.callback, self.image.capacity,
                                                          first, count), RANGE)
        self.assertFalse(self.image.out_of_bounds)

    def test_bpb_geometry_is_validated_before_data_access(self):
        cases = [(11, "H", 1024, FORMAT), (13, "B", 0, FORMAT), (13, "B", 3, FORMAT),
                 (14, "H", 0, FORMAT), (16, "B", 0, FORMAT), (16, "B", 3, FORMAT),
                 (17, "H", 1, FORMAT), (19, "H", 123, FORMAT), (22, "H", 1, FORMAT),
                 (32, "I", 0, FORMAT), (32, "I", self.image.total + 1, RANGE),
                 (36, "I", 0, FORMAT), (36, "I", 1, FORMAT),
                 (40, "H", 0x10, FORMAT), (40, "H", 0x82, FORMAT),
                 (42, "H", 1, FORMAT), (44, "I", 1, FORMAT),
                 (44, "I", self.image.clusters + 2, FORMAT), (510, "H", 0, FORMAT)]
        original = bytes(self.image.bpb)
        for offset, fmt, value, expected in cases:
            with self.subTest(offset=offset, value=value):
                self.image.bpb[:] = original
                struct.pack_into("<" + fmt, self.image.bpb, offset, value)
                self.mount(expected)
        self.image.bpb[:] = original
        put32(self.image.bpb, 32, self.image.total - 1)
        self.mount(FORMAT)  # 65524 clusters is FAT16, regardless of label.
        self.assertTrue(all(lba in (0, self.image.first) for lba in self.image.reads))

    def test_mirror_disagreement_and_reserved_high_bits(self):
        self.image.fat_overrides[1, 7] = 8
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), MIRROR)
        self.image.fat_overrides[1, 7] = 0xa0000006
        self.image.entries[5] |= 0xf0000000
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), OK)

    def test_active_nonmirrored_fat_is_selected(self):
        put16(self.image.bpb, 40, 0x81)
        for cluster in [2, 3, 5, 7, 6]:
            self.image.fat_overrides[1, cluster] = self.image.entries[cluster]
            self.image.entries[cluster] = 0x0ffffff7
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), OK)
        self.assertTrue(all(not self.image.fat_start <= lba < self.image.fat_start + self.image.fat_sectors
                            for lba in self.image.reads))

    def test_bad_free_reserved_out_of_range_and_short_long_cyclic_file_chains(self):
        for value in [0, 1, 0x0ffffff0, 0x0ffffff7, self.image.clusters + 2,
                      0x0fffffff, 5, 7]:
            with self.subTest(value=value):
                self.image.entries[7] = value
                self.mount()
                self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), CHAIN)
        self.image.entries[7] = 6
        self.image.entries[6] = 8
        self.image.entries[8] = 0x0fffffff
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), CHAIN)

    def test_directory_skips_deleted_lfn_volume_and_stops_at_zero(self):
        deleted = dirent("VERIFY.BIN", 8, 512)
        deleted[0] = 0xe5
        self.image.directory(3, [deleted, dirent("VERIFY.BIN", 8, 512, 0x0f),
                                dirent("VERIFY.BIN", 8, 512, 8),
                                dirent("VERIFY.BIN", 5, 1200)])
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), OK)
        self.image.directory(3, [bytearray(32), dirent("VERIFY.BIN", 5, 1200)])
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), NOT_FOUND)

    def test_fragmented_directory_chain_and_cycle(self):
        self.image.sectors[self.image.lba(3)] = b"\xe5" + b"\0" * 31
        self.image.sectors[self.image.lba(3)] *= 16
        self.image.entries[3] = 129
        self.image.entries[129] = 0x0fffffff
        self.image.directory(129, [dirent("VERIFY.BIN", 5, 1200)])
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), OK)
        self.image.entries[129] = 3
        self.image.sectors[self.image.lba(129)] = self.image.sectors[self.image.lba(3)]
        self.mount()
        before = len(self.image.reads)
        self.assertEqual(self.lib.test_open(b"IOSWITCH/MISSING.BIN"), CHAIN)
        self.assertLess(len(self.image.reads) - before, 16)

    def test_directory_has_finite_sector_budget(self):
        self.image.entries[3] = 100
        self.image.runs = [(100, 5000)]
        deleted = (b"\xe5" + b"\0" * 31) * 16
        self.image.sectors[self.image.lba(3)] = deleted
        self.image.synthetic_sector = lambda lba: deleted if self.image.lba(100) <= lba < self.image.lba(5100) else None
        self.mount()
        before = len(self.image.reads)
        self.assertEqual(self.lib.test_open(b"IOSWITCH/MISSING.BIN"), LIMIT)
        self.assertLess(len(self.image.reads) - before, 4200)

    def test_file_chain_and_callback_budgets_are_finite(self):
        self.image = FatImage(clusters=1048600)
        self.callback = READ512(self.image.callback)
        self.image.directory(3, [dirent("HUGE.BIN", 100, 1048577 * 512)])
        self.mount()
        before = len(self.image.reads)
        self.assertEqual(self.lib.test_open(b"IOSWITCH/HUGE.BIN"), LIMIT)
        self.assertLess(len(self.image.reads) - before, 10)

        # An otherwise valid operation cannot make an unlimited number of
        # sector callbacks, even when its requested file range is in bounds.
        self.image = FatImage()
        self.callback = READ512(self.image.callback)
        size = 33000 * 512
        self.image.runs = [(100, 33000)]
        self.image.directory(3, [dirent("LARGE.BIN", 100, size)])
        self.image.synthetic_sector = lambda lba: b"x" * 512
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/LARGE.BIN"), OK)
        before = len(self.image.reads)
        output = self.read(0, size, LIMIT)
        self.assertEqual(len(self.image.reads) - before, 32768)
        self.assertEqual(output[:512], b"x" * 512)
        self.assertEqual(output[-512:], b"\xaa" * 512)

    def test_io_failure_mount_open_read_and_output_prefix_contract(self):
        self.image.failed_lbas.add(0)
        self.mount(IO)
        self.image.failed_lbas = {self.image.first}
        self.mount(IO)
        self.image.failed_lbas = {self.image.fat_start}
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), IO)
        self.image.failed_lbas = {self.image.lba(7)}
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), OK)
        output = self.read(0, 1024, IO)
        self.assertEqual(output[:512], self.image.payload(self.image.lba(5)))
        self.assertEqual(output[512:], b"\xaa" * 512)

    def test_successful_and_failed_remount_invalidate_old_files_and_cache(self):
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), OK)
        self.mount()
        self.read(0, 1, ARG)
        self.image.entries[7] = 0
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), CHAIN)
        self.image.entries[7] = 6
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/VERIFY.BIN"), OK)
        self.mount(ARG, partition=4)
        self.read(0, 1, ARG)

    def test_64k_clusters_and_four_billion_byte_file(self):
        self.image = FatImage(spc=128, clusters=131100)
        self.callback = READ512(self.image.callback)
        size = 4_000_000_000
        first = 100
        count = (size + 65535) // 65536
        self.image.runs = [(first, count)]
        self.image.directory(3, [dirent("LARGE.BIN", first, size)])
        self.mount()
        self.assertEqual(self.lib.test_open(b"IOSWITCH/LARGE.BIN"), OK)
        self.assertLess(len(self.image.reads), 1000)  # cached FAT sectors, not one read/link
        offset = size - 513
        expected = bytearray()
        for pos in range(offset, size):
            lba = self.image.lba(first) + pos // 512
            expected.append(self.image.payload(lba)[pos % 512])
        self.assertEqual(self.read(offset, 513), expected)


if __name__ == "__main__":
    unittest.main()
