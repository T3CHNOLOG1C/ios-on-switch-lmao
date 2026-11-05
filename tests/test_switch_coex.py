"""Wi-Fi/Bluetooth coexistence state on the host: drivers/SwitchBluetooth/SwitchCoex.h and
drivers/SwitchWLAN/SwitchWLANCoex.h, compiled and driven through ctypes.

The Bluetooth side is fed bluetoothd's commands as r7v (11-05 12:33-12:38) sent them: the LE scan's parameters as
bluetoothd moved them (30/300 ms, 30/40 ms, 30/30 ms), the page scan, and Broadcom's Set ACL Priority (0xFC57) around
the AirPods' A2DP start at 12:36:27. The Wi-Fi side gets wifid's Bluetooth flags (APPLE80211_IOC_BT_COEX_FLAGS bits,
from wifid 21H461) and sta_info replies laid out as bcmdhd's sta_info_t version 4."""
import ctypes
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ROOT / 'drivers'
HARNESS = ROOT / 'tests/switch_coex_harness.c'
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wsign-conversion', '-I',
         str(DRIVERS / 'SwitchBluetooth'), '-I', str(DRIVERS / 'SwitchWLAN')]

LE_PARAMS, LE_ENABLE, CLASSIC_SCAN, PRIORITY = 1, 2, 4, 8


def scan_params(interval, window, kind=0):
    return bytes([kind]) + struct.pack('<HH', interval, window) + b'\x00\x01'


def sta_info(ver=4, length=200, tx=0, failed=0, ucast=0, mcast=0, tx_rate=0, rx_rate=0, decrypt_failed=0, idle=0,
             retried=0, exhausted=0):
    """A sta_info_t as the firmware lays it out (natural alignment), the fields SwitchWLANCoex.h reads filled."""
    b = bytearray(200)
    struct.pack_into('<HH', b, 0, ver, length)
    struct.pack_into('<II', b, 8, 0x10, idle)  # flags (associated), idle
    struct.pack_into('<IIIIIIII', b, 52, tx, failed, ucast, mcast, tx_rate, rx_rate, 1000, decrypt_failed)
    struct.pack_into('<II', b, 160, retried, exhausted)
    return bytes(b)


