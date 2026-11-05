"""drivers/SwitchUSB's device-mode data path (SwitchXUDC.cpp section "Data endpoints", XudcTransfer.h) on the host:
the TD planner and the OUT residue reading, compiled from the header the driver uses and driven through ctypes.

Planner, over random scatter-gather layouts (whole pages, mbuf-like ranges at odd offsets, pages above 4 GiB, runs
across 64 KiB boundaries): the pieces cover the request once and in order; a direct piece is exactly the client's memory
at its offset, below 4 GiB, within one 64 KiB window and at most 64 KiB; every piece but the last is whole packets; an
OUT direct piece at a run's edge starts or ends on a cache line; a bounce piece fits a 4 KiB page; the residue probe's
first piece is one packet. Whole pages below 4 GiB, contiguous or not, are never bounced.

OUT residue: against a model controller of either kind (residue of the whole TD, as Linux tegra-xudc reads it, or of the
event's TRB, as xHCI hosts report it), the driver's protocol (probe while unknown, then read each event the learned way)
gets every transfer's length right, short or full, and learns which kind it is talking to.

Throughput (a cost model, assumptions below): USB2 high speed bulk at ~40 MB/s takes under 10% of one core, against the
serial bounce path it replaces; the same per-request costs at SuperSpeed rates."""

import ctypes
import random
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USB = ROOT / 'drivers/SwitchUSB'
DRIVER = USB / 'SwitchXUDC.cpp'
HARDWARE = USB / 'XudcHardware.cpp'
HARNESS = ROOT / 'tests/switch_xudc_transfer_harness.cpp'
GIB4 = 1 << 32
PAGE = 4096
UNKNOWN, PER_TRB, PER_TD, BROKEN = range(4)


class Lib:
    lib = None


def setUpModule():
    Lib.directory = tempfile.TemporaryDirectory(prefix='ioswitch-xudc-')
    library = Path(Lib.directory.name) / 'libxudc.dylib'
    subprocess.run(
        [
            'xcrun',
            'clang++',
            '-std=c++20',
            '-Wall',
            '-Wextra',
            '-Werror',
            '-O1',
            '-shared',
            '-fPIC',
            '-I',
            str(USB),
            str(HARNESS),
            '-o',
            str(library),
        ],
        check=True,
    )
    lib = Lib.lib = ctypes.CDLL(str(library))
    u8, u32, u64, i = ctypes.c_uint8, ctypes.c_uint32, ctypes.c_uint64, ctypes.c_int
    ptr = ctypes.POINTER
    lib.xudc_plan.argtypes = [
        ptr(u64),
        ptr(u32),
        u32,
        u32,
        u32,
        i,
        i,
        u64,
        ptr(u64),
        ptr(u32),
        ptr(u32),
        ptr(u8),
        u32,
        ptr(u32),
    ]
    lib.xudc_plan.restype = u32
    lib.xudc_out_event.argtypes = [u8, ptr(u32), u32, u32, u32, i, ptr(u32)]


def plan(runs, total, mps, out=False, probe=False, reach=GIB4, maximum=1024):
    n = len(runs)
    pas = (ctypes.c_uint64 * max(n, 1))(*[r[0] for r in runs])
    lens = (ctypes.c_uint32 * max(n, 1))(*[r[1] for r in runs])
    pa, off, ln, bounce = (
        (ctypes.c_uint64 * 1024)(),
        (ctypes.c_uint32 * 1024)(),
        (ctypes.c_uint32 * 1024)(),
        (ctypes.c_uint8 * 1024)(),
    )
    stats = (ctypes.c_uint32 * 5)()
    count = Lib.lib.xudc_plan(
        pas, lens, n, total, mps, int(out), int(probe), reach, pa, off, ln, bounce, maximum, stats
    )
    return [(pa[k], off[k], ln[k], bool(bounce[k])) for k in range(count)], list(stats)


def out_event(known, lengths, index, residue, short):
    arr = (ctypes.c_uint32 * len(lengths))(*lengths)
    out = (ctypes.c_uint32 * 4)()
    Lib.lib.xudc_out_event(known, arr, len(lengths), index, residue, int(short), out)
    return bool(out[0]), bool(out[1]), out[2], out[3]


