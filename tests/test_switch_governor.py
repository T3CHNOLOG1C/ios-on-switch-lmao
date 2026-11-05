"""The on-demand clock governor (drivers/SwitchPower/SwitchGovernor.h): load traces in, operating points out, with the
tunables the kernel uses (read from SwitchPerfGovernor.cpp and TegraGM20B gpu.c, so a retune is tested as shipped).

Checks: saturation reaches the ceiling within one window and a partial burst within two (a frame at 60 Hz is two
CPU windows); idle and short dips keep the clock until the down hold has passed, then the clock goes to the highest
target the hold saw; boosts raise at once and expire; thermal ceilings and the cold floor clamp at once; nothing ever
leaves [lo, hi] or the table (fuzzed). The tables: the CPU's from L4T's T210B01 CVB rows (clk-tegra124-dfll-fcpu.c
CPUB01_CVB_TABLE, evaluated as cvb.c does) cut at Horizon's 1785 MHz boost clock, the GPU's from L4T's B01 noise-aware
table (tegra210-dvfs.c GPUB01_NA_CVB_TABLE, as TegraGM20B platform.c evaluates it) cut at Horizon's 768 MHz, each
re-derived here independently and compared with what the header picks."""
import ctypes
import random
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TREE = ROOT / 'out/native-a57/root-v1/tree'
POWER = TREE / 'drivers/SwitchPower'
HARNESS = ROOT / 'tests/switch_governor_harness.c'
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wsign-conversion', '-I', str(POWER)]

NINTENDO_CPU_MAX_KHZ = 1785000   # Horizon's boost-mode CPU clock (sys-clk README's table: "1785 -> max clock, boost mode")
NINTENDO_GPU_MAX_KHZ = 768000    # Horizon's docked GPU clock, its highest ("768 -> official docked")
CPU_VMIN_MV = 620                # CPUB01_CVB_TABLE vmin_coefficients {620000, 0, 0}
CPU_COLD_FLOOR_MV = 800          # tegra210b01_thermal_floor_table: below 20 C
CPU_CAPS_MV = (1060, 1010)       # tegra210b01_thermal_cap_table: from 64 C, from 84 C

# L4T CTCaer/switch-l4t-kernel-4.9 clk-tegra124-dfll-fcpu.c CPUB01_CVB_TABLE (the row every T210B01 speedo id 0/3
# takes): kHz, c0, c1, c2; speedo scale 100, voltage scale 1000.
CPUB01_CVB = [
    (204000, 721589, -12695, 27), (306000, 747134, -14195, 27), (408000, 776324, -15705, 27),
    (510000, 809160, -17205, 27), (612000, 845641, -18715, 27), (714000, 885768, -20215, 27),
    (816000, 929540, -21725, 27), (918000, 976958, -23225, 27), (1020000, 1028021, -24725, 27),
    (1122000, 1082730, -26235, 27), (1224000, 1141084, -27735, 27), (1326000, 1203084, -29245, 27),
    (1428000, 1268729, -30745, 27), (1581000, 1374032, -33005, 27), (1683000, 1448791, -34505, 27),
    (1785000, 1527196, -36015, 27), (1887000, 1609246, -37515, 27), (1963500, 1675751, -38635, 27),
    (2014500, 1716501, -39395, 27), (2091000, 1716501, -39395, 27), (2193000, 1775132, -40505, 27),
    (2295000, 1866287, -42005, 27),
]
# TegraGM20B platform.c gpu_b01_cvb (L4T tegra210-dvfs.c GPUB01_NA_CVB_TABLE "NAPLL En - p4v3") and its trips.
GPUB01_NA = [
    (76800, 610000, 0, 0, 0, 0, 0), (153600, 610000, 0, 0, 0, 0, 0), (230400, 610000, 0, 0, 0, 0, 0),
    (307200, 610000, 0, 0, 0, 0, 0), (384000, 610000, 0, 0, 0, 0, 0), (460800, 610000, 0, 0, 0, 0, 0),
    (537600, 801688, -10900, -163, 298, -10599, 162), (614400, 824214, -5743, -452, 238, -6325, 81),
    (691200, 848830, -3903, -552, 119, -4030, -2), (768000, 891575, -4409, -584, 0, -2849, 39),
    (844800, 940071, -5367, -602, -60, -63, -93), (921600, 986765, -6637, -614, -179, 1905, -13),
]
GPU_TRIPS_C = (-25, 20, 30, 50, 70, 90)