class Coex(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='ioswitch-coex-')
        library = Path(cls.directory.name) / 'libcoex.dylib'
        subprocess.run(['clang', *FLAGS, '-shared', '-fPIC', str(HARNESS), '-o', str(library)], check=True)
        lib = cls.lib = ctypes.CDLL(str(library))
        lib.coex_command.argtypes = [ctypes.c_uint16, ctypes.c_char_p, ctypes.c_uint32]
        lib.coex_command.restype = ctypes.c_uint
        lib.coex_describe.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        lib.coex_state.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
        lib.bt_flags_describe.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
        lib.sta_parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        lib.sta_fields.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
        lib.sta_describe.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        lib.afh_map.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint)]
        lib.afh_map.restype = ctypes.c_uint
        lib.afh_used.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint)]
        lib.afh_used.restype = ctypes.c_uint
        lib.pm2_sleep_ret_ext.argtypes = [ctypes.c_char_p]
        lib.coex_pm.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.coex_pm.restype = ctypes.c_uint
        lib.btc_status_describe.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.btc_profile_describe.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.nvram_value.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint32)]
        lib.btc_profile_static.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        lib.coex_mode.argtypes = [ctypes.c_uint, ctypes.c_uint]
        lib.coex_mode.restype = ctypes.c_uint

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.lib.coex_reset()
        self.lib.sta_reset()

    def command(self, opcode, params):
        return self.lib.coex_command(opcode, bytes(params), len(params))

    def describe(self, size=200):
        out = ctypes.create_string_buffer(size)
        n = self.lib.coex_describe(out, size)
        self.assertLess(n, size)
        return out.value.decode()

    def state(self):
        out = (ctypes.c_uint32 * 8)()
        self.lib.coex_state(out)
        return tuple(out)

    def flags(self, value):
        out = ctypes.create_string_buffer(48)
        self.lib.bt_flags_describe(value, out, len(out))
        return out.value.decode()

    def sta(self, buffer):
        return self.lib.sta_parse(buffer, len(buffer))

    def sta_line(self):
        out = ctypes.create_string_buffer(240)
        self.lib.sta_describe(out, len(out))
        return out.value.decode()

    # Bluetooth: the LE scan
    def test_r7v_le_scan_duty_cycles(self):
        self.assertEqual(self.describe(), 'LE scan off, unset')
        # 12:33:33.848: LE Set Scan Parameters [00 e001 3000 00 01], then enable [01 01]
        self.assertEqual(self.command(0x200b, bytes.fromhex('00e00130000100')), LE_PARAMS)
        self.assertEqual(self.describe(), 'LE scan off (30/300 ms)')
        self.assertEqual(self.command(0x200c, b'\x01\x01'), LE_ENABLE)
        self.assertEqual(self.describe(), 'LE scan on 30/300 ms passive (10%)')
        # bluetoothd's 30/40 ms (12:34:14) and 30/30 ms (12:33:54, WirelessProximity type 8)
        self.assertEqual(self.command(0x200b, scan_params(64, 48)), LE_PARAMS)
        self.assertEqual(self.describe(), 'LE scan on 30/40 ms passive (75%)')
        self.assertEqual(self.command(0x200b, scan_params(48, 48, kind=1)), LE_PARAMS)
        self.assertEqual(self.describe(), 'LE scan on 30/30 ms active (100%)')
        # the same parameters again change nothing; off and on are LE_ENABLE only
        self.assertEqual(self.command(0x200b, scan_params(48, 48, kind=1)), 0)
        self.assertEqual(self.command(0x200c, b'\x00\x01'), LE_ENABLE)
        self.assertEqual(self.describe(), 'LE scan off (30/30 ms)')
        self.assertEqual(self.state()[7], 5)

    def test_extended_scan_and_short_commands(self):
        # own address, filter, PHYs (1M), then {type 0, interval 0x01e0, window 0x0030}
        self.assertEqual(self.command(0x2041, bytes.fromhex('00000100e0013000')), LE_PARAMS)
        self.assertEqual(self.command(0x2042, bytes.fromhex('010000000000')), LE_ENABLE)
        self.assertEqual(self.describe(), 'LE scan on 30/300 ms passive (10%)')
        self.assertEqual(self.command(0x200b, b'\x00\xe0\x01'), 0)  # too short: ignored
        self.assertEqual(self.command(0x0c03, b''), 0)  # not followed
        self.assertEqual(self.command(0xfc57, b'\x0b\x00'), 0)

    # Bluetooth: classic scans
    def test_page_and_inquiry_scan(self):
        # 12:33:26.741 Write Page Scan Activity interval 2048 window 18, then Write Scan Enable 0x02
        self.assertEqual(self.command(0x0c1c, struct.pack('<HH', 2048, 18)), CLASSIC_SCAN)
        self.assertEqual(self.command(0x0c1a, b'\x02'), CLASSIC_SCAN)
        self.assertEqual(self.describe(), 'LE scan off, unset, page scan 11/1280 ms')
        self.assertEqual(self.command(0x0c1e, struct.pack('<HH', 4096, 18)), CLASSIC_SCAN)
        self.assertEqual(self.command(0x0c1a, b'\x03'), CLASSIC_SCAN)
        self.assertEqual(self.describe(), 'LE scan off, unset, page scan 11/1280 ms, inquiry scan 11/2560 ms')

    # Bluetooth: Broadcom's Set ACL Priority around A2DP
    def test_acl_priority_follows_a2dp(self):
        # 12:36:27.147 fc57 [0b 00 01]: the AirPods' link high; 12:36:31.461 the same link back to normal
        self.assertEqual(self.command(0xfc57, bytes.fromhex('0b0001')), PRIORITY)
        self.assertEqual(self.state()[5:7], (1, 0x000b))
        self.assertIn('ACL 000b high priority', self.describe())
        self.assertEqual(self.command(0xfc57, bytes.fromhex('0c0000')), 0)  # another link going normal: no change
        self.assertEqual(self.command(0xfc57, bytes.fromhex('0b00ff')), 0)  # 0xFF (AOSP's high) is still high
        self.assertEqual(self.command(0xfc57, bytes.fromhex('0b0000')), PRIORITY)
        self.assertEqual(self.state()[5:7], (0, 0xffff))
        self.assertNotIn('priority', self.describe())

    def test_describe_never_overruns(self):
        self.command(0x200b, scan_params(64, 48))
        self.command(0x200c, b'\x01\x00')
        self.command(0x0c1c, struct.pack('<HH', 2048, 18))
        self.command(0x0c1a, b'\x03')
        self.command(0xfc57, bytes.fromhex('0b0001'))
        full = self.describe()
        for size in (1, 2, 10, 33, len(full), len(full) + 1):
            out = ctypes.create_string_buffer(b'\xaa' * (size + 8), size + 8)
            n = self.lib.coex_describe(out, size)
            self.assertLess(n, size)
            self.assertEqual(out.raw[size:], b'\xaa' * 8)
            self.assertTrue(full.startswith(out.value.decode()))

    # Wi-Fi: wifid's flags
    def test_wifid_flags(self):
        self.assertEqual(self.flags(0), 'none')
        self.assertEqual(self.flags(2), 'A2DP')  # "Bluetooth A2DP device connected."
        self.assertEqual(self.flags(0xa), 'A2DP+paging')
        self.assertEqual(self.flags(0x5), 'SCO+inquiry')
        self.assertEqual(self.flags(0x12), 'A2DP+0x10')

    # Wi-Fi: the access point's sta_info
    def test_sta_info_fields_and_window(self):
        self.assertTrue(self.sta(sta_info(tx=100, failed=1, ucast=80, mcast=20, tx_rate=65000, rx_rate=39000,
                                          retried=5, exhausted=1)))
        fields = (ctypes.c_uint32 * 13)()
        self.lib.sta_fields(fields)
        self.assertEqual(tuple(fields), (4, 200, 7, 100, 1, 80, 20, 65000, 39000, 0, 0, 5, 1))
        # the next window: 22 sent and acknowledged, nothing unicast came in, broadcasts did (r7v 12:36:31)
        self.assertTrue(self.sta(sta_info(tx=122, failed=1, ucast=80, mcast=26, tx_rate=65000, rx_rate=1000, idle=5,
                                          retried=9, exhausted=1)))
        self.assertEqual(self.sta_line(), 'AP: tx 22 failed 0 retried 4 exhausted 0, rx unicast 0 multicast 6 '
                         'decrypt-failed 0, rates 65/1 Mb/s, idle 5 s')

    def test_sta_info_versions_and_lengths(self):
        self.assertFalse(self.sta(b'\x04\x00'))  # too short
        self.assertTrue(self.sta(sta_info(ver=2, length=60)))
        self.assertEqual(self.sta_line(), '')  # no version 3 fields: nothing to say
        self.assertTrue(self.sta(sta_info(ver=3, length=84, tx=7, ucast=3)))
        fields = (ctypes.c_uint32 * 13)()
        self.lib.sta_fields(fields)
        self.assertEqual(fields[2], 1)  # version 3 only, no retry counters
        self.assertTrue(self.sta_line().startswith('AP: tx 7 failed 0, rx unicast 3 '))
        # a version 4 reply cut short by the firmware's length: the retry counters are past it
        self.assertTrue(self.sta(sta_info(ver=4, length=100, retried=9)))
        self.lib.sta_fields(fields)
        self.assertEqual((fields[2], fields[11]), (3, 0))


    # AFH: the Wi-Fi channel's range marked bad
    def afh(self, mhz):
        buf = ctypes.create_string_buffer(10)
        rng = (ctypes.c_uint * 2)()
        usable = self.lib.afh_map(mhz, buf, rng)
        return usable, (rng[0], rng[1]), buf.raw

    def test_afh_map_for_wifi_channels(self):
        usable, rng, raw = self.afh(2437)  # r7v: channel 6
        self.assertEqual((usable, rng), (56, (24, 46)))
        bits = int.from_bytes(raw, 'little')
        self.assertEqual([n for n in range(80) if not bits >> n & 1], list(range(24, 47)) + [79])
        self.assertEqual(self.afh(2412)[:2], (57, (0, 21)))  # channel 1 clamps at Bluetooth channel 0
        self.assertEqual(self.afh(2472)[:2], (59, (59, 78)))  # channel 13 at 78
        self.assertGreaterEqual(min(self.afh(f)[0] for f in range(2412, 2473, 5)), 20)  # the spec's minimum
        usable, rng, raw = self.afh(5180)  # 5 GHz: every channel usable, bit 79 reserved 0
        self.assertEqual((usable, rng, int.from_bytes(raw, 'little')), (79, (79, 79), (1 << 79) - 1))
        self.assertEqual(self.afh(0)[0], 79)

    def test_afh_map_readback(self):
        inside = ctypes.c_uint()
        everything = ((1 << 79) - 1).to_bytes(10, 'little')
        self.assertEqual((self.lib.afh_used(everything, 24, 46, ctypes.byref(inside)), inside.value), (79, 23))
        avoided = self.afh(2437)[2]
        self.assertEqual((self.lib.afh_used(avoided, 24, 46, ctypes.byref(inside)), inside.value), (56, 0))

    # Wi-Fi: power save as HOS runs it
    def test_pm2_sleep_return_and_policy(self):
        out = ctypes.create_string_buffer(24)
        self.lib.pm2_sleep_ret_ext(out)
        logic, pad, low, high, rx, tx, txrx, rxb, txb, txrxb = struct.unpack('<BBHHHHHIII', out.raw)
        self.assertEqual((logic, pad, low, high, rx, tx, txrx, rxb, txb, txrxb), (2, 0, 2000, 5000, 10, 0, 0, 0, 0, 0))
        self.assertEqual(self.lib.coex_pm(1, 1), 2)
        self.assertEqual(self.lib.coex_pm(1, 0), 0)  # the firmware refused the sleep return: stay at PM 0
        self.assertEqual(self.lib.coex_pm(0, 1), 0)  # Bluetooth off

    # Wi-Fi: the firmware's coexistence state
    def test_btc_dynctl_status_and_profile(self):
        status = bytes([0]) + struct.pack('<H', 0x1234) + struct.pack('<bbbBB', -9, -40, -55, 22, 5)
        out = ctypes.create_string_buffer(120)
        self.assertGreater(self.lib.btc_status_describe(status, len(status), out, len(out)), 0)
        self.assertEqual(out.value.decode(), 'btc mode 5 desense 22 (BT power -9 RSSI -40, Wi-Fi RSSI -55)')
        self.assertEqual(self.lib.btc_status_describe(status[:7], 7, out, len(out)), -1)
        # the NVRAM's btcdyn_* as dctl_prof_t: version 1, flags 0x3, levels 99/0/22/24, hysteresis 5, mode 5, 0 mode
        # switch rows, 1 desense row (5,-120,0,-52,-72)
        profile = bytes([1, 3, 99, 0, 22, 24, 5, 5, 0, 1]) + bytes(20) + struct.pack('<bbbbb', 5, -120, 0, -52, -72)
        profile += bytes(50 - len(profile))
        out = ctypes.create_string_buffer(200)
        self.lib.btc_profile_describe(profile, len(profile), out, len(out))
        self.assertEqual(out.value.decode(), 'btcdyn v1 flags 0x3, desense levels 99/0/22/24, default mode 5, '
                         'desense rows 5,-120,0,-52,-72')
        self.lib.btc_profile_describe(profile[:40], 40, out, len(out))
        self.assertEqual(out.value, b'')

    def test_nvram_values_from_the_loaded_file(self):
        text = (DRIVERS / 'SwitchWLAN/firmware/brcm/brcmfmac4356A3-pcie.txt').read_bytes()
        value = ctypes.c_uint32()
        found = {}
        for key in (b'boardflags', b'boardflags2', b'muxenab', b'btc_params84', b'btcdyn_flags', b'xtalfreq'):
            self.assertTrue(self.lib.nvram_value(text, len(text), key, ctypes.byref(value)), key)
            found[key] = value.value
        self.assertEqual(found, {b'boardflags': 0x02400001, b'boardflags2': 0xc0802000, b'muxenab': 0x10,
                                 b'btc_params84': 0x8, b'btcdyn_flags': 0x3, b'xtalfreq': 37400})
        self.assertFalse(self.lib.nvram_value(text, len(text), b'btc_mode', ctypes.byref(value)))  # HOS sets none
        self.assertFalse(self.lib.nvram_value(b'boardflags=zz\n', 14, b'boardflags', ctypes.byref(value)))


    # Wi-Fi: the coexistence mode (r8a: desense with any Bluetooth link)
    def test_dynamic_coexistence_off_and_tdm_with_links(self):
        # the firmware's btc_dynctl as r8a read it back: version 1, flags 0x3, levels 99/0/22/24, default mode 5
        profile = bytes([1, 3, 99, 0, 22, 24, 5, 5, 0, 1]) + bytes(20) + struct.pack('<bbbbb', 5, -120, 0, -52, -72)
        profile = ctypes.create_string_buffer(profile + bytes(50 - len(profile)), 50)
        self.assertTrue(self.lib.btc_profile_static(profile, 50))
        self.assertEqual(profile.raw[:10], bytes([1, 0, 99, 0, 22, 24, 5, 5, 0, 1]))  # only the flags change
        self.assertEqual(struct.unpack('<bbbbb', profile.raw[30:35]), (5, -120, 0, -52, -72))
        other = ctypes.create_string_buffer(bytes([2, 3]) + bytes(48), 50)
        self.assertFalse(self.lib.btc_profile_static(other, 50))  # another version: left alone
        self.assertEqual(other.raw[1], 3)
        self.assertFalse(self.lib.btc_profile_static(profile, 40))  # short
        self.assertEqual(self.lib.coex_mode(0, 5), 5)  # no link: the firmware's own (hybrid)
        self.assertEqual(self.lib.coex_mode(1, 5), 1)  # a Joy-Con: full TDM
        self.assertEqual(self.lib.coex_mode(3, 5), 1)  # two Joy-Cons and the AirPods


if __name__ == '__main__':
    unittest.main()
