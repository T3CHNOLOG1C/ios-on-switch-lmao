"""thermalmonitord's sensors on the Switch (drivers/SwitchPower/SwitchSensors.h, CONTRACT.md next to it).

Checks, against what iPadOS 21H461 itself has (recorded below from /System/Library/ThermalMonitor/J120AP-Info.plist and
the thermalmonitord binary, ThermalMonitor-1952.120.7):
  * the sensor table is the plist's non-synth Sensors entries, same order, 4CCs and matchNames, so the watchdog mask of
    those indices is the 0x3fffffffff thermalmonitord logged as "expected";
  * every mapping is a real measurement of the nearest point: CPU-complex sensors from SOC_THERM's per-core tsensors
    (core n from cpu n), SoC die sensors from on-die points only, battery sensors from the battery thermistor, board
    thermistors from the PCB channel, tcal from the identity calibration;
  * unit conversions (SOC_THERM readback, MAX17050 TEMP, IOFixed, thermalmonitord's centi-degree validity window), the
    MAX17050 alert window, the HID shapes and vtable slots the kernel side relies on;
  * SOC_THERM's T210B01 fuse calibration against an independent model of L4T's tsensor-fuse.c;
  * CLTM's CPU ceiling: the DVD factor keys, the lowest-contributor rule and the governor index it maps to."""
import ctypes
import random
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TREE = ROOT / '.'
POWER = TREE / 'drivers/SwitchPower'
HARNESS = ROOT / 'tests/switch_thermal_sensors_harness.c'
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wsign-conversion', '-I', str(POWER)]

# J120AP-Info.plist "Sensors", 21H461: every entry without synthSensor (the 5 synth ones, SS0F SS0C TSBH TS0A TSFD,
# have requiresWatchDog false and are computed by thermalmonitord itself).
J120AP_HID_SENSORS = [
    ('TG0B', 'gas gauge battery'), ('TP1d', 'PMU tdev1'), ('TP2d', 'PMU tdev2'), ('TP3d', 'PMU tdev3'),
    ('TP4d', 'PMU tdev4'), ('TP5d', 'PMU tdev5'), ('TP6d', 'PMU tdev6'), ('TP7d', 'PMU tdev7'), ('TP8d', 'PMU tdev8'),
    ('TR1d', 'PMU2 tdev1'), ('TR2d', 'PMU2 tdev2'), ('TP0Z', 'PMU tcal'), ('TP0j', 'PMU tjunc'),
    ('TQ0B', 'Charger tbat'),
] + [(f'Th{n}{k}', f'{p}PMGR SOC Die Temp Sensor{n}') for n in range(4)
     for k, p in (('i', ''), ('a', 'Avg: '), ('x', 'Max: '))] + [
    (f'Tc{n}{k}', f'{p}ACC Temp Sensor{n}') for n in range(4) for k, p in (('i', ''), ('a', 'Avg: '), ('x', 'Max: '))]
EXPECTED_MASK = 0x3fffffffff  # "is_alive_func returned unhealthy : current 0, mask 3fffffffff, expected 3fffffffff"

# SwitchSensors.h switch_sensor_source
SOC_CPU = [0, 1, 2, 3]
SOC_GPU, SOC_PLLX, TMP451_SOC, TMP451_PCB, BATTERY, TCAL = 4, 5, 6, 7, 8, 9
DIE_POINTS = {SOC_GPU, SOC_PLLX, TMP451_SOC}

# MitigationController's CPU keys (thermalmonitord __DATA_CONST 0x10007dbd8..0x10007dbf0, CFStrings).
CLTM_KEYS = ['AppleARMPerformanceControllerDVDFactor1-slowloop', 'AppleARMPerformanceControllerDVDFactor1-hip',
             'AppleARMPerformanceControllerDVDFactor1-upo', 'AppleARMPerformanceControllerDVDFactor1-lpm']

# L4T tegra210-soctherm.c tegra210b01_tsensors / tegra210b01_tsensor_config.
B01_TSENSORS = [('cpu0', 0xc0, 0x098, 1085000, 3244200), ('cpu1', 0xe0, 0x084, 1126200, -67500),
                ('cpu2', 0x100, 0x088, 1098400, 2251100), ('cpu3', 0x120, 0x12c, 1108000, 602700),
                ('gpu', 0x180, 0x154, 1074300, 2734900), ('pllx', 0x1a0, 0x160, 1039700, 6829100)]