def pa_at(runs, offset):
    start = 0
    for pa, length in runs:
        if offset < start + length:
            return pa + offset - start, start + length - offset
        start += length
    raise AssertionError('offset beyond the runs')


def random_layout(rng, kind):
    """Physically contiguous runs (merged as the driver merges them) and their total."""
    runs = []
    if kind == 'pages':
        runs.extend(
            (rng.choice([0x8000_0000, 0x1_2000_0000]) + rng.randrange(1 << 18) * PAGE, PAGE)
            for _ in range(rng.randint(1, 24))
        )
    elif kind == 'mbufs':
        for _ in range(rng.randint(1, 12)):
            size = rng.choice([20, 256, 2048, 4096, 16384, rng.randint(1, 16384)])
            base = rng.choice([0x8000_0000, 0x1_0000_0000]) + rng.randrange(1 << 20) * 64 + rng.randrange(64)
            runs.append((base, size))
    else:  # one contiguous buffer, anywhere, across 64 KiB boundaries and maybe 4 GiB
        size = rng.randint(1, 300_000)
        runs.append((rng.choice([0xFFFF_0000 - rng.randrange(1 << 16), 0x9000_0000 + rng.randrange(1 << 20)]), size))
    merged = []
    for pa, length in runs:
        if merged and merged[-1][0] + merged[-1][1] == pa:
            merged[-1] = (merged[-1][0], merged[-1][1] + length)
        else:
            merged.append((pa, length))
    return merged, sum(length for _, length in merged)


