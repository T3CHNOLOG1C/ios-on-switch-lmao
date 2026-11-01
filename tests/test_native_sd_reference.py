"""Independent host checks for the fixed, same-boot sector-reference contract."""
from pathlib import Path
import ctypes
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
PAGE_BYTES = 4096
DATA_OFFSET = 512
DATA_BYTES = 512
HEADER = (0x52445349, 1, 0x400, 0x200, 0x200, 0x700B0000, 0, 1, 1, 0)


class Reference(ctypes.Structure):
    _fields_ = [('crc32', ctypes.c_uint32), ('sector', ctypes.c_uint8 * DATA_BYTES)]


class GuardedReference(ctypes.Structure):
    _fields_ = [('before', ctypes.c_uint8 * 16), ('result', Reference),
                ('after', ctypes.c_uint8 * 16)]


def make_page(sector=None, commit=0):
    if sector is None:
        sector = bytes(range(256)) * 2
    assert len(sector) == DATA_BYTES
    page = bytearray(PAGE_BYTES)
    struct.pack_into('<10I', page, 0, *HEADER)
    # This deliberately follows the U-Boot byte contract, independent of C.
    struct.pack_into('>I', page, 0x28, zlib.crc32(sector))
    struct.pack_into('<I', page, 0x2C, commit)
    page[DATA_OFFSET:DATA_OFFSET + DATA_BYTES] = sector
    return page


class NativeSdReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang')
        if not compiler:
            raise unittest.SkipTest('C compiler unavailable')
        cls.temporary = tempfile.TemporaryDirectory(prefix='iosw-sd-reference-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        library = cls.directory / 'reference.so'
        subprocess.run([compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fPIC', '-shared', '-I', str(ROOT / 'native/storage'),
                        str(ROOT / 'native/storage/sd_reference.c'), '-o', str(library)],
                       check=True, capture_output=True, text=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.iosw_sd_reference_validate.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                                      ctypes.c_uint32, ctypes.c_void_p]
        cls.lib.iosw_sd_reference_validate.restype = ctypes.c_int
        cls.lib.iosw_sd_crc32.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        cls.lib.iosw_sd_crc32.restype = ctypes.c_uint32
        cls.compiler = compiler

    def invoke(self, page, expected_commit=0, length=PAGE_BYTES, expected_error=0):
        raw = (ctypes.c_uint8 * len(page)).from_buffer_copy(page)
        output = GuardedReference()
        ctypes.memset(ctypes.addressof(output), 0xA5, ctypes.sizeof(output))
        before = bytes(output)
        error = self.lib.iosw_sd_reference_validate(raw, length, expected_commit,
                                                   ctypes.byref(output.result))
        self.assertEqual(error, expected_error)
        self.assertEqual(bytes(raw), bytes(page), 'validator modified input')
        self.assertEqual(bytes(output.before), b'\xA5' * 16)
        self.assertEqual(bytes(output.after), b'\xA5' * 16)
        if error:
            self.assertEqual(bytes(output), before, 'failure committed output')
        else:
            sector = bytes(page[DATA_OFFSET:DATA_OFFSET + DATA_BYTES])
            self.assertEqual(bytes(output.result.sector), sector)
            self.assertEqual(output.result.crc32, zlib.crc32(sector))
        return output

    def test_valid_loader_and_kernel_commits_and_sector_patterns(self):
        rng = random.Random(0x49534452)
        patterns = (bytes(512), b'\xFF' * 512, bytes(range(256)) * 2,
                    bytes(rng.randrange(256) for _ in range(512)))
        for commit in (0, 1):
            for sector in patterns:
                with self.subTest(commit=commit, crc=zlib.crc32(sector)):
                    self.invoke(make_page(sector, commit), commit)

    def test_crc_against_zlib_known_vectors_and_non_word_lengths(self):
        rng = random.Random(0x53444352)
        vectors = [b'', b'123456789', bytes(512), b'\xFF' * 512]
        vectors += [bytes(rng.randrange(256) for _ in range(length))
                    for length in (1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64,
                                   127, 128, 255, 256, 511, 512, 513, 1023, 4096)]
        for value in vectors:
            with self.subTest(length=len(value)):
                raw = ctypes.create_string_buffer(value)
                self.assertEqual(self.lib.iosw_sd_crc32(raw, len(value)), zlib.crc32(value))
        self.assertEqual(self.lib.iosw_sd_crc32(None, 0), 0)

    def test_rejects_wrong_page_size_before_reading_any_page_bytes(self):
        # A one-byte allocation is enough because every wrong size must fail
        # before a header read. This checks the bound independently of padding.
        for length in (0, 1, 511, 512, 1024, 4095, 4097, 0xFFFFFFFF):
            with self.subTest(length=length):
                self.invoke(b'\xEE', length=length, expected_error=1)

    def test_rejects_null_arguments_and_invalid_expected_commit(self):
        output = GuardedReference()
        ctypes.memset(ctypes.addressof(output), 0xA5, ctypes.sizeof(output))
        before = bytes(output)
        self.assertEqual(self.lib.iosw_sd_reference_validate(None, 4096, 0,
                                                            ctypes.byref(output.result)), 1)
        self.assertEqual(bytes(output), before)
        raw = ctypes.create_string_buffer(bytes(make_page()))
        self.assertEqual(self.lib.iosw_sd_reference_validate(raw, 4096, 0, None), 1)
        for commit in (2, 0xFFFFFFFF):
            self.invoke(b'\xEE', expected_commit=commit, expected_error=1)

    def test_every_header_word_and_each_high_bit_is_fixed(self):
        for word in range(10):
            for bit in (0, 7, 15, 23, 31):
                with self.subTest(word=word, bit=bit):
                    page = make_page()
                    struct.pack_into('<I', page, word * 4, HEADER[word] ^ (1 << bit))
                    self.invoke(page, expected_error=2)
        # These specifically distinguish whole-device sector0 from partition
        # data or a guessed SDHC/eMMC descriptor.
        for offset, value in ((0x14, 0x700B0600), (0x18, 1), (0x1C, 2),
                              (0x20, 0), (0x24, 1), (0x0C, 0x1000),
                              (0x10, 0xFFFFFFFF)):
            page = make_page()
            struct.pack_into('<I', page, offset, value)
            self.invoke(page, expected_error=2)

    def test_commit_must_exactly_match_the_callers_phase(self):
        self.invoke(make_page(commit=0), expected_commit=1, expected_error=2)
        self.invoke(make_page(commit=1), expected_commit=0, expected_error=2)
        for commit in (2, 0xFFFFFFFF):
            self.invoke(make_page(commit=commit), expected_error=2)

    def test_every_reserved_byte_must_be_zero(self):
        page = make_page()
        for offset in (*range(0x30, 0x200), *range(0x400, 0x1000)):
            with self.subTest(offset=hex(offset)):
                page[offset] = 1
                self.invoke(page, expected_error=3)
                page[offset] = 0

    def test_every_sector_byte_is_covered_by_crc_without_partial_output(self):
        page = make_page()
        for offset in range(DATA_OFFSET, DATA_OFFSET + DATA_BYTES):
            with self.subTest(offset=hex(offset)):
                page[offset] ^= 1
                self.invoke(page, expected_error=4)
                page[offset] ^= 1

    def test_crc_is_big_endian_and_all_four_bytes_are_checked(self):
        for offset in range(0x28, 0x2C):
            page = make_page()
            page[offset] ^= 1
            self.invoke(page, expected_error=4)
        page = make_page()
        crc = zlib.crc32(page[0x200:0x400])
        self.assertNotEqual(struct.pack('<I', crc), struct.pack('>I', crc))
        struct.pack_into('<I', page, 0x28, crc)
        self.invoke(page, expected_error=4)

    def test_unaligned_input_page_and_guarded_read_only_page(self):
        # The serialized contract uses byte reads, so no aligned host deref is
        # needed. Actual firmware page address remains fixed and aligned.
        page = bytes(make_page())
        raw = ctypes.create_string_buffer(b'X' + page + b'Y')
        out = Reference()
        self.assertEqual(self.lib.iosw_sd_reference_validate(ctypes.byref(raw, 1), 4096,
                                                            0, ctypes.byref(out)), 0)
        self.assertEqual(raw.raw[0], ord('X'))
        self.assertEqual(raw.raw[-2], ord('Y'))
        # Separate process: surrounding inaccessible pages catch reads outside
        # the exact4096-byte extent; mprotect also proves input is not written.
        source = self.directory / 'guard.c'
        source.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "sd_reference.h"
int main(void) {
    size_t granule = (size_t)sysconf(_SC_PAGESIZE);
    assert(granule >= IOSW_SD_REFERENCE_BYTES);
    uint8_t *allocation = mmap(0, granule * 3, PROT_NONE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(allocation != MAP_FAILED);
    uint8_t *middle = allocation + granule;
    assert(mprotect(middle, granule, PROT_READ | PROT_WRITE) == 0);
    /* Place at both accessible-page boundaries across separate validations. */
    for (unsigned end = 0; end < 2; ++end) {
        uint8_t *page = middle + (end ? granule - 4096 : 0);
        memset(page, 0, 4096);
        uint32_t header[10] = {0x52445349,1,0x400,0x200,0x200,0x700b0000,0,1,1,0};
        memcpy(page, header, sizeof header);
        /* zlib.crc32(bytes(512)) = 0xb2aa7578; firmware serializes big endian. */
        page[0x28]=0xb2;page[0x29]=0xaa;page[0x2a]=0x75;page[0x2b]=0x78;
        assert(mprotect(middle, granule, PROT_READ) == 0);
        struct iosw_sd_reference out;
        assert(iosw_sd_reference_validate(page,4096,0,&out) == IOSW_SD_REF_OK);
        assert(out.crc32 == 0xb2aa7578);
        assert(mprotect(middle, granule, PROT_READ | PROT_WRITE) == 0);
    }
    assert(munmap(allocation, granule * 3) == 0);
    return 0;
}
''')
        executable = self.directory / 'guard'
        subprocess.run([self.compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'native/storage'), str(source),
                        str(ROOT / 'native/storage/sd_reference.c'), '-o', str(executable)],
                       check=True, capture_output=True, text=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