def c_div(a, b):
    """C's truncating integer division."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def div_round_closest(x, d):
    """Linux DIV_ROUND_CLOSEST for a positive divisor."""
    return c_div(x + d // 2, d) if x > 0 else c_div(x - d // 2, d)


def cpu_rows(speedo):
    """cvb.c build_opp_table for CPUB01_CVB_TABLE: CVB voltage rounded up to the MAX77812's 5 mV step, clamped to
    [the CVB Vmin, the table's max_millivolts]."""
    rows = []
    for khz, c0, c1, c2 in CPUB01_CVB:
        uv = div_round_closest(c2 * speedo, 100)
        uv = div_round_closest((uv + c1) * speedo, 100) + c0
        mv = -(-uv // 5000) * 5
        rows.append((khz, min(max(mv, CPU_VMIN_MV), 1235)))
    return rows


def gpu_na_uv(khz, speedo):
    """TegraGM20B platform.c tegra_gm20b_dvfs_na_uv: worst over the trips, 5 mV step, 610 mV floor."""
    row = next(r for r in GPUB01_NA if r[0] == khz)
    worst = 610000
    for t in GPU_TRIPS_C:
        uv = div_round_closest(row[3] * speedo, 100)
        uv = div_round_closest((uv + row[2]) * speedo, 100) + row[1]
        tv = div_round_closest(row[4] * speedo, 100) + row[5] + div_round_closest(row[6] * t, 10)
        uv += div_round_closest(tv * t, 10)
        uv = -(-uv // 5000) * 5000 if uv > 0 else 0
        worst = max(worst, uv)
    return worst


def constant(path, name):
    """A constexpr tunable from a kernel source, or None when the source does not have it (yet)."""
    match = path.is_file() and re.search(r'\b' + re.escape(name) + r'\s*=\s*([0-9]+)', path.read_text())
    return int(match.group(1)) if match else None


def define(path, name):
    match = path.is_file() and re.search(r'#define\s+' + re.escape(name) + r'\s+\(?([0-9]+)', path.read_text())
    return int(match.group(1)) if match else None


MS = 1000  # the tests' time unit is the microsecond


@unittest.skipUnless((POWER / 'SwitchGovernor.h').is_file(), 'needs drivers/SwitchPower/SwitchGovernor.h')
class SwitchGovernorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='ioswitch-governor-')
        library = Path(cls.directory.name) / 'libgovernor.dylib'
        subprocess.run(['clang', *FLAGS, '-shared', '-fPIC', str(HARNESS), '-o', str(library)], check=True)
        lib = cls.lib = ctypes.CDLL(str(library))
        u32, u64, uint, ptr = ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint, ctypes.c_void_p
        lib.gov_size.restype = ctypes.c_size_t
        lib.gov_init.argtypes = [ptr, ctypes.POINTER(u32), uint, uint, u32, u32, u64, u64]
        lib.gov_init.restype = None
        lib.gov_step.argtypes = [ptr, u64, u32]
        lib.gov_step.restype = uint
        lib.gov_boost.argtypes = [ptr, uint, u64, u64]
        lib.gov_boost.restype = uint
        lib.gov_limit.argtypes = [ptr, uint, uint, u64]
        lib.gov_limit.restype = uint
        for name in ('gov_cur', 'gov_lo', 'gov_hi'):
            getattr(lib, name).argtypes = [ptr]
            getattr(lib, name).restype = uint
        lib.gov_khz.argtypes = [ptr, uint]
        lib.gov_khz.restype = u32
        lib.gov_residency.argtypes = [ptr, uint]
        lib.gov_residency.restype = u64
        lib.gov_account.argtypes = [ptr, u64]
        lib.gov_account.restype = None
        lib.gov_counter.argtypes = [ptr, uint]
        lib.gov_counter.restype = u32
        lib.gov_run.argtypes = [ptr, ctypes.POINTER(u64), ctypes.POINTER(u32), uint, ctypes.POINTER(uint)]
        lib.gov_run.restype = None
        lib.gov_cpu_table.argtypes = [ctypes.POINTER(u32), ctypes.POINTER(u32), uint, u32, u32, ctypes.POINTER(u32),
                                      ctypes.POINTER(u32), uint]
        lib.gov_cpu_table.restype = uint
        lib.gov_index_at_mv.argtypes = [ctypes.POINTER(u32), uint, u32]
        lib.gov_index_at_mv.restype = uint
        lib.gov_gpu_uv.argtypes = [ctypes.c_int32, u32, ctypes.c_int32, ctypes.c_int32, ctypes.c_int]
        lib.gov_gpu_uv.restype = ctypes.c_int32

        # The kernel's tunables, as shipped (the governor's state machine is checked with them; without the kernel
        # sources that carry them, the traces run on ondemand-like stand-ins and the GPU checks are skipped).
        cpu = POWER / 'SwitchPerfGovernor.cpp'
        cls.cpu_tuned = constant(cpu, 'kCpuWindowUs') is not None
        cls.cpu_window = constant(cpu, 'kCpuWindowUs') or 8000
        cls.cpu_up = constant(cpu, 'kCpuUpPermille') or 900
        cls.cpu_target = constant(cpu, 'kCpuTargetPermille') or 750
        cls.cpu_hold = constant(cpu, 'kCpuDownHoldUs') or 80000
        cls.touch_hold = constant(cpu, 'kBoostTouchUs') or 600000
        gpu = TREE / 'drivers/TegraGM20B/gpu.c'
        cls.gpu_window = define(gpu, 'GOV_WINDOW_US')
        cls.gpu_up = define(gpu, 'GOV_UP_PERMILLE')
        cls.gpu_target = define(gpu, 'GOV_TARGET_PERMILLE')
        cls.gpu_hold = define(gpu, 'GOV_DOWN_HOLD_US')
        cls.gpu_margin = define(gpu, 'FIXED_PLL_MARGIN_PCT')

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    # ---------------------------------------------------------------------------------------------- helpers
    def cpu_table(self, speedo=1659, vmin=CPU_VMIN_MV):
        rows = cpu_rows(speedo)
        n = len(rows)
        khz = (ctypes.c_uint32 * n)(*[r[0] for r in rows])
        mv = (ctypes.c_uint32 * n)(*[r[1] for r in rows])
        out_khz = (ctypes.c_uint32 * 24)()
        out_mv = (ctypes.c_uint32 * 24)()
        count = self.lib.gov_cpu_table(khz, mv, n, NINTENDO_CPU_MAX_KHZ, vmin, out_khz, out_mv, 24)
        return list(out_khz[:count]), list(out_mv[:count])

    def governor(self, khz, start=0, up=None, target=None, hold=None, now=0):
        g = ctypes.create_string_buffer(self.lib.gov_size())
        table = (ctypes.c_uint32 * len(khz))(*khz)
        self.lib.gov_init(g, table, len(khz), start, self.cpu_up if up is None else up,
                          self.cpu_target if target is None else target, self.cpu_hold if hold is None else hold, now)
        return g

    def cpu_governor(self, start=0, now=0):
        khz, _ = self.cpu_table()
        return self.governor(khz, start=start, now=now), khz

    def trace(self, g, busy, start=0, window=None):
        """busy: one permille per window; returns the index after each window."""
        window = window or self.cpu_window
        n = len(busy)
        times = (ctypes.c_uint64 * n)(*[start + (i + 1) * window for i in range(n)])
        loads = (ctypes.c_uint32 * n)(*busy)
        out = (ctypes.c_uint * n)()
        self.lib.gov_run(g, times, loads, n, out)
        return list(out)

    # ---------------------------------------------------------------------------------------------- tables
    def test_cpu_table_for_this_chip(self):
        """CPU speedo 1659 (the device's SwitchPerf line, 11-05): from the Vmin row (918 MHz, the DVCO's lowest rate
        without the skipper) to Horizon's 1785 MHz; 1683 MHz at 885 mV as SwitchPerf logged from the same CVB row."""
        khz, mv = self.cpu_table(1659)
        self.assertEqual(khz, [918000, 1020000, 1122000, 1224000, 1326000, 1428000, 1581000, 1683000, 1785000])
        self.assertEqual(mv[0], CPU_VMIN_MV)
        self.assertEqual(dict(zip(khz, mv))[1683000], 885)
        self.assertEqual(dict(zip(khz, mv))[1785000], 940)
        self.assertEqual(mv, sorted(mv))

    def test_cpu_table_never_above_horizon_boost(self):
        for speedo in range(1400, 1951, 10):
            khz, mv = self.cpu_table(speedo)
            self.assertTrue(khz, speedo)
            self.assertLessEqual(max(khz), NINTENDO_CPU_MAX_KHZ, speedo)
            self.assertEqual(max(khz), NINTENDO_CPU_MAX_KHZ, speedo)  # every speedo's CVB reaches it under 1235 mV
            self.assertTrue(all(k % 25500 == 0 for k in khz), speedo)  # whole DFLL steps (51 MHz ref / 2)
            for k, v in zip(khz, mv):  # each pair is a CVB row, voltage and all
                self.assertIn((k, v), cpu_rows(speedo))

    def test_thermal_cap_ceiling(self):
        """L4T's caps (1060 mV from 64 C, 1010 mV from 84 C) never bind on this chip (1785 MHz needs 940 mV); on a
        slow chip (speedo 1450: 1785 MHz needs 1015 mV) the 84 C cap takes the ceiling to 1683 MHz."""
        for speedo, ceilings in ((1659, (1785000, 1785000)), (1450, (1785000, 1683000))):
            khz, mv = self.cpu_table(speedo)
            table = (ctypes.c_uint32 * len(mv))(*mv)
            got = tuple(self.lib.gov_index_at_mv(table, len(mv), cap) for cap in CPU_CAPS_MV)
            self.assertEqual(tuple(khz[i] for i in got), ceilings, speedo)
            for cap, index in zip(CPU_CAPS_MV, got):
                self.assertLessEqual(mv[index], cap)
                if index + 1 < len(mv):
                    self.assertGreater(mv[index + 1], cap)

    def test_cold_floor_index(self):
        """Below 20 C the DFLL keeps VDD_CPU at 800 mV or more: the governor's floor is the highest row at or below
        it (1428 MHz on this chip), so the skipper never engages."""
        khz, mv = self.cpu_table(1659)
        table = (ctypes.c_uint32 * len(mv))(*mv)
        self.assertEqual(khz[self.lib.gov_index_at_mv(table, len(mv), CPU_COLD_FLOOR_MV)], 1428000)

    def test_gpu_voltages(self):
        """GPU speedo 1673: NA table + the fixed PLL's 20 % margin, on the 5 mV step, inside [610, 800] mV; every
        Horizon rate up to 768 MHz fits, 844.8 MHz does not (and is above Horizon's maximum anyway); cold: 800 mV."""
        got = {}
        for row in GPUB01_NA:
            uv = self.lib.gov_gpu_uv(gpu_na_uv(row[0], 1673), self.gpu_margin, 610000, 800000, 0)
            got[row[0]] = uv
            if uv > 0:
                self.assertGreaterEqual(uv, gpu_na_uv(row[0], 1673))
                self.assertEqual(uv % 5000, 0)
                self.assertEqual(self.lib.gov_gpu_uv(gpu_na_uv(row[0], 1673), self.gpu_margin, 610000, 800000, 1),
                                 800000)
        self.assertEqual(got[76800], 735000)
        self.assertEqual(got[460800], 735000)
        self.assertEqual(got[768000], 800000)
        self.assertEqual(got[844800], -1)
        self.assertEqual(max(k for k, v in got.items() if v > 0), NINTENDO_GPU_MAX_KHZ)

    # ---------------------------------------------------------------------------------------------- traces
    def test_saturation_reaches_the_ceiling_in_one_window(self):
        g, khz = self.cpu_governor(start=0)
        self.assertEqual(self.trace(g, [1000])[0], len(khz) - 1)
        self.assertEqual(self.lib.gov_counter(g, 2), 1)  # one jump

    def test_burst_from_idle_reaches_the_top_within_a_frame(self):
        """A burst that starts mid-window (half busy, then saturated) is at 1785 MHz after the second window: two
        CPU windows are under one 60 Hz frame."""
        g, khz = self.cpu_governor(start=0)
        out = self.trace(g, [0, 0, 500, 1000])
        self.assertEqual(khz[out[3]], NINTENDO_CPU_MAX_KHZ)
        self.assertLessEqual(2 * self.cpu_window, 16667)

    def test_idle_drops_only_after_the_hold(self):
        g, khz = self.cpu_governor(start=len(self.cpu_table()[0]) - 1)
        windows = self.cpu_hold // self.cpu_window + 3
        out = self.trace(g, [0] * windows)
        first_low = next(i for i, x in enumerate(out) if x != len(khz) - 1)
        # the targets first fall at window 0's end; the clock follows at the first window that ends a hold later
        windows_in_hold = -(-self.cpu_hold // self.cpu_window)
        self.assertEqual(first_low, windows_in_hold)
        self.assertEqual(out[-1], 0)
        self.assertEqual(self.lib.gov_counter(g, 1), 1)  # one step down, straight to the lowest point

    def test_short_dips_keep_the_clock(self):
        """Busy and idle alternating faster than the hold: the clock never comes down."""
        g, khz = self.cpu_governor(start=0)
        dip = max(1, self.cpu_hold // self.cpu_window - 1)
        trace = ([1000] * 3 + [0] * dip) * 20
        out = self.trace(g, trace)
        self.assertTrue(all(x == len(khz) - 1 for x in out[0:]), out)
        self.assertEqual(self.lib.gov_counter(g, 1), 0)

    def test_hold_drops_to_the_highest_target_it_saw(self):
        g, khz = self.cpu_governor(start=0)
        top = len(khz) - 1
        self.trace(g, [1000])
        # at 1785 MHz: 40 % busy wants 1020 MHz (952 / 0.75), 55 % wants 1326 MHz (1309 / 0.75), 10 % the floor
        loads = [400, 550, 100]
        windows = self.cpu_hold // self.cpu_window + 1
        out = self.trace(g, (loads * windows)[:windows], start=self.cpu_window)
        self.assertEqual(out[-1], khz.index(1326000), [khz[i] for i in out])
        self.assertTrue(all(x == top for x in out[:-1]))

    def test_proportional_point(self):
        """Half busy at 1785 MHz is 892 MHz of work: the lowest point that covers it at the target load."""
        g, khz = self.cpu_governor(start=len(self.cpu_table()[0]) - 1)
        out = self.trace(g, [500] * (self.cpu_hold // self.cpu_window + 2))
        need = 1785000 * 500 / self.cpu_target
        expect = min(k for k in khz if k >= need)
        self.assertEqual(khz[out[-1]], expect)

    def test_touch_boost_is_immediate_and_expires(self):
        g, khz = self.cpu_governor(start=0)
        top = len(khz) - 1
        now = 10 * MS
        self.assertEqual(self.lib.gov_boost(g, top, now + self.touch_hold, now), top)
        idle = (self.touch_hold + self.cpu_hold) // self.cpu_window + 2
        out = self.trace(g, [0] * idle, start=now)
        times = [now + (i + 1) * self.cpu_window for i in range(idle)]
        for t, x in zip(times, out):
            if t < now + self.touch_hold:
                self.assertEqual(x, top, t)  # held for the whole boost, idle or not
        self.assertEqual(out[-1], 0)
        last_top = max(t for t, x in zip(times, out) if x == top)
        self.assertGreaterEqual(last_top, now + self.touch_hold)

    def test_boosts_combine(self):
        g, khz = self.cpu_governor(start=0)
        self.lib.gov_boost(g, 3, 100 * MS, 0)
        self.lib.gov_boost(g, 6, 50 * MS, 1)
        self.assertEqual(self.lib.gov_cur(g), 6)
        out = self.trace(g, [0] * 30)  # every window idle: the floor is 6 until 100 ms, then the hold
        times = [(i + 1) * self.cpu_window for i in range(30)]
        self.assertTrue(all(x >= 6 for t, x in zip(times, out) if t < 100 * MS))

    def test_thermal_ceiling_clamps_at_once_and_lifts(self):
        g, khz = self.cpu_governor(start=0)
        top = len(khz) - 1
        self.trace(g, [1000])
        self.assertEqual(self.lib.gov_limit(g, 0, top - 1, 9 * MS), top - 1)  # down at once, no hold
        self.assertEqual(self.trace(g, [1000] * 5, start=9 * MS), [top - 1] * 5)
        self.lib.gov_limit(g, 0, top, 60 * MS)
        self.assertEqual(self.trace(g, [1000], start=60 * MS)[0], top)
        self.assertEqual(self.lib.gov_counter(g, 4), 1)

    def test_cold_floor_raises_at_once(self):
        g, khz = self.cpu_governor(start=0)
        floor = khz.index(1428000)
        self.assertEqual(self.lib.gov_limit(g, floor, len(khz) - 1, 1), floor)
        self.assertTrue(all(x >= floor for x in self.trace(g, [0] * 40)))

    def test_flapping_load_is_rate_limited(self):
        """A load alternating every window between saturation and idle moves the clock at most once per hold."""
        g, khz = self.cpu_governor(start=0)
        windows = 2000
        self.trace(g, [1000 if i % 2 == 0 else 0 for i in range(windows)])
        elapsed = windows * self.cpu_window
        self.assertLessEqual(self.lib.gov_counter(g, 1), elapsed // self.cpu_hold + 1)

    def test_residency_adds_up(self):
        g, khz = self.cpu_governor(start=0)
        random.seed(5)
        n = 500
        self.trace(g, [random.randrange(0, 1001) for _ in range(n)])
        total = sum(self.lib.gov_residency(g, i) for i in range(len(khz)))
        self.assertEqual(total, n * self.cpu_window)

    def test_fuzz_limits_and_responsiveness(self):
        random.seed(1105)
        khz, _ = self.cpu_table()
        top = len(khz) - 1
        for trial in range(60):
            g = self.governor(khz, start=random.randrange(len(khz)))
            lo, hi, now = 0, top, 0
            for _ in range(400):
                now += self.cpu_window
                roll = random.random()
                if roll < 0.03:
                    hi = random.randrange(len(khz))
                    lo = random.randrange(hi + 1)
                    self.lib.gov_limit(g, lo, hi, now)
                elif roll < 0.08:
                    self.lib.gov_boost(g, random.randrange(len(khz)), now + random.randrange(0, 2000) * MS, now)
                busy = random.choice((0, 0, 100, 300, 600, 899, 900, 1000, random.randrange(1001)))
                cur = self.lib.gov_step(g, now, busy)
                self.assertGreaterEqual(cur, self.lib.gov_lo(g))
                self.assertLessEqual(cur, self.lib.gov_hi(g))
                self.assertLessEqual(self.lib.gov_khz(g, cur), NINTENDO_CPU_MAX_KHZ)
                if busy >= self.cpu_up:
                    self.assertEqual(cur, self.lib.gov_hi(g))

    def test_gpu_tunables_reach_the_top_within_two_frames(self):
        if self.gpu_window is None:
            self.skipTest('TegraGM20B gpu.c has no governor')
        rates = [r[0] for r in GPUB01_NA if r[0] <= NINTENDO_GPU_MAX_KHZ]
        g = self.governor(rates, start=0, up=self.gpu_up, target=self.gpu_target, hold=self.gpu_hold)
        out = self.trace(g, [1000], window=self.gpu_window)
        self.assertEqual(rates[out[0]], NINTENDO_GPU_MAX_KHZ)
        self.assertLessEqual(self.gpu_window, 2 * 16667)
        out = self.trace(g, [0] * (self.gpu_hold // self.gpu_window + 2), start=self.gpu_window, window=self.gpu_window)
        self.assertEqual(rates[out[-1]], 76800)

    def test_tunables_are_sane(self):
        if not self.cpu_tuned:
            self.skipTest('no SwitchPerfGovernor.cpp')
        self.assertLess(self.cpu_window * 2, 16667)          # up within a frame
        self.assertGreaterEqual(self.cpu_hold, 4 * self.cpu_window)
        self.assertLess(self.cpu_target, self.cpu_up)
        self.assertGreater(self.touch_hold, self.cpu_hold)


if __name__ == '__main__':
    unittest.main()