B01 = dict(tall=16300, tiddq_en=1, ten_count=1, tsample=240, tsample_ate=480, pdiv=12, pdiv_ate=6)


def s32(x):
    return ((x + (1 << 31)) & 0xffffffff) - (1 << 31)


def s16(x):
    return ((x + 0x8000) & 0xffff) - 0x8000


def sign_extend32(value, index):
    shift = 31 - index
    return s32((value << shift) & 0xffffffff) >> shift


def c_div(a, b):
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def div64_s64_precise(a, b):
    """tsensor-fuse.c: s64 a, s32 b."""
    b = s32(b)
    return c_div((a << 16) * 2 + 1, 2 * b) >> 16


def l4t_calib(common, fuse, alpha, beta):
    """tegra_calc_shared_calib + tegra_calc_tsensor_calib, tegra210_soctherm_fuse, T210B01 config."""
    base_cp = (common & (0x3ff << 11)) >> 11
    base_ft = (common & (0x7ff << 21)) >> 21
    shifted_ft = sign_extend32((common & (0x1f << 6)) >> 6, 4)
    shifted_cp = sign_extend32(common, 5)
    actual_cp, actual_ft = 2 * 25 + shifted_cp, 2 * 105 + shifted_ft
    cp = base_cp * 64 + sign_extend32(fuse, 12)
    ft = base_ft * 32 + sign_extend32((fuse & (0x1fff << 13)) >> 13, 12)
    delta_sens, delta_temp = ft - cp, actual_ft - actual_cp
    if delta_sens == 0:
        return None
    mult, div = B01['pdiv'] * B01['tsample_ate'], B01['tsample'] * B01['pdiv_ate']
    therma = s16(div64_s64_precise(delta_temp * (1 << 13) * mult, delta_sens * div))
    thermb = s16(div64_s64_precise(ft * actual_cp - cp * actual_ft, delta_sens))
    therma = s16(div64_s64_precise(therma * alpha, 1000000))
    thermb = s16(div64_s64_precise(thermb * alpha + beta, 1000000))
    return ((therma & 0xffff) << 16) | (thermb & 0xffff)


