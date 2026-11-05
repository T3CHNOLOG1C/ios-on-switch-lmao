"""The file SwitchNVRAM keeps the NVRAM in (drivers/SwitchNVRAM/SwitchNVRAMStore.h): its layout, round trips, the
newest-valid-slot rule, and what a damaged file falls back to. Banks are built here as IODTNVRAM serializes them (CHRP
header checksum, adler32 from the generation on) and checked against Python's zlib, an implementation of its own."""
import ctypes
import os
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STORE = ROOT / 'drivers/SwitchNVRAM'
HARNESS = ROOT / 'tests/switch_nvram_store_harness.c'
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wsign-conversion', '-I', str(STORE)]
BANK = 8192
HEADER = 32


def chrp_header(name, size):
    raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
    checksum = raw[0] + sum(raw[2:16])
    while checksum > 255:
        checksum = (checksum & 255) + (checksum >> 8)
    raw[1] = checksum
    return raw


def bank(variables=(), generation=1, size=BANK, name=b'nvram'):
    """A bank as IONVRAMCHRPHandler::serializeVariables writes it, with the packaging's proxy layout (common 2 KiB)."""
    image = bytearray(size)
    image[:16] = chrp_header(name, 32)
    image[32:48] = chrp_header(b'common', 16 + 2048)
    image[48 + 2048:48 + 2048 + 16] = chrp_header(b'free', size - 48 - 2048)
    body = b''.join(key + b'=' + value + b'\0' for key, value in variables)
    image[48:48 + len(body)] = body
    struct.pack_into('<I', image, 20, generation)
    struct.pack_into('<I', image, 16, zlib.adler32(bytes(image[20:])))
    return bytes(image)


def stride(size=BANK):
    return (HEADER + size + 4095) // 4096 * 4096


def slot(image, sequence, version=1, magic=b'IOSWNVRM', reserved=0):
    header = magic + struct.pack('<IIQI', version, len(image), sequence, reserved)
    crc = zlib.crc32(header + image)
    data = header + struct.pack('<I', crc) + image
    return data + bytes(stride(len(image)) - len(data))


