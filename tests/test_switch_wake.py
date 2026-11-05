"""Waking the Switch (drivers/SwitchSuspend/SwitchWakeSources.h) on the host.

The header is compiled and driven through ctypes: a Bluetooth Joy-Con's or Pro Controller's Home in the HID input
reports it sends to bluetoothd (reports 0x30, 0x21 and 0x3F behind the HIDP header on an L2CAP dynamic channel; anything
else, a continuation, a fragmented frame or a signalling channel is not one), Home edges per connection, the wake
guarantee's state machine (armed by a wake source with the panel off, disarmed by a display-on request, firing at its
deadline), the power journal's ring, and the PMC wake events SwitchSuspend arms for SC7 (the same registers as its
constants before r15, and the wake reason decoded from the status registers). Also: the console buttons' HID descriptor
(drivers/SwitchTouch/SwitchHIDDevice.cpp) carries Menu as its fourth bit, kSwitchButtonHome, in the one-byte report."""
import ctypes
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TREE = ROOT / 'out/native-a57/root-v1/tree'
SUSPEND = TREE / 'drivers/SwitchSuspend'
HARNESS = ROOT / 'tests/switch_wake_harness.c'
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wsign-conversion', '-I',
         str(SUSPEND)]

HANDLE = 0x000b
HOME = 0x10


def acl(payload, handle=HANDLE, cid=0x0041, boundary=0x2, l2cap_length=None, acl_length=None):
    """An inbound HCI ACL packet (from its handle/flags header): L2CAP basic frame on `cid` carrying `payload`."""
    l2cap = len(payload) if l2cap_length is None else l2cap_length
    frame = l2cap.to_bytes(2, 'little') + cid.to_bytes(2, 'little') + bytes(payload)
    length = len(frame) if acl_length is None else acl_length
    value = handle | boundary << 12
    return value.to_bytes(2, 'little') + length.to_bytes(2, 'little') + frame


def full_report(report=0x30, shared=0x00, right=0x00, left=0x00):
    """HIDP DATA|INPUT + a standard full report: id, timer, battery/connection, right, shared, left, sticks, ..."""
    body = bytes([report, 0x5a, 0x8e, right, shared, left]) + bytes(43)  # 49 bytes, as a Joy-Con sends 0x30
    return bytes([0xa1]) + body


def simple_report(buttons1=0x00, buttons2=0x00):
    """HIDP DATA|INPUT + 0x3F (simple HID): id, two button bytes, hat, four 16-bit axes (12 bytes)."""
    return bytes([0xa1, 0x3f, buttons1, buttons2, 0x08]) + bytes([0x00, 0x80] * 4)