class SwitchThermalSensors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        library = Path(cls.tmp.name) / 'libsensors.dylib'
        subprocess.run(['clang', *FLAGS, '-shared', '-fPIC', str(HARNESS), '-o', str(library)], check=True)
        lib = cls.lib = ctypes.CDLL(str(library))
        for name in ('sensor_fourcc', 'sensor_name', 'tsensor_name', 'cltm_key'):
            getattr(lib, name).restype = ctypes.c_char_p
        for name in ('sensors_count', 'sensor_fourcc_value', 'max17050_talrt', 'tsensor_calib', 'tsensor_base',
                     'tsensor_fuse', 'tsensor_config0', 'tsensor_config1', 'index_at_fraction', 'cltm_ceiling'):
            getattr(lib, name).restype = ctypes.c_uint32
        lib.tsensor_calib.argtypes = [ctypes.c_uint32] * 3
        lib.cltm_ceiling.argtypes = [ctypes.c_uint32] * 4
        lib.index_at_fraction.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint, ctypes.c_uint32]
        lib.max17050_millic.argtypes = [ctypes.c_uint]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def sensors(self):
        lib = self.lib
        return [(lib.sensor_fourcc(i).decode(), lib.sensor_name(i).decode(), lib.sensor_source(i))
                for i in range(lib.sensors_count())]

    # ---- the contract with thermalmonitord ---------------------------------------------------------------------------
    def test_table_is_the_plist(self):
        table = self.sensors()
        self.assertEqual([(f, n) for f, n, _ in table], J120AP_HID_SENSORS)
        self.assertEqual((1 << len(table)) - 1, EXPECTED_MASK)  # requiresWatchDog defaults to true: one bit each
        self.assertEqual(len({n for _, n, _ in table}), len(table))  # Product is the key: all distinct
        for i, (f, _, _) in enumerate(table):
            self.assertEqual(self.lib.sensor_fourcc_value(i), int.from_bytes(f.encode(), 'big'))  # LocationID match

    def test_every_value_is_the_nearest_measurement(self):
        for fourcc, name, source in self.sensors():
            with self.subTest(fourcc=fourcc):
                if name.endswith(tuple(f'ACC Temp Sensor{n}' for n in range(4))):
                    self.assertEqual(source, SOC_CPU[int(name[-1])])  # core n's own tsensor
                elif 'PMGR SOC Die Temp Sensor' in name:
                    self.assertIn(source, DIE_POINTS)  # on the die, outside the CPU cores
                elif name in ('gas gauge battery', 'Charger tbat'):
                    self.assertEqual(source, BATTERY)  # the pack's thermistor
                elif name == 'PMU tcal':
                    self.assertEqual(source, TCAL)
                elif re.fullmatch(r'PMU2? (tdev\d|tjunc)', name):
                    self.assertEqual(source, TMP451_PCB)  # board thermistors: the PCB
                else:
                    self.fail(f'unmapped {fourcc} {name}')
        # i/a/x of one PMGR/ACC sensor always read the same point.
        groups = {}
        for fourcc, _, source in self.sensors():
            if fourcc[:2] in ('Th', 'Tc'):
                groups.setdefault(fourcc[:3], set()).add(source)
        self.assertTrue(all(len(s) == 1 for s in groups.values()), groups)
        # Each of the six SOC_THERM sensors and both TMP451 channels feed something.
        self.assertTrue(set(range(10)) <= {s for _, _, s in self.sensors()})

    def test_cltm_units(self):
        lib = self.lib
        for milli in (45500, 45499, 0, -1250, 37812, 100000, -39999, 149999):
            fixed = lib.fixed_of(milli)
            self.assertAlmostEqual(fixed / 65536, milli / 1000, delta=1 / 65536)
            self.assertEqual(lib.cltm_centi(fixed), int(fixed / 65536 * 100))  # (int)(float * 100.0)
        self.assertEqual(lib.cltm_centi(lib.fixed_of(45500)), 4550)
        # thermalmonitord's window: -40 C < t < 150 C (centi-degrees).
        self.assertTrue(lib.cltm_valid(-3999))
        self.assertFalse(lib.cltm_valid(-4000))
        self.assertTrue(lib.cltm_valid(14999))
        self.assertFalse(lib.cltm_valid(15000))
        self.assertTrue(lib.cltm_valid(0))
        # Whatever the kernel hands over passes it.
        for milli in range(-39999, 150000, 37):
            if lib.plausible(milli):
                self.assertTrue(lib.cltm_valid(lib.cltm_centi(lib.fixed_of(milli))), milli)
        self.assertFalse(lib.plausible(-40000))
        self.assertFalse(lib.plausible(150000))

    def test_soctherm_readback(self):
        lib = self.lib
        self.assertEqual(lib.soctherm_millic(0x2d80), 45500)  # 45 + half
        self.assertEqual(lib.soctherm_millic(0x2d00), 45000)
        self.assertEqual(lib.soctherm_millic(0x0581), -5500)  # negate bit
        self.assertEqual(lib.soctherm_millic(0x0000), 0)
        for raw in range(0, 0x10000, 7):
            t = ((raw & 0xff00) >> 8) * 1000 + (500 if raw & 0x80 else 0)
            self.assertEqual(lib.soctherm_millic(raw), -t if raw & 1 else t)

    def test_max17050(self):
        lib = self.lib
        self.assertEqual(lib.max17050_millic(0x1f80), 31500)
        self.assertEqual(lib.max17050_millic(0xff00), -1000)
        self.assertEqual(lib.max17050_millic(0x2560), 37375)
        for milli in range(-30000, 80000, 113):
            talrt = lib.max17050_talrt(milli)
            hi = ctypes.c_int8(talrt >> 8).value
            lo = ctypes.c_int8(talrt & 0xff).value
            self.assertLess(lo * 1000, milli)  # the reading never violates its own window
            self.assertGreater(hi * 1000, milli)
            self.assertLessEqual(hi * 1000 - milli, 1500)  # about a degree either way
            self.assertLessEqual(milli - lo * 1000, 1500)

    # ---- SOC_THERM ---------------------------------------------------------------------------------------------------
    def test_tsensor_layout(self):
        lib = self.lib
        for i, (name, base, fuse, _, _) in enumerate(B01_TSENSORS):
            self.assertEqual(lib.tsensor_name(i).decode(), name)
            self.assertEqual(lib.tsensor_base(i), base)
            self.assertEqual(lib.tsensor_fuse(i), fuse)
        self.assertEqual(lib.tsensor_config0(), B01['tall'] << 8)
        self.assertEqual(lib.tsensor_config1(), (B01['tsample'] - 1) | B01['tiddq_en'] << 15 | B01['ten_count'] << 24 |
                         1 << 31)

    def test_fuse_calibration_matches_l4t(self):
        rng = random.Random(1952)
        cases = [(0x0f8c3c41, 0x01d0e9d3)] + [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(4000)]
        checked = 0
        for common, fuse in cases:
            for i, (_, _, _, alpha, beta) in enumerate(B01_TSENSORS):
                want = l4t_calib(common, fuse, alpha, beta)
                got = self.lib.tsensor_calib(common, fuse, i)
                if want is None:
                    self.assertEqual(got, 0)
                    continue
                self.assertEqual(got, want, (hex(common), hex(fuse), i))
                checked += 1
        self.assertGreater(checked, 20000)

    # ---- mitigation --------------------------------------------------------------------------------------------------
    def test_cltm_ceiling(self):
        lib = self.lib
        self.assertEqual([lib.cltm_key(i).decode() for i in range(4)], CLTM_KEYS)
        self.assertEqual(lib.cltm_ceiling(0x10000, 0x10000, 0x10000, 0x10000), 0x10000)
        self.assertEqual(lib.cltm_ceiling(0x10000, 24248, 0x10000, 0x10000), 24248)
        self.assertEqual(lib.cltm_ceiling(30000, 24248, 0, 0x10000), 0)
        # The governor's DFLL points (918..1785 MHz, SwitchGovernor.h table on speedo 1659) and CLTM's percentages.
        khz = [918000, 1020000, 1122000, 1224000, 1326000, 1428000, 1581000, 1683000, 1785000]
        arr = (ctypes.c_uint32 * len(khz))(*khz)

        def index(percent):
            fraction = percent * 65536 // 100 if percent < 100 else 0x10000  # MitigationController scaleToFixedPoint
            return lib.index_at_fraction(arr, len(khz), fraction)

        self.assertEqual(index(101), len(khz) - 1)  # no limit
        self.assertEqual(index(100), len(khz) - 1)
        self.assertEqual(index(0), 0)               # heavy pressure: the lowest point, never off
        self.assertEqual(khz[index(70)], 1224000)    # 1249.5 MHz allowed
        self.assertEqual(khz[index(96)], 1683000)    # 1713.6 MHz allowed
        last = 0
        for percent in range(0, 102):
            i = index(percent)
            limit = (khz[-1] * (percent * 65536 // 100 if percent < 100 else 0x10000)) >> 16
            self.assertGreaterEqual(i, last)
            self.assertTrue(i == 0 or khz[i] <= limit, percent)
            self.assertTrue(i == len(khz) - 1 or khz[i + 1] > limit, percent)
            last = i

    # ---- the kernel side's shapes ------------------------------------------------------------------------------------
    def test_hid_shapes(self):
        hid = (TREE / 'drivers/SwitchTouch/SwitchHIDDevice.cpp').read_text()
        # Apple vendor 0xff00 / TemperatureSensor 0x0005: the pair thermalmonitord matches (createHIDEventSystemObject).
        self.assertIn('0x06, 0x00, 0xff, 0x09, 0x05, 0xa1, 0x01', hid)
        self.assertRegex(hid, r'0x5108, 0xff00, 0x05, "Switch Temperature Sensor"')
        # IOHIDEventService reserved 8..11 in IOHIDEventService.h order (2031.120.4): dispatchEvent 198 (proven by the
        # edge wrapper), getPrimaryUsagePage 199, getPrimaryUsage 200, copyEvent 201.
        self.assertIn('kDispatch = 198', hid)
        self.assertIn('kPrimaryUsagePage = 199, kPrimaryUsage = 200, kCopyEvent = 201', hid)
        # IOHIDEvent: setTimeStamp 17, getType 18, setType 19 (as SwitchALS), getFixedValue 25, setFixedValue 26.
        self.assertIn('kSetTimeStamp = 17, kGetType = 18, kSetType = 19, kGetFixed = 25, kSetFixed = 26', hid)
        self.assertIn('kType = 15, kFieldLevel = 0xf0000', hid)  # what IOHIDEventGetFloatValue(e, 0xf0000) reads
        als = (TREE / 'drivers/SwitchBrightness/SwitchALS.cpp').read_text()
        self.assertIn('kSetTimeStamp = 17, kSetType = 19, kSetIntegerValue = 24', als)


if __name__ == '__main__':
    unittest.main()
