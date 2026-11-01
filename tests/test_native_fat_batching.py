"""Narrow host checks of the active V14 FAT batching path; no disk/device I/O."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_native_fat32 import ARG, CHAIN, IO, OK, RANGE, READ512, FatImage, dirent
from test_native_split_disk import Part

ROOT = Path(__file__).resolve().parents[1]
STORAGE = ROOT / "out/native-a57/root-v1/tree/diagnostics/ioswitch/storage"
READ_SECTORS = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint64,
                               ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint8))


def compile_batching(directory):
    compiler = shutil.which("clang")
    if not compiler:
        raise unittest.SkipTest("C compiler unavailable")
    wrapper = Path(directory) / "batching.c"
    wrapper.write_text(r'''
#include "fat32.h"
#include "split_disk.h"
static struct iosw_fat_volume volume;
static struct iosw_fat_file file;
static struct iosw_fat_indexed_file indexed;
static struct iosw_split_indexed_disk split;
int mount_image(iosw_fat_read512 read, uint64_t capacity) {
    return iosw_fat_mount(&volume,read,0,capacity,0);
}
void set_batch(iosw_fat_read_sectors read) {volume.read_sectors=read;}
int has_batch(void) {return volume.read_sectors!=0;}
int open_file(const char *path,uint32_t *map,uint32_t count) {
    int result=iosw_fat_open(&volume,path,&file);
    return result ? result : iosw_fat_open_indexed(&volume,path,&indexed,map,count);
}
int read_file(int use_index,uint64_t offset,void *out,uint32_t length) {
    return use_index ? iosw_fat_read_indexed(&volume,&indexed,offset,out,length) :
                       iosw_fat_read(&volume,&file,offset,out,length);
}
int open_split(const struct iosw_split_part *parts,uint32_t count,uint64_t size,
               uint32_t *map,uint32_t entries) {
    return iosw_split_open_indexed(&split,&volume,parts,count,size,map,entries);
}
int read_split(uint64_t offset,void *out,uint32_t length) {
    return iosw_split_read_indexed(&split,offset,out,length);
}
void limit_volume(uint64_t capacity,uint32_t total) {
    volume.capacity_sectors=capacity;volume.total_sectors=total;
}
''')
    library = Path(directory) / "batching.so"
    subprocess.run([compiler, "-std=c11", "-shared", "-fPIC", "-O2", "-Wall",
                    "-Wextra", "-Werror", "-I", str(STORAGE), str(wrapper),
                    str(STORAGE / "fat32.c"), str(STORAGE / "split_disk.c"),
                    "-o", str(library)], check=True, capture_output=True, text=True)
    lib = ctypes.CDLL(str(library))
    lib.mount_image.argtypes = [READ512, ctypes.c_uint64]
    lib.set_batch.argtypes = [READ_SECTORS]
    lib.open_file.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32]
    lib.read_file.argtypes = [ctypes.c_int, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint32]
    lib.open_split.argtypes = [ctypes.POINTER(Part), ctypes.c_uint32, ctypes.c_uint64,
                              ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32]
    lib.read_split.argtypes = [ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint32]
    lib.limit_volume.argtypes = [ctypes.c_uint64, ctypes.c_uint32]
    return lib


class FatBatchingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.lib = compile_batching(cls.temp.name)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.batches = []
        self.batch_fail = 0
        self.batch_callback = READ_SECTORS(self.read_sectors)
        self.configure()

    def configure(self, spc=1, chain=None, size=None):
        self.image = FatImage(spc=spc)
        self.chain = list(range(100, 400)) if chain is None else chain
        self.size = len(self.chain) * spc * 512 if size is None else size
        for at, cluster in enumerate(self.chain):
            self.image.entries[cluster] = (self.chain[at+1] if at+1 < len(self.chain)
                                           else 0x0fffffff)
        self.image.directory(3, [dirent("VERIFY.BIN", self.chain[0], self.size)])
        self.single_callback = READ512(self.image.callback)
        self.assertEqual(self.lib.mount_image(self.single_callback, self.image.capacity), OK)
        self.map = (ctypes.c_uint32 * len(self.chain))()
        self.assertEqual(self.lib.open_file(b"IOSWITCH/VERIFY.BIN", self.map, len(self.chain)), OK)
        self.clear_reads()

    def clear_reads(self):
        self.batches.clear()
        self.image.reads.clear()

    def read_sectors(self, context, lba, count, output):
        del context
        self.batches.append((lba, count))
        if not 1 <= count <= 128 or not self.image.first <= lba or lba+count > self.image.first+self.image.total:
            self.image.out_of_bounds.append((lba, count))
            return 1
        if self.batch_fail == len(self.batches):
            ctypes.memset(output, 0xee, count*512)
            return 1
        for i in range(count):
            ctypes.memmove(ctypes.cast(output, ctypes.c_void_p).value+i*512,
                           bytes(self.image.sector(lba+i)), 512)
        return 0

    def batch(self, enabled=True):
        self.lib.set_batch(self.batch_callback if enabled else READ_SECTORS())

    def expected(self, offset, length, chain=None):
        chain = self.chain if chain is None else chain
        first, end = offset//512, (offset+length+511)//512
        sectors = [self.image.lba(chain[n//self.image.spc], n%self.image.spc)
                   for n in range(first, end)]
        return b"".join(bytes(self.image.sector(lba)) for lba in sectors)[offset%512:offset%512+length]

    def read(self, offset, length, status=OK, indexed=True, split=False):
        out = ctypes.create_string_buffer(b"\xaa"*(length+2), length+2)
        result = (self.lib.read_split(offset, ctypes.byref(out, 1), length) if split else
                  self.lib.read_file(indexed, offset, ctypes.byref(out, 1), length))
        self.assertEqual(result, status)
        self.assertEqual((out.raw[0], out.raw[-1]), (0xaa, 0xaa))
        self.assertFalse(self.image.out_of_bounds)
        return out.raw[1:-1]

    def test_contiguous_64k_read_replaces_128_callbacks_with_one(self):
        expected = self.read(512, 65536)
        self.assertEqual(len(self.image.reads), 128)
        self.batch(); self.clear_reads()
        self.assertEqual(self.read(512, 65536), expected)
        self.assertEqual(self.batches, [(self.image.lba(101), 128)])
        self.assertEqual(self.image.reads, [])

    def test_maximum_run_is_128_sectors_with_exact_remaining_sector(self):
        self.batch()
        self.assertEqual(self.read(0, 129*512), self.expected(0, 129*512))
        self.assertEqual(self.batches, [(self.image.lba(100), 128)])
        self.assertEqual(self.image.reads, [self.image.lba(228)])

    def test_fragmented_runs_and_cross_cluster_start_match_sector_fallback(self):
        self.configure(spc=4, chain=[100, 101, 110, 111, 112, 105])
        expected = self.read(3*512, 21*512)
        self.batch(); self.clear_reads()
        self.assertEqual(self.read(3*512, 21*512), expected)
        self.assertEqual(self.batches, [(self.image.lba(100, 3), 5),
                                       (self.image.lba(110), 12),
                                       (self.image.lba(105), 4)])
        self.assertEqual(self.image.reads, [])

    def test_unaligned_ends_read_only_covering_sectors(self):
        self.batch()
        offset, length = 501, 128*512+31
        self.assertEqual(self.read(offset, length), self.expected(offset, length))
        self.assertEqual(self.batches, [(self.image.lba(101), 128)])
        self.assertEqual(self.image.reads, [self.image.lba(100), self.image.lba(229)])

    def test_file_tail_partial_sector_never_joins_full_sector_run(self):
        self.configure(chain=[100, 101, 102, 103], size=3*512+17)
        self.batch()
        self.assertEqual(self.read(0, self.size), self.expected(0, self.size))
        self.assertEqual(self.batches, [(self.image.lba(100), 3)])
        self.assertEqual(self.image.reads, [self.image.lba(103)])

    def test_unindexed_reads_batch_within_cluster_and_match_existing_path(self):
        self.configure(spc=4, chain=[100, 101, 110])
        expected = self.read(0, self.size, indexed=False)
        self.batch(); self.clear_reads()
        self.assertEqual(self.read(0, self.size, indexed=False), expected)
        self.assertEqual(self.batches, [(self.image.lba(c), 4) for c in self.chain])

    def test_null_callback_retains_single_sector_failure_output_contract(self):
        self.image.failed_lbas.add(self.image.lba(101))
        result = self.read(0, 3*512, IO)
        self.assertEqual(result[:512], self.expected(0, 512))
        self.assertEqual(result[512:], b"\xaa"*(2*512))
        self.assertEqual(self.batches, [])
        self.assertEqual(self.image.reads, [self.image.lba(100), self.image.lba(101)])

    def test_batch_error_returns_immediately_without_fallback_or_next_run(self):
        self.batch(); self.batch_fail = 1
        result = self.read(0, 129*512, IO)
        self.assertEqual(result[:128*512], b"\xee"*(128*512))
        self.assertEqual(result[128*512:], b"\xaa"*512)
        self.assertEqual(self.batches, [(self.image.lba(100), 128)])
        self.assertEqual(self.image.reads, [])

    def test_second_batch_error_preserves_only_prior_successful_run(self):
        self.batch(); self.batch_fail = 2
        result = self.read(0, 256*512, IO)
        self.assertEqual(result[:128*512], self.expected(0, 128*512))
        self.assertEqual(result[128*512:], b"\xee"*(128*512))
        self.assertEqual(len(self.batches), 2)
        self.assertEqual(self.image.reads, [])

    def test_invalid_requested_bounds_issue_no_callbacks(self):
        self.batch()
        for offset, length in [(self.size+1, 0), (self.size, 1),
                               (self.size-511, 512), (2**64-1, 1)]:
            self.read(offset, length, RANGE)
        self.assertEqual(self.batches, [])
        self.assertEqual(self.image.reads, [])

    def test_remount_clears_callback_and_invalidates_handles(self):
        self.batch()
        self.assertEqual(self.lib.has_batch(), 1)
        self.assertEqual(self.lib.mount_image(self.single_callback, self.image.capacity), OK)
        self.assertEqual(self.lib.has_batch(), 0)
        self.read(0, 512, ARG)
        self.assertEqual(self.lib.open_file(b"IOSWITCH/VERIFY.BIN", self.map, len(self.chain)), OK)
        self.clear_reads()
        self.assertEqual(self.read(0, 1024), self.expected(0, 1024))
        self.assertEqual(self.batches, [])
        self.assertEqual(len(self.image.reads), 2)

    def test_corrupt_index_does_not_merge_or_read_invalid_cluster(self):
        self.batch(); self.map[2] = 0
        self.read(0, 4*512, CHAIN)
        self.assertEqual(self.batches, [(self.image.lba(100), 2)])
        self.assertEqual(self.image.reads, [])

    def test_contiguous_run_is_bounded_by_capacity_and_partition_before_callback(self):
        self.batch()
        end = self.image.lba(100)+2
        self.lib.limit_volume(end, self.image.total)
        self.read(0, 3*512, RANGE)
        self.lib.limit_volume(self.image.capacity, end-self.image.first)
        self.read(0, 3*512, RANGE)
        self.assertEqual(self.batches, [])
        self.assertEqual(self.image.reads, [])

    def test_split_part_boundary_stays_separate_even_if_physically_contiguous(self):
        self.image.entries[101] = 0x0fffffff
        self.image.entries[105] = 0x0fffffff
        self.image.directory(3, [dirent("PART0000.BIN", 100, 1024),
                                 dirent("PART0001.BIN", 102, 2048)])
        self.assertEqual(self.lib.mount_image(self.single_callback, self.image.capacity), OK)
        parts = (Part*2)(Part(b"IOSWITCH/PART0000.BIN", 0, 1024),
                         Part(b"IOSWITCH/PART0001.BIN", 1024, 2048))
        self.assertEqual(self.lib.open_split(parts, 2, 3072, self.map, len(self.chain)), OK)
        self.batch(); self.clear_reads()
        self.assertEqual(self.read(0, 3072, split=True), self.expected(0, 3072))
        self.assertEqual(self.batches, [(self.image.lba(100), 2), (self.image.lba(102), 4)])
        self.assertEqual(self.image.reads, [])

    def test_real_split_boundary_and_offsets_above_four_gib_use_exact_sector_runs(self):
        self.image = FatImage(spc=128, clusters=131100)
        sizes, starts = [4_000_000_000, 2_077_580_800], [100, 70000]
        counts = [(size+65535)//65536 for size in sizes]
        self.image.runs = list(zip(starts, counts))
        self.image.directory(3, [dirent("PART0000.BIN", starts[0], sizes[0]),
                                 dirent("PART0001.BIN", starts[1], sizes[1])])
        self.single_callback = READ512(self.image.callback)
        self.assertEqual(self.lib.mount_image(self.single_callback, self.image.capacity), OK)
        parts = (Part*2)(Part(b"IOSWITCH/PART0000.BIN", 0, sizes[0]),
                         Part(b"IOSWITCH/PART0001.BIN", sizes[0], sizes[1]))
        self.map = (ctypes.c_uint32 * sum(counts))()
        self.assertEqual(self.lib.open_split(parts, 2, sum(sizes), self.map, sum(counts)), OK)
        self.batch(); self.clear_reads()
        seam_lba = self.image.lba(starts[0])+sizes[0]//512-32
        next_lba = self.image.lba(starts[1])
        expected = b"".join(self.image.payload(lba) for lba in
                            [*range(seam_lba, seam_lba+32), *range(next_lba, next_lba+32)])
        self.assertEqual(self.read(sizes[0]-32*512, 64*512, split=True), expected)
        self.assertEqual(self.batches, [(seam_lba, 32), (next_lba, 32)])
        self.assertEqual(self.image.reads, [])
        self.clear_reads()
        offset = 2**32+512
        lba = next_lba+(offset-sizes[0])//512
        expected = b"".join(self.image.payload(n) for n in range(lba, lba+128))
        self.assertEqual(self.read(offset, 65536, split=True), expected)
        self.assertEqual(self.batches, [(lba, 128)])
        self.assertEqual(self.image.reads, [])


if __name__ == "__main__":
    unittest.main()
