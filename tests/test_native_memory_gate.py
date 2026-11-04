"""Execute the same bounded FDT reservation parser used before native writes."""
import ctypes
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
START, END = 0xB2000000, 0xE2200000

def u32(n):
    return struct.pack('>I', n)

def cells(*values):
    return struct.pack('>' + 'Q' * len(values), *values)

def tree(*, reservations=None, children=None, root_cells=2, reserved_cells=2,
         ranges=b'', include_reserved=True):
    keys = bytearray()
    def node(name, props, children=()):
        label = name.encode() + b'\0'
        data = u32(1) + label + bytes(-len(label) % 4)
        for key, value in props:
            offset = len(keys); keys.extend(key.encode() + b'\0')
            data += u32(3) + u32(len(value)) + u32(offset) + value + bytes(-len(value) % 4)
        return data + b''.join(children) + u32(2)
    if children is None:
        children = [('ramoops', [('reg', cells(0xB0000000, 0x200000))], []),
                    ('framebuffer-placeholder', [('reg', cells(0, 0, 0, 0))], [])]
    nested = [node(n, p, [node(*grand) for grand in c]) for n, p, c in children]
    reserved = node('reserved-memory', [('#address-cells', u32(reserved_cells)),
                    ('#size-cells', u32(2)), ('ranges', ranges)], nested)
    structure = node('', [('#address-cells', u32(root_cells)), ('#size-cells', u32(2))],
                     [reserved] if include_reserved else []) + u32(9)
    if reservations is None:
        reservations = [(0x80000000, 0x20000), (0xF5A00000, 0x400000), (0x8D000000, 0x40000)]
    reserves = b''.join(cells(*r) for r in reservations) + bytes(16)
    structure_at = 40 + len(reserves)
    strings_at = structure_at + len(structure)
    total = strings_at + len(keys)
    header = struct.pack('>10I', 0xD00DFEED, total, structure_at, strings_at, 40,
                         17, 16, 0, len(keys), len(structure))
    return header + reserves + structure + keys

class Report(ctypes.Structure):
    # The leading fields of struct loader_memory_report; the parser also fills bank_count and the RAM ledger after
    # them (about 1.6 KB), so the buffer must hold the whole struct.
    _fields_ = [('reservations', ctypes.c_uint32), ('reserved_nodes', ctypes.c_uint32),
                ('required_reservations', ctypes.c_uint32), ('ramoops_seen', ctypes.c_uint32),
                ('rest', ctypes.c_uint8 * 4096)]

class NativeMemoryGateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('clang')
        if not cc:
            raise unittest.SkipTest('clang is required to exercise the native C parser')
        cls.temp = tempfile.TemporaryDirectory()
        lib = Path(cls.temp.name) / 'memory.dylib'
        subprocess.run([cc, '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(ROOT/'native/loader/memory.c'), '-o', str(lib)], check=True,
                       capture_output=True)
        cls.lib = ctypes.CDLL(str(lib))
        cls.check = cls.lib.loader_check_fdt_memory
        cls.check.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64,
                             ctypes.c_uint64, ctypes.POINTER(Report)]
        cls.check.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def result(self, data, capacity=None):
        data = bytes(data)
        buffer = ctypes.create_string_buffer(data)
        report = Report()
        status = self.check(buffer, len(data) if capacity is None else capacity,
                            START, END, ctypes.byref(report))
        return status, report

    def test_complete_map_and_zero_sized_placeholders(self):
        rc, r = self.result(tree())
        self.assertEqual(rc, 0)
        self.assertEqual((r.reservations, r.reserved_nodes, r.required_reservations, r.ramoops_seen), (3, 2, 7, 1))

    def test_missing_required_reservations(self):
        all_ranges = [(0x80000000, 0x20000), (0xF5A00000, 0x400000), (0x8D000000, 0x40000)]
        for i in range(3):
            with self.subTest(i=i):
                self.assertEqual(self.result(tree(reservations=all_ranges[:i]+all_ranges[i+1:]))[0], 5)

    def test_reservation_anywhere_in_full_rounded_window_is_rejected(self):
        good = [(0x80000000, 0x20000), (0xF5A00000, 0x400000), (0x8D000000, 0x40000)]
        for start in (START, START + 0x3FFF, START + 0x20000000, END - 1):
            with self.subTest(start=start):
                self.assertEqual(self.result(tree(reservations=good+[(start, 1)]))[0], 4)
        self.assertEqual(self.result(tree(reservations=good+[(START-1, 1), (END, 1)]))[0], 0)

    def test_reserved_node_overlap_and_overflow_are_rejected(self):
        original = [('ramoops', [('reg', cells(0xB0000000, 0x200000))], [])]
        for start, size in ((START, 1), (END-1, 1), ((1<<64)-1, 2)):
            with self.subTest(start=start):
                self.assertEqual(self.result(tree(children=original+[('other', [('reg', cells(start, size))], [])]))[0], 20)
        # An empty interval is not an allocation, regardless of its address.
        self.assertEqual(self.result(tree(children=original+[('empty', [('reg', cells(START, 0))], [])]))[0], 0)

    def test_reservation_overflow_and_zero_size_are_rejected(self):
        self.assertEqual(self.result(tree(reservations=[((1<<64)-1, 2)]))[0], 4)
        self.assertEqual(self.result(tree(reservations=[(START, 0)]))[0], 4)

    def test_dynamic_missing_nested_and_malformed_reserved_nodes(self):
        for child in (('dynamic', [('size', cells(0x1000))], []),
                      ('dynamic', [('reg', cells(0, 0)), ('alloc-ranges', cells(0, 0))], []),
                      ('missing', [], []), ('bad-length', [('reg', bytes(15))], []),
                      ('duplicate', [('reg', cells(0, 0)), ('reg', cells(0, 0))], []),
                      ('nested', [('reg', cells(0, 0))], [('deeper', [], [])])):
            with self.subTest(child=child[0]):
                self.assertNotEqual(self.result(tree(children=[child]))[0], 0)

    def test_address_formats_ranges_and_required_ramoops(self):
        for kwargs in ({'root_cells': 1}, {'reserved_cells': 1}, {'ranges': bytes(16)},
                       {'children': []}, {'include_reserved': False}):
            with self.subTest(kwargs=kwargs):
                self.assertNotEqual(self.result(tree(**kwargs))[0], 0)

    def test_header_bounds_and_truncation(self):
        original = tree()
        for n in (0, 1, 39, 40, len(original)-1):
            self.assertNotEqual(self.result(original[:n])[0], 0)
        for offset, value in ((0, 0), (4, 0x40001), (8, 0), (12, 40), (16, 41),
                              (20, 16), (32, 0xFFFFFFFF), (36, 0xFFFFFFFF)):
            modified = bytearray(original); struct.pack_into('>I', modified, offset, value)
            with self.subTest(offset=offset):
                self.assertNotEqual(self.result(modified)[0], 0)

    def test_reserve_offset_arithmetic_cannot_wrap(self):
        for reserve, structure in ((0xfffffff8, 8), (0xfffffff0, 0),
                                   (0x40000, 0x40010), (40, 40), (48, 40)):
            modified = bytearray(tree())
            struct.pack_into('>I', modified, 8, structure)
            struct.pack_into('>I', modified, 16, reserve)
            with self.subTest(reserve=reserve, structure=structure):
                self.assertEqual(self.result(modified)[0], 2)

if __name__ == '__main__':
    unittest.main()
