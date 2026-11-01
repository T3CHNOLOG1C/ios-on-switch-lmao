"""Independent serialized card-evidence fixtures; no firmware or device access."""
from pathlib import Path
import ctypes
import hashlib
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib

from tests.test_native_sd_reference import Reference, make_page

ROOT = Path(__file__).resolve().parents[1]


class Card(ctypes.Structure):
    _fields_ = [('capacity_sectors', ctypes.c_uint64), ('capacity_bytes', ctypes.c_uint64),
                ('ocr', ctypes.c_uint32), ('rca', ctypes.c_uint32), ('version', ctypes.c_uint32),
                ('csd', ctypes.c_uint32 * 4), ('cid', ctypes.c_uint32 * 4)]


def checksum(page):
    struct.pack_into('<I', page, 0x4F0, zlib.crc32(page[0x400:0x4F0]))


def card_page(csize=1952879, commit=0):
    page = make_page(commit=commit)
    struct.pack_into('<II', page, 4, 2, 0x500)
    struct.pack_into('<8I', page, 0x400, 0x4D435349, 1, 0x100, 1, 0x700B0000, 1, 512, 1)
    sectors = (csize + 1) * 1024
    struct.pack_into('<QQ4I', page, 0x420, sectors, sectors * 512,
                     0xC0FF8000, 0x1234, 0x80030000, 0)
    struct.pack_into('<4I', page, 0x440, 0x400E0032, (9 << 16) | (csize >> 16),
                     (csize & 0xFFFF) << 16, 0x01C00000)
    struct.pack_into('<4I', page, 0x450, 0x03534453, 0x44534331, 0x12345678, 0x9ABCDEF0)
    struct.pack_into('<4Q', page, 0x460, 0xFF010000, 0xFF020000, 0xFFCF0000, 0x77B77000)
    page[0x480:0x4A0] = hashlib.sha256((ROOT / 'out/switch/boot-reference/bl33.bin').read_bytes()).digest()
    checksum(page)
    return page


class NativeSdCardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        clang = shutil.which('clang')
        if not clang:
            raise unittest.SkipTest('clang unavailable')
        temp = tempfile.TemporaryDirectory(prefix='iosw-sd-card-')
        cls.addClassCleanup(temp.cleanup)
        path = Path(temp.name) / 'card.so'
        subprocess.run([clang, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-shared', '-fPIC', str(ROOT / 'native/storage/sd_reference.c'),
                        '-o', str(path)], check=True, capture_output=True, text=True)
        cls.lib = ctypes.CDLL(str(path))
        cls.lib.iosw_sd_card_reference_validate.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
            ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p]
        cls.lib.iosw_sd_card_reference_validate.restype = ctypes.c_int
        cls.lib.iosw_sd_reference_validate.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
            ctypes.c_uint32, ctypes.c_void_p]

    def invoke(self, page, expected=0, commit=0, length=4096):
        raw = ctypes.create_string_buffer(bytes(page))
        reference, card = Reference(), Card()
        ctypes.memset(ctypes.addressof(reference), 0xA5, ctypes.sizeof(reference))
        ctypes.memset(ctypes.addressof(card), 0xA5, ctypes.sizeof(card))
        old = bytes(reference), bytes(card)
        result = self.lib.iosw_sd_card_reference_validate(raw, length, commit,
                                                         ctypes.byref(reference), ctypes.byref(card))
        self.assertEqual(result, expected)
        self.assertEqual(raw.raw[:-1], bytes(page))
        if result:
            self.assertEqual((bytes(reference), bytes(card)), old)
        else:
            self.assertEqual(bytes(reference.sector), bytes(page[0x200:0x400]))
            self.assertEqual(card.capacity_sectors, struct.unpack_from('<Q', page, 0x420)[0])
            self.assertEqual(card.capacity_bytes, card.capacity_sectors * 512)
        return card

    def test_valid_capacities_and_both_commit_phases(self):
        for size in (0, 0xFFF, 1952879, 0x3FFFFF):
            for commit in (0, 1):
                with self.subTest(size=size, commit=commit):
                    self.invoke(card_page(size, commit), commit=commit)

    def test_legacy_and_extended_profiles_are_not_interchangeable(self):
        self.invoke(make_page(), expected=6)
        raw = ctypes.create_string_buffer(bytes(card_page()))
        self.assertEqual(self.lib.iosw_sd_reference_validate(raw, 4096, 0, ctypes.byref(Reference())), 2)
        for word, value in ((4, 1), (8, 0x400), (8, 0x600), (0x2C, 1)):
            page = card_page(); struct.pack_into('<I', page, word, value)
            self.invoke(page, expected=2)

    def test_sizes_and_nulls_refuse_before_any_read(self):
        for size in (0, 1, 4095, 4097, 0xFFFFFFFF):
            self.invoke(b'X', expected=1, length=size)
        self.invoke(b'X', expected=1, commit=2)
        self.assertEqual(self.lib.iosw_sd_card_reference_validate(None, 4096, 0,
                         ctypes.byref(Reference()), ctypes.byref(Card())), 1)
        raw = ctypes.create_string_buffer(bytes(card_page()))
        self.assertEqual(self.lib.iosw_sd_card_reference_validate(raw, 4096, 0,
                         None, ctypes.byref(Card())), 1)
        self.assertEqual(self.lib.iosw_sd_card_reference_validate(raw, 4096, 0,
                         ctypes.byref(Reference()), None), 1)

    def test_fixed_export_identity_and_firmware_hash(self):
        for offset in (*range(0x400, 0x420, 4), *range(0x480, 0x4A0)):
            with self.subTest(offset=hex(offset)):
                page = card_page(); page[offset] ^= 1; checksum(page)
                self.invoke(page, expected=6)

    def test_all_card_evidence_bytes_are_crc_protected(self):
        for offset in range(0x420, 0x4F4):
            with self.subTest(offset=hex(offset)):
                page = card_page(); page[offset] ^= 1
                self.invoke(page, expected=4)

    def test_reserved_bytes_cannot_be_smuggled_under_valid_crc(self):
        for offset in (*range(0x43C, 0x440), *range(0x4A0, 0x4F0),
                       *range(0x4F4, 0x500), 0x30, 0x1FF, 0x500, 0xFFF):
            with self.subTest(offset=hex(offset)):
                page = card_page(); page[offset] = 1; checksum(page)
                self.invoke(page, expected=3)

    def test_wrong_card_protocol_and_csd_capacity_refuse(self):
        cases = [(0x430, 0x80FF8000), (0x430, 0x40FF8000), (0x434, 0),
                 (0x434, 0x10000), (0x438, 0x30000), (0x440, 0),
                 (0x440, 0x800E0032), (0x444, 8 << 16)]
        for offset, value in cases:
            with self.subTest(offset=hex(offset), value=value):
                page = card_page(); struct.pack_into('<I', page, offset, value); checksum(page)
                self.invoke(page, expected=6)
        for offset, value in ((0x420, 0), (0x420, 0x100000001), (0x428, 512),
                              (0x428, 0xFFFFFFFFFFFFFFFF)):
            page = card_page(); struct.pack_into('<Q', page, offset, value); checksum(page)
            self.invoke(page, expected=6)
        page = card_page(); page[0x450:0x460] = bytes(16); checksum(page)
        self.invoke(page, expected=6)

    def test_firmware_pointer_provenance_is_bounded_and_aligned(self):
        for offset in (0x460, 0x468, 0x470):
            for value in (0, 0xEFFFFFF8, 0xFF010001, 0xFFD00000, 0x100000000):
                page = card_page(); struct.pack_into('<Q', page, offset, value); checksum(page)
                self.invoke(page, expected=6)
        page = card_page(); page[0x468:0x470] = page[0x460:0x468]; checksum(page)
        self.invoke(page, expected=6)

    def test_sector_crc_failure_never_commits_valid_card_output(self):
        page = card_page(); page[0x234] ^= 1
        self.invoke(page, expected=4)


if __name__ == '__main__':
    unittest.main()