class Planner(unittest.TestCase):
    @staticmethod
    def run_offset(runs, offset):
        start = 0
        for _, length in runs:
            if offset < start + length:
                return start
            start += length
        raise AssertionError('offset beyond the runs')

    def check(self, runs, total, mps, out, probe):
        pieces, stats = plan(runs, total, mps, out, probe)
        self.assertTrue(pieces, 'planned')
        offset = 0
        for k, (pa, off, length, bounce) in enumerate(pieces):
            self.assertEqual(off, offset)
            last = k == len(pieces) - 1
            if not last:
                self.assertEqual(length % mps, 0, 'whole packets but in the last TRB')
            if bounce:
                self.assertLessEqual(length, PAGE)
            else:
                want, run_left = pa_at(runs, off)
                self.assertEqual(pa, want, 'direct: the client memory itself')
                self.assertLessEqual(length, run_left, 'within one run')
                self.assertLessEqual(pa + length, GIB4)
                self.assertLessEqual(length, 65536)
                self.assertEqual(pa >> 16, (pa + length - 1) >> 16, 'no 64 KiB boundary inside a TRB')
                if out:  # a run's own edges on a cache line (inside a run, lines are all this transfer's)
                    run_start = pa_at(runs, off)[0] - (off - self.run_offset(runs, off))
                    if pa == run_start:
                        self.assertEqual(pa % 64, 0)
                    if length == run_left:
                        self.assertEqual((pa + length) % 64, 0)
            offset += length
        self.assertEqual(offset, total)
        if probe and total >= 2 * mps:
            self.assertEqual(pieces[0][2], mps, 'the probe packet alone')
        self.assertEqual(stats[0], sum(p[2] for p in pieces if p[3]))
        return pieces, stats

    def test_random_layouts(self):
        rng = random.Random(7)
        for case in range(3000):
            kind = ('pages', 'mbufs', 'buffer')[case % 3]
            runs, total = random_layout(rng, kind)
            mps = rng.choice([512, 512, 64, 1024])
            out = rng.random() < 0.5
            self.check(runs, total, mps, out, out and rng.random() < 0.5)

    def test_whole_pages_go_direct(self):
        rng = random.Random(3)
        for _ in range(200):
            pages = [(0x8000_0000 + rng.randrange(1 << 18) * PAGE, PAGE) for _ in range(16)]
            for out in (False, True):
                pieces, stats = self.check(pages, 16 * PAGE, 512, out, False)
                self.assertEqual(stats[0], 0)
        pieces, _ = self.check([(0x9000_0000, 65536)], 65536, 512, True, False)
        self.assertEqual(len(pieces), 1)  # 64 KiB aligned contiguous: one TRB
        pieces, _ = self.check([(0x9000_8000, 65536)], 65536, 512, False, False)
        self.assertEqual(len(pieces), 2)  # across a 64 KiB boundary: split there, both direct
        self.assertFalse(any(p[3] for p in pieces))

    def test_bounced_only_what_must_be(self):
        # A page above 4 GiB: that page alone through the pool.
        pieces, stats = self.check(
            [(0x8000_0000, PAGE), (0x1_0000_0000, PAGE), (0x8000_2000, PAGE)], 3 * PAGE, 512, False, False
        )
        self.assertEqual([p[3] for p in pieces], [False, True, False])
        self.assertEqual(stats[3], PAGE // 512)
        # An mbuf chain: a 20-byte header then a payload at an odd offset: the packet across the break is copied, the
        # rest goes direct.
        runs = [(0x8000_0010, 20), (0x8100_0000, 8192)]
        pieces, stats = self.check(runs, 8212, 512, False, False)
        self.assertEqual(stats[0], 512)
        self.assertEqual(stats[1], 1)
        # OUT at an odd start: the first packet (cache-line edge) and the short last one are copied.
        pieces, stats = self.check([(0x8000_0020, 4096)], 4096, 512, True, False)
        self.assertEqual(stats[4], 2)

    def test_too_many_pieces_and_zero_length(self):
        pages = [(0x8000_0000 + k * 2 * PAGE, PAGE) for k in range(40)]
        self.assertEqual(plan(pages, 40 * PAGE, 512, maximum=39)[0], [])
        pieces, _ = plan([], 0, 512)
        self.assertEqual(pieces, [(0, 0, 0, False)])


class Controller:
    """An XUDC receiving `sent` bytes into an OUT TD of TRBs `lengths` (ISP on all, IOC on the last and, when probing,
    the first): the transfer events it raises, residue reported per TD or per TRB."""

    def __init__(self, per_td):
        self.per_td = per_td

    def events(self, lengths, sent, probe):
        """The transfer ends short (a short packet, or a zero-length one on a TRB's end) unless all of it came."""
        total, out, filled = sum(lengths), [], 0
        for k, length in enumerate(lengths):
            got = min(length, sent - filled)
            if got == length:  # this TRB full; a zero-length packet then lands in the next one
                filled += length
                if k == len(lengths) - 1 or (probe and k == 0):
                    out.append((k, total - filled if self.per_td else 0, False))
                if filled == total:
                    return out
                continue
            out.append((k, total - sent if self.per_td else length - got, True))
            return out
        return out


class OutResidue(unittest.TestCase):
    def run_protocol(self, per_td, seed):
        rng = random.Random(seed)
        hw = Controller(per_td)
        known = UNKNOWN
        for _ in range(400):
            mps = 512
            runs, total = random_layout(rng, rng.choice(['pages', 'mbufs']))
            if total < 2:
                continue
            probe = known == UNKNOWN and total >= 2 * mps
            pieces, _ = plan(runs, total, mps, True, probe)
            lengths = [p[2] for p in pieces]
            if len(lengths) > 1 and (known == BROKEN or (known == UNKNOWN and not probe)):
                continue  # the serial path: one TRB at a time, nothing to read
            sent = total if rng.random() < 0.3 else rng.randint(0, total)
            result = None
            for index, residue, short in hw.events(lengths, sent, probe and len(lengths) > 1):
                done, failed, actual, learned = out_event(known, lengths, index, residue, short)
                if learned not in (UNKNOWN, known) and known != BROKEN:
                    known = learned
                if done:
                    self.assertFalse(failed, f'{lengths} sent {sent}')
                    result = actual
                    break
            self.assertEqual(result, sent, f'{lengths} sent {sent} per_td {per_td}')
        return known

    def test_whole_td_residue(self):
        self.assertEqual(self.run_protocol(True, 1), PER_TD)

    def test_per_trb_residue(self):
        self.assertEqual(self.run_protocol(False, 2), PER_TRB)

    def test_nonsense_fails_the_request(self):
        done, failed, _, learned = out_event(PER_TD, [512, 4096, 4096], 1, 3, True)  # 3 <= rest (4096): not per TD
        self.assertTrue(failed)
        self.assertEqual(learned, BROKEN)
        done, failed, _, learned = out_event(UNKNOWN, [512, 4096], 0, 4096, False)  # the probe, per TD
        self.assertFalse(done)
        self.assertEqual(learned, PER_TD)


class Throughput(unittest.TestCase):
    """A cost model, the same costs for both paths (microseconds; assumptions, for the next boot's stats line to
    replace): an interrupt with its event and the completion 6; prepare() + complete() of a kernel range 6 plus 0.3 a
    page; getPhysicalSegment 0.3 a page; a cache line cleaned or invalidated 0.006; a copy through bcopy_phys 0.5 a KiB
    (2 GB/s); planning 0.02 a packet and a TRB 0.1 (new path only); the bus at 42.5 MB/s (USB2 high speed bulk) or
    400 MB/s (SuperSpeed). The serial path (before r17) moves one 64 KiB piece at a time: prepare, copy and flush each
    piece, and the bus waits for the CPU between pieces (20 us of interrupt and work-loop latency plus that work). The
    new path keeps requests queued, so the bus never waits, and copies only what it bounces."""

    BUS_GAP = 20.0

    def per_request(self, request, bounced, new, out):
        pages = request / PAGE
        cost = 6 + 6 + 0.3 * pages + 0.3 * pages + 0.006 * request / 64 * (2 if out else 1)
        if new:
            cost += 0.02 * request / 512 + 0.1 * pages
        cost += (bounced if new else 1.0) * request / 1024 * 0.5
        return cost

    def rate_and_cpu(self, request, bounced, new, bus, out=False):
        cost = self.per_request(request, bounced, new, out)
        wire = request / bus  # us on the bus (MB/s = bytes/us)
        seconds_per_request = wire if new else wire + self.BUS_GAP + cost
        rate = request / seconds_per_request
        return rate, cost / seconds_per_request

    def test_usb2_high_speed_under_a_tenth_of_a_core(self):
        lines = []
        for request in (16384, 65536):
            for out in (False, True):
                rate, cpu = self.rate_and_cpu(request, 0.5, True, 42.5, out)
                _, low_cpu = self.rate_and_cpu(request, 0.0, True, 42.5, out)
                old, old_cpu = self.rate_and_cpu(request, 1.0, False, 42.5, out)
                lines.append(
                    f'{"OUT" if out else "IN"} {request // 1024} KiB: {rate:.1f} MB/s at {cpu:.1%} of a core '
                    f'(half bounced), {low_cpu:.1%} (none), serial path {old:.1f} MB/s at {old_cpu:.1%}'
                )
                self.assertLess(cpu, 0.10)
                self.assertGreater(rate, old)
                self.assertLess(low_cpu, old_cpu)
        ss = self.rate_and_cpu(65536, 0.0, True, 400)[1]
        ss_half = self.rate_and_cpu(65536, 0.5, True, 400)[1]
        lines.append(
            f'SuperSpeed IN 64 KiB at 400 MB/s: {ss:.0%} of a core with nothing bounced, {ss_half:.0%} with '
            'half the pages above 4 GiB'
        )
        print('\nXUDC cost model: ' + '; '.join(lines))


class DriverGuard(unittest.TestCase):
    def test_driver_uses_the_header(self):
        source = DRIVER.read_text()
        for needle in (
            '#include "XudcTransfer.h"',
            'plan(runs, runCount, r.length, ep.mps, out, probe, kReach',
            'outEvent(residue, lengths, r.trbs, pos, e.residue(), shortPacket)',
            'kIOMemoryIncoherentIOStore',
            'kIOMemoryIncoherentIOFlush',
            'TRB_CHAIN',
            'kIOMemoryPhysicallyContiguous',
        ):
            self.assertIn(needle, source)
        # The shared bounce pool: pages freed by one endpoint's completions re-kick the others (kickWaiting after the
        # event drain and after a cancel), or an OUT read waiting for pages behind a busy IN endpoint would never start.
        self.assertIn('poolFreed = true;', source)
        self.assertEqual(source.count('    kickWaiting();'), 2)
        hardware = HARDWARE.read_text()
        self.assertIn('xw(RT_IMOD, (kImodUs * 4) << 16 | (kImodUs * 4));', hardware)
        self.assertNotIn('xw(RT_IMOD, 0);', hardware)


if __name__ == '__main__':
    unittest.main()
