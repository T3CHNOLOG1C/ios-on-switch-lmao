"""Actual portable block backend with a bounded mock source and real FAT/split I/O."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

try:
    from .test_native_fat32 import READ512, FatImage, dirent
    from .test_native_split_disk import Part
except ImportError:
    from test_native_fat32 import READ512, FatImage, dirent
    from test_native_split_disk import Part

ROOT = Path(__file__).resolve().parents[1]
OK, NOT_READY, INVALID, IO, TIMEOUT, NOMEM, CORRUPT = range(7)


class Info(ctypes.Structure):
    _fields_ = [("size_bytes", ctypes.c_uint64), ("label", ctypes.c_char * 16)]


def compile_backend(directory):
    compiler = shutil.which("clang")
    if not compiler:
        raise unittest.SkipTest("C compiler unavailable")
    wrapper = Path(directory) / "backend_wrapper.c"
    wrapper.write_text(r'''
#include "block_backend_source.h"
#include <stdlib.h>
#include <string.h>
static struct iosw_fat_volume volume;
static struct iosw_block_source source;
static struct iosw_block_backend *backend;
static struct iosw_split_part parts[2][16];
static char paths[2][16][105], labels[2][32];
static iosw_fat_read512 supplied_read;
static uint64_t capacity;
static int export_error, ready_error, begin_error, end_error, request_error;
static uint32_t active, sector_limit_override, advance_ms;
static uint64_t now_ms, request_start;
/* alloc calls, free calls, live allocations, live bytes, allocator errors,
 * begin calls, end calls, last sector budget, last seconds budget,
 * request reads, total sector callbacks. */
static uint64_t counters[11];
static unsigned fail_allocation;
static struct allocation {void *pointer; size_t bytes;} allocations[16];

static void *allocate(void *context,size_t bytes) {
    (void)context;counters[0]++;
    if(counters[0]==fail_allocation)return NULL;
    void *pointer=malloc(bytes);
    if(!pointer)return NULL;
    for(unsigned i=0;i<16;i++)if(!allocations[i].pointer) {
        allocations[i]=(struct allocation){pointer,bytes};
        counters[2]++;counters[3]+=bytes;return pointer;
    }
    counters[4]++;free(pointer);return NULL;
}
static void release(void *context,void *pointer,size_t bytes) {
    (void)context;counters[1]++;
    for(unsigned i=0;i<16;i++)if(allocations[i].pointer==pointer) {
        if(allocations[i].bytes!=bytes)counters[4]++;
        counters[2]--;counters[3]-=allocations[i].bytes;
        allocations[i]=(struct allocation){0};free(pointer);return;
    }
    counters[4]++;
}
static int ready(void *context) {(void)context;return ready_error;}
static int begin(void *context,uint32_t sectors,uint32_t seconds) {
    (void)context;counters[5]++;
    counters[7]=sectors;counters[8]=seconds;
    if(begin_error)return begin_error;
    if(active)return IOSW_BLOCK_INVALID;
    active=1;request_error=0;request_start=now_ms;counters[9]=0;
    return IOSW_BLOCK_OK;
}
static int end(void *context) {
    (void)context;counters[6]++;
    if(!active)return IOSW_BLOCK_INVALID;
    active=0;
    if(end_error)return end_error;
    if(now_ms-request_start>=counters[8]*1000)return IOSW_BLOCK_TIMEOUT;
    return request_error;
}
static int read512(void *context,uint64_t lba,uint8_t out[512]) {
    (void)context;counters[10]++;
    if(active) {
        uint64_t limit=counters[7];
        if(sector_limit_override<limit)limit=sector_limit_override;
        if(counters[9]>=limit || now_ms-request_start>=counters[8]*1000) {
            request_error=IOSW_BLOCK_TIMEOUT;return -1;
        }
        counters[9]++;now_ms+=advance_ms;
    }
    int error=supplied_read(NULL,lba,out);
    if(error && active)request_error=IOSW_BLOCK_IO;
    return error;
}
int ioswitch_files_block_source(struct iosw_block_source *out) {
    if(export_error)return export_error;
    *out=source;return IOSW_BLOCK_OK;
}
void test_backend_destroy(void) {
    if(backend){iosw_block_destroy(backend);backend=NULL;}
}
int test_backend_reset(iosw_fat_read512 callback,uint64_t sectors) {
    test_backend_destroy();
    if(counters[2] || counters[4])return -100;
    memset(counters,0,sizeof(counters));memset(&source,0,sizeof(source));
    memset(parts,0,sizeof(parts));memset(paths,0,sizeof(paths));
    memset(labels,0,sizeof(labels));memset(allocations,0,sizeof(allocations));
    volume=(struct iosw_fat_volume){0};supplied_read=callback;capacity=sectors;
    active=0;sector_limit_override=UINT32_MAX;advance_ms=0;now_ms=0;
    export_error=ready_error=begin_error=end_error=request_error=0;fail_allocation=0;
    source.volume=&volume;source.ready=ready;source.begin=begin;source.end=end;
    return iosw_fat_mount(&volume,read512,NULL,capacity,0);
}
int test_backend_set_disk(unsigned which,const struct iosw_split_part *input,
                           uint32_t count,uint64_t total,const char *label) {
    if(which>=2 || count>16)return -1;
    source.disks[which]=(struct iosw_block_source_disk){total,count,parts[which],labels[which]};
    if(!label)source.disks[which].label=NULL;
    else {strncpy(labels[which],label,31);labels[which][31]=0;}
    for(unsigned i=0;i<count;i++) {
        parts[which][i]=input[i];
        if(input[i].path) {
            strncpy(paths[which][i],input[i].path,104);paths[which][i][104]=0;
            parts[which][i].path=paths[which][i];
        }
    }
    return 0;
}
int test_backend_create(void) {
    if(backend)return IOSW_BLOCK_INVALID;
    struct iosw_block_allocator allocator={NULL,allocate,release};
    return iosw_block_create(&allocator,&backend);
}
int test_backend_info(unsigned disk,struct iosw_block_info *out) {
    return iosw_block_get_info(backend,disk,out);
}
int test_backend_read(unsigned disk,uint64_t offset,void *out,uint32_t bytes) {
    return iosw_block_read(backend,disk,offset,out,bytes);
}
int test_backend_remount(uint32_t partition) {
    return iosw_fat_mount(&volume,read512,NULL,capacity,partition);
}
void test_backend_fault(unsigned which,int value) {
    if(which==0)export_error=value;
    if(which==1)ready_error=value;
    if(which==2)begin_error=value;
    if(which==3)end_error=value;
    if(which==4)fail_allocation=(unsigned)value;
    if(which==5)sector_limit_override=(uint32_t)value;
    if(which==6)advance_ms=(uint32_t)value;
}
void test_backend_time(uint64_t value) {now_ms=value;}
uint64_t test_backend_stat(unsigned which) {return which<11?counters[which]:active;}
int test_backend_has_handle(void) {return backend!=NULL;}
''')
    library = Path(directory) / "backend.so"
    subprocess.run([compiler, "-std=c11", "-shared", "-fPIC", "-O2", "-Wall", "-Wextra",
                    "-Werror", "-I", str(ROOT / "native/storage"), str(wrapper),
                    str(ROOT / "native/storage/block_backend.c"),
                    str(ROOT / "native/storage/fat32.c"),
                    str(ROOT / "native/storage/split_disk.c"), "-o", str(library)],
                   check=True, capture_output=True, text=True)
    lib = ctypes.CDLL(str(library))
    lib.test_backend_reset.argtypes = [READ512, ctypes.c_uint64]
    lib.test_backend_set_disk.argtypes = [ctypes.c_uint32, ctypes.POINTER(Part), ctypes.c_uint32,
                                        ctypes.c_uint64, ctypes.c_char_p]
    lib.test_backend_info.argtypes = [ctypes.c_uint32, ctypes.POINTER(Info)]
    lib.test_backend_read.argtypes = [ctypes.c_uint32, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint32]
    lib.test_backend_fault.argtypes = [ctypes.c_uint32, ctypes.c_int]
    lib.test_backend_time.argtypes = [ctypes.c_uint64]
    lib.test_backend_stat.argtypes = [ctypes.c_uint32]
    lib.test_backend_stat.restype = ctypes.c_uint64
    return lib


class NativeBlockBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.lib = compile_backend(cls.temp.name)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.image = FatImage()
        self.image.entries.update({5: 7, 7: 0x0fffffff, 6: 130, 130: 0x0fffffff,
                                   8: 0x0fffffff, 10: 0x0fffffff})
        self.image.directory(3, [dirent("OS", 4, attr=0x10), dirent("SYSTEMOS", 10, attr=0x10)])
        self.image.directory(4, [dirent("PART0000.BIN", 5, 1024), dirent("PART0001.BIN", 6, 1024)])
        self.image.directory(10, [dirent("PART0000.BIN", 8, 512)])
        self.parts = [[Part(b"IOSWITCH/OS/PART0000.BIN", 0, 1024),
                       Part(b"IOSWITCH/OS/PART0001.BIN", 1024, 1024)],
                      [Part(b"IOSWITCH/SYSTEMOS/PART0000.BIN", 0, 512)]]
        self.sizes = [2048, 512]
        self.reset_source()

    def tearDown(self):
        self.lib.test_backend_destroy()
        self.assertEqual(self.stat(2), 0)
        self.assertEqual(self.stat(3), 0)
        self.assertEqual(self.stat(4), 0)
        self.assertFalse(self.image.out_of_bounds)

    def stat(self, which):
        return self.lib.test_backend_stat(which)

    def reset_source(self):
        self.callback = READ512(self.image.callback)
        self.assertEqual(self.lib.test_backend_reset(self.callback, self.image.capacity), OK)
        for which in range(2):
            self.set_disk(which)

    def set_disk(self, which, parts=None, size=None, label=None):
        parts = self.parts[which] if parts is None else parts
        size = self.sizes[which] if size is None else size
        label = [b"OS", b"SystemOS"][which] if label is None else label
        array = (Part * len(parts))(*parts)
        self.assertEqual(self.lib.test_backend_set_disk(which, array, len(parts), size, label), 0)

    def create(self, expected=OK):
        self.assertEqual(self.lib.test_backend_create(), expected)
        self.assertEqual(bool(self.lib.test_backend_has_handle()), expected == OK)
        self.assertEqual(self.stat(11), 0)
        if expected != OK:
            self.assertEqual(self.stat(2), 0)
            self.assertEqual(self.stat(3), 0)

    def read(self, disk, offset, length, expected=OK):
        out = ctypes.create_string_buffer(b"\xaa" * (length + 2), length + 2)
        before = len(self.image.reads)
        allocations = self.stat(0)
        self.assertEqual(self.lib.test_backend_read(disk, offset, ctypes.byref(out, 1), length), expected)
        self.assertEqual(out.raw[0], 0xaa)
        self.assertEqual(out.raw[-1], 0xaa)
        self.assertEqual(self.stat(0), allocations)
        self.assertEqual(self.stat(11), 0)
        self.assertFalse([lba for lba in self.image.reads[before:]
                          if self.image.fat_start <= lba < self.image.data_start])
        self.assertFalse(self.image.out_of_bounds)
        return out.raw[1:-1]

    def info(self, which, expected=OK):
        result = Info()
        ctypes.memset(ctypes.byref(result), 0xaa, ctypes.sizeof(result))
        self.assertEqual(self.lib.test_backend_info(which, ctypes.byref(result)), expected)
        if expected == NOT_READY:
            self.assertEqual(bytes(result), b"\0" * ctypes.sizeof(result))
        return result

    def test_create_owns_both_maps_info_and_exact_initialization_budget(self):
        self.create()
        self.assertEqual(self.stat(0), 3)
        self.assertEqual(self.stat(2), 3)
        self.assertEqual((self.stat(5), self.stat(6), self.stat(7), self.stat(8)), (1, 1, 131072, 120))
        for disk, label in enumerate([b"OS", b"SystemOS"]):
            metadata = self.info(disk)
            self.assertEqual(metadata.size_bytes, self.sizes[disk])
            self.assertEqual(metadata.label, label)
        self.lib.test_backend_destroy()
        self.assertEqual((self.stat(1), self.stat(2), self.stat(3)), (3, 0, 0))

    def test_every_allocation_failure_releases_prior_allocations(self):
        for failure in (1, 2, 3):
            with self.subTest(allocation=failure):
                self.reset_source()
                self.lib.test_backend_fault(4, failure)
                before = len(self.image.reads)
                self.create(NOMEM)
                self.assertEqual(self.stat(0), failure)
                self.assertEqual(self.stat(1), failure - 1)
                self.assertEqual(self.stat(5), 0)
                self.assertEqual(len(self.image.reads), before)

    def test_source_refusal_and_ready_failure_happen_before_allocation_or_io(self):
        for which, status in [(0, CORRUPT), (0, NOT_READY), (1, NOT_READY)]:
            with self.subTest(source_fault=which, status=status):
                self.reset_source()
                self.lib.test_backend_fault(which, status)
                before = len(self.image.reads)
                self.create(status)
                self.assertEqual(self.stat(0), 0)
                self.assertEqual(len(self.image.reads), before)

    def test_invalid_metadata_and_aggregate_16mib_cap_refuse_before_allocation(self):
        self.set_disk(1, label=b"label-is-too-long-for-the-abi")
        self.create(CORRUPT)
        self.assertEqual(self.stat(0), 0)
        self.image = FatImage(spc=8, clusters=1048600)
        self.reset_source()
        size = 0xfffffe00
        # Each disk fits individually: 12 MiB and 8 MiB; the shared cap is 16 MiB.
        for disk, count in [(0, 3), (1, 2)]:
            parts = [Part(b"IOSWITCH/OS/PART0000.BIN", i * size, size) for i in range(count)]
            self.set_disk(disk, parts, count * size)
        before = len(self.image.reads)
        self.create(CORRUPT)
        self.assertEqual(self.stat(0), 0)
        self.assertEqual(len(self.image.reads), before)

    def test_failed_open_and_begin_end_failures_release_all_owned_memory(self):
        self.parts[1][0].path = b"IOSWITCH/SYSTEMOS/MISSING.BIN"
        self.set_disk(1)
        self.create(CORRUPT)
        self.assertEqual((self.stat(1), self.stat(5), self.stat(6)), (3, 1, 1))
        self.parts[1][0].path = b"IOSWITCH/SYSTEMOS/PART0000.BIN"
        for phase in (2, 3):
            with self.subTest(phase=phase):
                self.reset_source()
                self.lib.test_backend_fault(phase, TIMEOUT)
                self.create(TIMEOUT)
                self.assertEqual(self.stat(1), 3)
                self.assertEqual(self.stat(6), int(phase == 3))

    def test_reads_cross_parts_without_fat_io_allocations_or_global_uptime_limit(self):
        self.create()
        self.lib.test_backend_time(200_000)
        expected = b"".join(self.image.payload(self.image.lba(c)) for c in [7, 6])
        self.assertEqual(self.read(0, 512, 1024), expected)
        self.assertEqual((self.stat(7), self.stat(8), self.stat(9)), (2, 5, 2))
        self.lib.test_backend_time(2**40)
        self.assertEqual(self.read(1, 0, 512), self.image.payload(self.image.lba(8)))
        self.assertEqual((self.stat(7), self.stat(8), self.stat(9)), (1, 5, 1))
        self.assertEqual(self.stat(0), 3)

    def test_invalid_alignment_zero_count_disk_bounds_and_overflow_do_zero_io(self):
        self.create()
        before = len(self.image.reads)
        begins = self.stat(5)
        cases = [(2, 0, 512), (0, 0, 0), (0, 1, 512), (0, 0, 513),
                 (0, 0, 65536 + 512), (0, 2048, 512), (0, 1536, 1024),
                 (0, 2**64 - 512, 512)]
        for disk, offset, length in cases:
            with self.subTest(disk=disk, offset=offset, length=length):
                self.assertEqual(self.read(disk, offset, length, INVALID), b"\xaa" * length)
        self.assertEqual(self.lib.test_backend_read(0, 0, None, 512), INVALID)
        self.assertEqual(len(self.image.reads), before)
        self.assertEqual(self.stat(5), begins)
        self.info(0)

    def test_ready_failure_latches_offline_for_both_disks(self):
        self.create()
        self.lib.test_backend_fault(1, NOT_READY)
        self.info(0, NOT_READY)
        self.lib.test_backend_fault(1, OK)
        before = len(self.image.reads)
        self.read(1, 0, 512, NOT_READY)
        self.info(1, NOT_READY)
        self.assertEqual(len(self.image.reads), before)

    def test_read_begin_failure_does_not_issue_read_or_call_end(self):
        self.create()
        self.lib.test_backend_fault(2, TIMEOUT)
        before = len(self.image.reads)
        self.assertEqual(self.read(0, 0, 512, TIMEOUT), b"\xaa" * 512)
        self.assertEqual(len(self.image.reads), before)
        self.assertEqual(self.stat(6), 1)
        self.lib.test_backend_fault(2, OK)
        self.read(1, 0, 512, NOT_READY)

    def test_read_end_failure_discards_success_and_latches_both_disks(self):
        self.create()
        self.lib.test_backend_fault(3, TIMEOUT)
        self.read(0, 0, 512, TIMEOUT)
        self.assertEqual(self.stat(9), 1)
        self.assertEqual(self.stat(6), 2)
        before = len(self.image.reads)
        self.read(1, 0, 512, NOT_READY)
        self.assertEqual(len(self.image.reads), before)

    def test_shared_request_budget_is_not_reset_at_split_boundary(self):
        self.create()
        self.lib.test_backend_fault(5, 1)
        output = self.read(0, 512, 1024, TIMEOUT)
        self.assertEqual(output[:512], self.image.payload(self.image.lba(7)))
        self.assertEqual(output[512:], b"\xaa" * 512)
        self.assertEqual((self.stat(7), self.stat(9)), (2, 1))
        self.read(1, 0, 512, NOT_READY)

    def test_slow_final_sector_fails_end_deadline_instead_of_reporting_success(self):
        self.create()
        self.lib.test_backend_fault(6, 5000)
        self.read(0, 0, 512, TIMEOUT)
        self.assertEqual(self.stat(9), 1)
        self.read(1, 0, 512, NOT_READY)

    def test_io_failure_keeps_error_prefix_contract_and_latches_offline(self):
        self.create()
        self.image.failed_lbas.add(self.image.lba(6))
        output = self.read(0, 512, 1024, IO)
        self.assertEqual(output[:512], self.image.payload(self.image.lba(7)))
        self.assertEqual(output[512:], b"\xaa" * 512)
        before = len(self.image.reads)
        self.read(1, 0, 512, NOT_READY)
        self.assertEqual(len(self.image.reads), before)

    def test_remount_invalidates_indexed_backend_without_any_data_read(self):
        self.create()
        self.assertEqual(self.lib.test_backend_remount(0), OK)
        before = len(self.image.reads)
        self.read(0, 0, 512, CORRUPT)
        self.assertEqual(len(self.image.reads), before)
        self.read(1, 0, 512, NOT_READY)

    def test_real_part_sizes_cross_decimal_and_four_gib_boundaries_with_bounded_cost(self):
        self.image = FatImage(spc=128, clusters=200000)
        sizes, starts = [4_000_000_000, 2_077_580_800, 3_303_014_400], [100, 70000, 110000]
        for start, size in zip(starts, sizes):
            self.image.runs.append((start, (size + 65535) // 65536))
        self.image.entries[10] = 0x0fffffff
        self.image.directory(3, [dirent("OS", 4, attr=0x10), dirent("SYSTEMOS", 10, attr=0x10)])
        self.image.directory(4, [dirent("PART0000.BIN", starts[0], sizes[0]),
                                dirent("PART0001.BIN", starts[1], sizes[1])])
        self.image.directory(10, [dirent("PART0000.BIN", starts[2], sizes[2])])
        self.parts = [[Part(b"IOSWITCH/OS/PART0000.BIN", 0, sizes[0]),
                       Part(b"IOSWITCH/OS/PART0001.BIN", sizes[0], sizes[1])],
                      [Part(b"IOSWITCH/SYSTEMOS/PART0000.BIN", 0, sizes[2])]]
        self.sizes = [sum(sizes[:2]), sizes[2]]
        self.reset_source()
        self.create()
        for disk, offset, length in [(0, sizes[0] - 512, 1024), (0, 2**32 - 512, 1024),
                                     (1, sizes[2] - 65536, 65536), (0, sum(sizes[:2]) - 512, 512)]:
            with self.subTest(disk=disk, offset=offset):
                before = len(self.image.reads)
                actual = self.read(disk, offset, length)
                expected = bytearray()
                for pos in range(offset, offset + length, 512):
                    part = 2 if disk else int(pos >= sizes[0])
                    local = pos - (sizes[0] if part == 1 else 0)
                    expected.extend(self.image.payload(self.image.lba(starts[part]) + local // 512))
                self.assertEqual(actual, expected)
                self.assertEqual(len(self.image.reads) - before, length // 512)
                self.assertEqual(self.stat(7), length // 512)
        self.assertEqual(self.stat(0), 3)


if __name__ == "__main__":
    unittest.main()
