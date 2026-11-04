"""loader_check_fdt_memory after the r9-preboot round 4 rewrite (one walk for several ranges, word reads) against the
previous parser, kept verbatim in tests/native_memory_reference.c: on generated and randomly damaged FDTs, every
single-range status is unchanged, and so is the whole report whenever it passes (the only time the loader
uses it), and the multi-range result is what the single-range
calls in index order would have stopped at."""
import ctypes
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from test_native_memory_gate import cells, tree, u32  # noqa: E402

REPORT_BYTES = 4096  # larger than struct loader_memory_report


class Range(ctypes.Structure):
    _fields_ = [('start', ctypes.c_uint64), ('end', ctypes.c_uint64)]


def fdts(rng):
    yield tree()
    for _ in range(300):
        reservations = [(0x80000000, 0x20000), (0xF5A00000, 0x400000), (0x8D000000, 0x40000)]
        for _ in range(rng.randrange(4)):
            reservations.append((rng.choice([0xA0000000, 0xAA000000, 0xB1000000, 0xB8000000, 0xE0000000]) +
                                 rng.randrange(0, 0x2000000, 0x1000), rng.choice([0x1000, 0x200000, 0x3000000])))
        rng.shuffle(reservations)
        children = [('ramoops', [('reg', cells(0xB0000000, 0x200000))], [])]
        for n in range(rng.randrange(4)):
            children.append((f'carve{n}', [('reg', cells(rng.choice([0xA001E000, 0xAA100000, 0xC0000000, 0xFFE00000]),
                                                         rng.choice([0x1000, 0x100000])))], []))
        yield tree(reservations=reservations, children=children)
    base = tree()
    for _ in range(300):
        damaged = bytearray(base)
        for _ in range(rng.randrange(1, 4)):
            damaged[rng.randrange(len(damaged))] = rng.randrange(256)
        yield bytes(damaged)


class NativeMemoryRangesTests(unittest.TestCase):
    def test_single_and_multi_range_match_the_previous_parser(self):
        with tempfile.TemporaryDirectory(prefix='ioswitch-memory-') as directory:
            directory = Path(directory)
            libs = {}
            for name, source in (('new', ROOT / 'native/loader/memory.c'),
                                 ('old', ROOT / 'tests/native_memory_reference.c')):
                lib = directory / f'{name}.dylib'
                subprocess.run(['clang', '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror',
                                '-I', str(ROOT / 'native/loader'), str(source),
                                '-o', str(lib)], check=True, capture_output=True)
                libs[name] = ctypes.CDLL(str(lib))
            single = {n: libs[n].loader_check_fdt_memory for n in libs}
            for f in single.values():
                f.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_void_p]
                f.restype = ctypes.c_int
            multi = libs['new'].loader_check_fdt_memory_ranges
            multi.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(Range), ctypes.c_uint,
                              ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)]
            multi.restype = ctypes.c_int
            rng = random.Random(1104)
            candidates = [(0xB2000000, 0xE2200000), (0xA001F000, 0xA0020000), (0xAA100000, 0xAAF43000),
                          (0xA0000000, 0xA001D000), (0xA001EF00, 0xA001F000), (0xB0000000, 0xB0001000)]
            checked = multis = 0
            for data in fdts(rng):
                buffer = ctypes.create_string_buffer(data, len(data) + 8)
                picks = rng.sample(candidates, rng.randrange(1, len(candidates) + 1))
                expected = None
                for index, (start, end) in enumerate(picks):
                    reports = {n: ctypes.create_string_buffer(REPORT_BYTES) for n in single}
                    status = {n: single[n](buffer, len(data), start, end, reports[n]) for n in single}
                    self.assertEqual(status['new'], status['old'], (hex(start), data[:16]))
                    if not status['old']:  # callers use the report only when the walk passed
                        self.assertEqual(reports['new'].raw, reports['old'].raw)
                    checked += 1
                    if expected is None and status['old']:
                        expected = (status['old'], index)
                ranges = (Range * len(picks))(*[Range(s, e) for s, e in picks])
                failed = ctypes.c_uint(99)
                report = ctypes.create_string_buffer(REPORT_BYTES)
                status = multi(buffer, len(data), ranges, len(picks), report, ctypes.byref(failed))
                self.assertEqual((status, failed.value) if status else (0, None), expected or (0, None))
                multis += 1
            self.assertGreater(checked, 1000)
            print(f'{checked} single-range and {multis} multi-range checks')


if __name__ == '__main__':
    unittest.main()