class Wake(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='ioswitch-wake-')
        library = Path(cls.directory.name) / 'libwake.dylib'
        subprocess.run(['clang', *FLAGS, '-shared', '-fPIC', str(HARNESS), '-o', str(library)], check=True)
        lib = cls.lib = ctypes.CDLL(str(library))
        lib.wake_bt_home.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint16)]
        lib.wake_bt_edge.argtypes = [ctypes.c_uint16, ctypes.c_int]
        lib.wake_guard_arm.argtypes = [ctypes.c_uint, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_int]
        lib.wake_guard_check.argtypes = [ctypes.c_uint64, ctypes.c_int, ctypes.c_int]
        lib.wake_guard_deadline.restype = ctypes.c_uint64
        lib.ring_append.argtypes = [ctypes.c_char_p]
        lib.ring_render.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        lib.ring_render.restype = ctypes.c_size_t
        lib.pmc_registers.argtypes = [ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint32)]
        lib.pmc_armed.argtypes = [ctypes.c_int, ctypes.c_int]
        lib.pmc_armed.restype = ctypes.c_uint64
        lib.pmc_reason.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64]
        lib.pmc_reason.restype = ctypes.c_char_p
        lib.source_name.argtypes = [ctypes.c_uint]
        lib.source_name.restype = ctypes.c_char_p

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.lib.wake_edges_reset()
        self.lib.wake_guard_reset()
        self.lib.ring_reset()

    def home(self, packet, length=None):
        """(result, handle) for the first `length` bytes of the packet (default: SWITCH_WAKE_BT_PEEK, as the driver)."""
        handle = ctypes.c_uint16(0xffff)
        n = min(len(packet), self.lib.bt_peek()) if length is None else length
        return self.lib.wake_bt_home(packet[:n], n, ctypes.byref(handle)), handle.value

    # --- Bluetooth Home ------------------------------------------------------------------------------------------
    def test_full_report_home(self):
        self.assertEqual(self.home(acl(full_report(0x30, shared=HOME))), (1, HANDLE))
        self.assertEqual(self.home(acl(full_report(0x21, shared=HOME | 0x02))), (1, HANDLE))  # Plus too
        self.assertEqual(self.home(acl(full_report(0x31, shared=HOME))), (1, HANDLE))

    def test_full_report_without_home(self):
        self.assertEqual(self.home(acl(full_report(0x30, shared=0x02, right=0xff))), (0, HANDLE))
        # A Joy-Con (L) reports Capture (0x20) in the shared byte, never Home.
        self.assertEqual(self.home(acl(full_report(0x30, shared=0x20, left=0x01))), (0, HANDLE))

    def test_simple_report(self):
        self.assertEqual(self.home(acl(simple_report(buttons2=HOME))), (1, HANDLE))
        self.assertEqual(self.home(acl(simple_report(buttons1=0xff, buttons2=0xef))), (0, HANDLE))
        # 0x3F has a fixed size: another length is not this report.
        self.assertEqual(self.home(acl(simple_report(buttons2=HOME) + b'\x00'))[0], -1)

    def test_handle_without_flags(self):
        packet = acl(full_report(0x30, shared=HOME), handle=0x0abc, boundary=0x0)  # first non-flushable
        self.assertEqual(self.home(packet), (1, 0x0abc))

    def test_not_a_controller_report(self):
        self.assertEqual(self.home(acl(full_report(0x30, shared=HOME), boundary=0x1))[0], -1)  # continuation
        self.assertEqual(self.home(acl(full_report(0x30, shared=HOME), cid=0x0001))[0], -1)  # signalling channel
        output = bytes([0xa2]) + full_report(0x30, shared=HOME)[1:]  # HIDP DATA|OUTPUT
        self.assertEqual(self.home(acl(output))[0], -1)
        keyboard = bytes([0xa1, 0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0])  # a keyboard's boot report
        self.assertEqual(self.home(acl(keyboard))[0], -1)
        # The first of several ACL packets of one L2CAP frame (its length says more is coming).
        fragmented = acl(full_report(0x30, shared=HOME), l2cap_length=100)
        self.assertEqual(self.home(fragmented)[0], -1)
        self.assertEqual(self.home(acl(full_report(0x30, shared=HOME))[:13], 13)[0], -1)  # not in yet
        self.assertEqual(self.home(b'', 0)[0], -1)

    def test_peek_is_enough(self):
        # The driver passes only SWITCH_WAKE_BT_PEEK bytes; both report kinds are decided within them.
        self.assertLessEqual(4 + 4 + 1 + 1 + 4 + 1, self.lib.bt_peek())
        self.assertEqual(self.home(acl(full_report(0x30, shared=HOME)), 14), (1, HANDLE))
        self.assertEqual(self.home(acl(simple_report(buttons2=HOME)), 12), (1, HANDLE))

    def test_edges(self):
        edge = self.lib.wake_bt_edge
        self.assertEqual(edge(HANDLE, 1), 1)
        self.assertEqual(edge(HANDLE, 1), 0)  # held: 0x30 repeats every 15 ms
        self.assertEqual(edge(HANDLE, 0), 0)
        self.assertEqual(edge(HANDLE, 1), 1)
        self.assertEqual(edge(0x0c, 1), 1)  # another controller
        self.assertEqual(edge(HANDLE, 1), 0)

    def test_edges_table_full(self):
        edge = self.lib.wake_bt_edge
        for handle in range(1, 9):
            self.assertEqual(edge(handle, 1), 1)
        self.assertEqual(edge(9, 1), 1)  # takes the oldest slot (handle 1)
        self.assertEqual(edge(2, 1), 0)  # still held
        self.assertEqual(edge(1, 1), 1)  # handle 1 was forgotten: a fresh press

    # --- The wake guarantee --------------------------------------------------------------------------------------
    def test_guard_fires_when_nothing_answers(self):
        lib, timeout = self.lib, 2500
        self.assertEqual(lib.wake_guard_arm(0, 1000, timeout, 0), 1)
        self.assertEqual(lib.wake_guard_arm(2, 2000, timeout, 0), 0)  # keeps the first deadline and source
        self.assertEqual((lib.wake_guard_deadline(), lib.wake_guard_source()), (3500, 0))
        self.assertEqual(lib.wake_guard_check(3499, 0, 0), -1)
        self.assertEqual(lib.wake_guard_check(3500, 0, 0), 1)
        self.assertEqual(lib.wake_guard_armed(), 0)
        self.assertEqual(lib.wake_guard_check(9000, 0, 0), 0)

    def test_guard_disarmed_by_display_on(self):
        lib = self.lib
        self.assertEqual(lib.wake_guard_arm(1, 0, 2500, 0), 1)
        lib.wake_guard_display_on()
        self.assertEqual(lib.wake_guard_check(5000, 0, 0), 0)
        # The on request still being played (panel not lit yet) or the panel already lit: nothing to do either.
        self.assertEqual(lib.wake_guard_arm(1, 0, 2500, 0), 1)
        self.assertEqual(lib.wake_guard_check(5000, 0, 1), 0)
        self.assertEqual(lib.wake_guard_arm(1, 0, 2500, 0), 1)
        self.assertEqual(lib.wake_guard_check(5000, 1, 0), 0)

    def test_guard_not_armed_with_panel_on(self):
        self.assertEqual(self.lib.wake_guard_arm(0, 0, 2500, 1), 0)
        self.assertEqual(self.lib.wake_guard_check(5000, 0, 0), 0)

    def test_guard_timeout_and_names(self):
        self.assertEqual(self.lib.guard_ms(), 2500)
        names = [self.lib.source_name(i).decode() for i in range(5)]
        self.assertEqual(names, ['power key', 'rail Joy-Con Home', 'Bluetooth Home', 'power in', 'unknown'])

    # --- The journal's ring --------------------------------------------------------------------------------------
    def render(self):
        buffer = ctypes.create_string_buffer(64 * 1024)
        length = self.lib.ring_render(buffer, len(buffer))
        return buffer.value.decode(), length

    def test_ring_keeps_the_last_lines_oldest_first(self):
        lines = self.lib.ring_lines()
        for i in range(lines + 6):
            self.lib.ring_append(f'line {i}'.encode())
        text, length = self.render()
        self.assertEqual(text.splitlines(), [f'line {i}' for i in range(6, lines + 6)])
        self.assertEqual(length, len(text))

    def test_ring_cuts_long_lines_and_newlines(self):
        self.lib.ring_append(b'x' * 500)
        self.lib.ring_append(b'first\nsecond')
        text, _ = self.render()
        self.assertEqual(text.splitlines(), ['x' * (self.lib.ring_line_bytes() - 1), 'first'])

    def test_ring_render_capacity(self):
        self.lib.ring_append(b'abcdef')
        small = ctypes.create_string_buffer(4)
        self.assertEqual(self.lib.ring_render(small, 4), 3)
        self.assertEqual(small.value, b'abc')
        self.lib.ring_reset()
        self.assertEqual(self.render(), ('', 0))

    # --- PMC wake events -----------------------------------------------------------------------------------------
    def registers(self, events):
        out = (ctypes.c_uint32 * 6)()
        self.lib.pmc_registers(events, out)
        return list(out)

    def test_pmc_armed_events(self):
        self.assertEqual(self.lib.pmc_armed(1, 0), (1 << 51) | (1 << 14))
        self.assertEqual(self.lib.pmc_armed(0, 0), 1 << 51)  # a USB-PD ALERT stuck low is not armed
        # The gauge's ALRT (r15-charging: charge-zone edges only during sleep), armed only while it reads high.
        self.assertEqual(self.lib.pmc_armed(1, 1), (1 << 51) | (1 << 14) | (1 << 27))
        self.assertEqual(self.lib.pmc_armed(0, 1), (1 << 51) | (1 << 27))

    def test_pmc_registers_match_the_old_constants(self):
        # SwitchSuspend before r15: WAKE_MASK = 1 << 14 (when the ALERT was high), WAKE2_MASK = 1 << (51 - 32), both
        # active low (LVL bits cleared), nothing active high.
        mask0, mask1, high0, high1, low0, low1 = self.registers(self.lib.pmc_armed(1, 0))
        self.assertEqual((mask0, mask1), (1 << 14, 1 << 19))
        self.assertEqual((high0, high1), (0, 0))
        self.assertEqual((low0, low1), (1 << 14, 1 << 19))
        mask0, mask1, high0, high1, low0, low1 = self.registers(1 << 16)  # soc-rtc: active high
        self.assertEqual((mask0, mask1, high0, high1, low0, low1), (1 << 16, 0, 1 << 16, 0, 0, 0))

    def test_pmc_reason(self):
        armed = self.lib.pmc_armed(1, 1)
        self.assertEqual(self.lib.pmc_reason(0, 1 << 19, armed), b'pwrbtn')
        self.assertEqual(self.lib.pmc_reason(1 << 27, 0, armed), b'battery')
        mask0, mask1, high0, high1, low0, low1 = self.registers(1 << 27)  # gauge ALRT: active low
        self.assertEqual((mask0, low0, high0), (1 << 27, 1 << 27, 0))
        self.assertEqual(self.lib.pmc_reason(1 << 14, 0, armed), b'usb-c')
        self.assertEqual(self.lib.pmc_reason(1 << 14, 1 << 19, armed), b'pwrbtn')  # table order
        self.assertEqual(self.lib.pmc_reason(1 << 14, 0, self.lib.pmc_armed(0, 0)), b'unknown')  # not armed
        self.assertEqual(self.lib.pmc_reason(1 << 8, 1 << 20, armed), b'unknown')  # Wi-Fi/BT wake: not armed

    # --- The console buttons' descriptor -------------------------------------------------------------------------
    def test_buttons_descriptor_has_home(self):
        source = (TREE / 'drivers/SwitchTouch/SwitchHIDDevice.cpp').read_text()
        body = re.search(r'kButtonsDescriptor\[\] = \{(.*?)\};', source, re.S).group(1)
        body = re.sub(r'//[^\n]*', '', body)
        data = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]{2}', body)]
        usages, bits, size, count, i = [], 0, 0, 0, 0
        while i < len(data):
            prefix = data[i]
            n = prefix & 3
            value = int.from_bytes(bytes(data[i + 1:i + 1 + n]), 'little')
            tag = prefix & 0xfc
            if tag == 0xa0:  # Collection: the usages before it name the collection, not a field
                usages = []
            elif tag == 0x08:
                usages.append(value)
            elif tag == 0x74:
                size = value
            elif tag == 0x94:
                count = value
            elif tag == 0x80:
                bits += size * count
            i += 1 + n
        self.assertEqual(usages, [0x30, 0xe9, 0xea, 0x40])  # Power, Volume +, Volume -, Menu (Home)
        self.assertEqual(bits, 8)  # the report's one byte after its ID (SwitchButtonsReport)
        header = (TREE / 'drivers/SwitchTouch/SwitchHID.h').read_text()
        self.assertIn('kSwitchButtonHome = 1u << 3', header)


if __name__ == '__main__':
    unittest.main()