@unittest.skipUnless((STORE / 'SwitchNVRAMStore.h').is_file(), 'needs drivers/SwitchNVRAM')
class SwitchNVRAMStoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='ioswitch-nvram-')
        library = Path(cls.directory.name) / 'libstore.dylib'
        subprocess.run(['clang', *FLAGS, '-shared', '-fPIC', str(HARNESS), '-o', str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.nv_crc32.restype = ctypes.c_uint32
        cls.lib.nv_crc32.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        cls.lib.nv_stride.restype = ctypes.c_uint32
        cls.lib.nv_stride.argtypes = [ctypes.c_uint32]
        cls.lib.nv_chrp_valid.restype = ctypes.c_int
        cls.lib.nv_chrp_valid.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
        cls.lib.nv_fill.restype = None
        cls.lib.nv_fill.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint64]
        cls.lib.nv_pick.restype = ctypes.c_int
        cls.lib.nv_pick.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint64)]

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def fill(self, image, sequence):
        buffer = ctypes.create_string_buffer(self.lib.nv_stride(len(image)))
        self.lib.nv_fill(buffer, image, len(image), sequence)
        return buffer.raw

    def pick(self, data, size=BANK):
        sequence = ctypes.c_uint64(0)
        index = self.lib.nv_pick(data, len(data), size, ctypes.byref(sequence))
        return index, sequence.value

    def image_of(self, data, index, size=BANK):
        start = index * stride(size) + HEADER
        return data[start:start + size]

    def write(self, data, current, image, sequence):
        """The driver's write: the slot that does not hold the current bank (SwitchNVRAM::writeOut)."""
        target = current ^ 1
        data = bytearray(data.ljust(2 * stride(), b'\0'))
        data[target * stride():(target + 1) * stride()] = self.fill(image, sequence)
        return bytes(data), target

    # --- format ---------------------------------------------------------------------------------------------------
    def test_crc32_is_zlibs(self):
        for data in (b'', b'123456789', os.urandom(1), os.urandom(8192 + 33)):
            self.assertEqual(self.lib.nv_crc32(data, len(data)), zlib.crc32(data))

    def test_stride_is_header_and_bank_in_4k_blocks(self):
        self.assertEqual(self.lib.nv_stride(8192), 12288)
        self.assertEqual(self.lib.nv_stride(4096 - 32), 4096)
        self.assertEqual(self.lib.nv_stride(65536), 69632)

    def test_slot_layout(self):
        image = bank([(b'boot-args', b'-v')], generation=7)
        self.assertEqual(self.fill(image, 0x100000002), slot(image, 0x100000002))

    def test_chrp_banks_iodtnvram_accepts(self):
        self.assertTrue(self.lib.nv_chrp_valid(bank(), BANK))
        self.assertTrue(self.lib.nv_chrp_valid(bank([(b'fm-spstatus', b'abc'), (b'ldm', b'true')]), BANK))
        self.assertTrue(self.lib.nv_chrp_valid(bank(name=b'2nvram'), BANK))

    def test_chrp_banks_iodtnvram_refuses(self):
        good = bank([(b'StartupMute', b'0')])
        bad_adler = bytearray(good)
        bad_adler[50] ^= 1
        bad_checksum = bytearray(good)
        bad_checksum[1] ^= 1
        self.assertFalse(self.lib.nv_chrp_valid(bytes(bad_adler), BANK))
        self.assertFalse(self.lib.nv_chrp_valid(bytes(bad_checksum), BANK))
        self.assertFalse(self.lib.nv_chrp_valid(bank(name=b'common'), BANK))
        self.assertFalse(self.lib.nv_chrp_valid(bytes(BANK), BANK))
        self.assertFalse(self.lib.nv_chrp_valid(good[:16], 16))

    # --- round trips ----------------------------------------------------------------------------------------------
    def test_round_trip(self):
        image = bank([(b'boot-args', b''), (b'fm-spstatus', b'ab')])
        data, current = self.write(b'', 1, image, 1)
        self.assertEqual(current, 0)
        self.assertEqual(self.pick(data), (0, 1))
        self.assertEqual(self.image_of(data, 0), image)

    def test_writes_alternate_and_the_newest_wins(self):
        data, current = b'', 1
        for sequence in range(1, 8):
            image = bank([(b'count', str(sequence).encode())], generation=sequence)
            data, current = self.write(data, current, image, sequence)
            self.assertEqual(self.pick(data), (current, sequence))
            self.assertEqual(self.image_of(data, current), image)
        self.assertEqual(len(data), 2 * stride())

    def test_sequence_is_64_bit(self):
        old, new = bank([(b'a', b'1')]), bank([(b'a', b'2')])
        data = self.fill(old, 0xffffffff) + self.fill(new, 0x100000000)
        self.assertEqual(self.pick(data), (1, 0x100000000))
        data = self.fill(new, 0x100000000) + self.fill(old, 0xffffffff)
        self.assertEqual(self.pick(data), (0, 0x100000000))

    # --- damage ---------------------------------------------------------------------------------------------------
    def test_torn_newest_write_falls_back_to_the_previous_bank(self):
        banks = [bank([(b'v', str(n).encode() * 40)], generation=n) for n in (1, 2, 3)]
        data, current = self.write(b'', 1, banks[0], 1)
        data, current = self.write(data, current, banks[1], 2)
        complete, target = self.write(data, current, banks[2], 3)
        self.assertEqual(self.pick(complete), (target, 3))
        start = target * stride()
        # A power cut during write 3: its slot holds the new bytes up to the cut and write 1's after it.
        for cut in (8, HEADER, HEADER + 48 + 3, HEADER + 48 + 39):
            torn = bytearray(data)
            torn[start:start + cut] = complete[start:start + cut]
            self.assertEqual(self.pick(bytes(torn)), (current, 2), cut)
            self.assertEqual(self.image_of(bytes(torn), current), banks[1])

    def test_a_flipped_byte_anywhere_crc_covers_rejects_that_slot(self):
        image = bank([(b'boot-args', b'-v')])
        good = self.fill(image, 5)
        for at in list(range(0, HEADER)) + [HEADER, HEADER + 48, HEADER + BANK - 1]:
            damaged = bytearray(good)
            damaged[at] ^= 0x40
            self.assertEqual(self.pick(bytes(damaged)), (-1, 0), at)
        damaged = bytearray(good)
        damaged[HEADER + BANK] ^= 0x40   # padding: not part of the bank, ignored
        self.assertEqual(self.pick(bytes(damaged)), (0, 5))

    def test_valid_crc_over_a_bank_iodtnvram_would_panic_on(self):
        image = bytearray(bank([(b'boot-args', b'-v')]))
        image[60] ^= 1   # adler now wrong: IONVRAMCHRPHandler::unserializeImage panics on this
        self.assertEqual(self.pick(slot(bytes(image), 3)), (-1, 0))

    def test_other_versions_magics_sizes_and_reserved_bits(self):
        image = bank()
        self.assertEqual(self.pick(slot(image, 1, version=2)), (-1, 0))
        self.assertEqual(self.pick(slot(image, 1, magic=b'IOSWNVRX')), (-1, 0))
        self.assertEqual(self.pick(slot(image, 1, reserved=1)), (-1, 0))
        small = bank(size=4096)
        self.assertEqual(self.pick(slot(small, 1), size=BANK), (-1, 0))
        self.assertEqual(self.pick(slot(small, 1), size=4096), (0, 1))

    def test_short_empty_and_garbage_files_start_empty(self):
        good = self.fill(bank(), 9)
        self.assertEqual(self.pick(b''), (-1, 0))
        self.assertEqual(self.pick(good[:HEADER + BANK - 1]), (-1, 0))
        self.assertEqual(self.pick(good[:HEADER + BANK]), (0, 9))
        self.assertEqual(self.pick(os.urandom(2 * stride())), (-1, 0))
        self.assertEqual(self.pick(bytes(2 * stride())), (-1, 0))
        # Slot 1 cut short by a power cut while the file grew: slot 0 still counts.
        self.assertEqual(self.pick(good + self.fill(bank(), 10)[:200]), (0, 9))

    def test_every_file_length_under_address_sanitizer(self):
        executable = Path(self.directory.name) / 'lengths'
        subprocess.run(['clang', *FLAGS, '-DSWITCH_NVRAM_HARNESS_MAIN', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', str(HARNESS), '-o', str(executable)], check=True)
        result = subprocess.run([str(executable)], text=True, capture_output=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: 24577 file lengths', result.stdout)


if __name__ == '__main__':
    unittest.main()
