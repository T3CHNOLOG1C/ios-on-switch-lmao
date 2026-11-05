"""SwitchBluetooth's link table and link trace (drivers/SwitchBluetooth/SwitchBluetoothLinks.h) on the host.

The header is compiled and driven through ctypes with the HCI the r7r test produced (11-05 11:07:42-:46: the left
Joy-Con connecting and authenticating, the right one connecting while it is up) and with two Joy-Cons and AirPods at
once: the per-handle credit accounting (packets sent, completed, outstanding, peak) across three links, the stray
counts, the trace lines, and that a Link Key Request Reply never prints its key."""
import ctypes
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BLUETOOTH = ROOT / 'drivers/SwitchBluetooth'
HARNESS = ROOT / 'tests/switch_bluetooth_links_harness.c'
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wsign-conversion', '-I',
         str(BLUETOOTH)]

LEFT = '48:f1:eb:b6:cf:96'
RIGHT = '48:f1:eb:b6:9e:d1'
AIRPODS = '34:0e:22:32:9d:ed'
ACL, LE = 1, 2


def hci_addr(text):
    """A display-order address as HCI carries it (least significant byte first)."""
    return bytes(reversed(bytes.fromhex(text.replace(':', ''))))


def acl_header(handle, flags=0x2):
    value = handle | flags << 12
    return bytes([value & 0xff, value >> 8])


class Links(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='ioswitch-btlinks-')
        library = Path(cls.directory.name) / 'liblinks.dylib'
        subprocess.run(['clang', *FLAGS, '-shared', '-fPIC', str(HARNESS), '-o', str(library)], check=True)
        lib = cls.lib = ctypes.CDLL(str(library))
        u8p = ctypes.c_char_p
        lib.bt_link_open.argtypes = [ctypes.c_uint16, ctypes.c_uint8, u8p, ctypes.c_uint8]
        lib.bt_link_close.argtypes = [ctypes.c_uint16, ctypes.POINTER(ctypes.c_uint32)]
        lib.bt_link_rx.argtypes = [u8p]
        lib.bt_link_tx.argtypes = [u8p]
        lib.bt_links_completed.argtypes = [u8p, ctypes.c_uint32]
        lib.bt_link_counts.argtypes = [ctypes.c_uint16, ctypes.POINTER(ctypes.c_uint32)]
        lib.bt_event_wanted.argtypes = [ctypes.c_uint8, ctypes.c_uint8]
        lib.bt_describe_event.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint8, u8p, ctypes.c_uint32]
        lib.bt_describe_command.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint16, u8p, ctypes.c_uint32]
        lib.bt_command_traced.argtypes = [ctypes.c_uint16]
        lib.bt_request_note.argtypes = [u8p, ctypes.c_uint32]
        lib.bt_accept_as_central.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
        lib.bt_cod_is_gamepad.argtypes = [ctypes.c_uint32]

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.lib.bt_links_reset()
        self.lib.bt_requests_reset()

    # helpers
    def event(self, code, params):
        out = ctypes.create_string_buffer(200)
        self.lib.bt_describe_event(out, len(out), code, bytes(params), len(params))
        return out.value.decode()

    def command(self, opcode, params):
        out = ctypes.create_string_buffer(160)
        self.lib.bt_describe_command(out, len(out), opcode, bytes(params), len(params))
        return out.value.decode()

    def counts(self, handle):
        out = (ctypes.c_uint32 * 6)()
        found = self.lib.bt_link_counts(handle, out)
        return (tuple(out[:4]) if found else None), tuple(out[4:])

    def close(self, handle):
        out = (ctypes.c_uint32 * 4)()
        return tuple(out) if self.lib.bt_link_close(handle, out) else None

    def completed(self, *pairs):
        params = bytes([len(pairs)]) + b''.join(h.to_bytes(2, 'little') + n.to_bytes(2, 'little') for h, n in pairs)
        return self.lib.bt_links_completed(params, len(params))

    # the trace
    def test_left_joycon_connection_as_r7r_saw_it(self):
        request = hci_addr(LEFT) + bytes([0x08, 0x25, 0x00, 0x01])  # class 0x002508 (gamepad), ACL
        self.assertEqual(self.event(0x04, request), 'Connection Request {} class 002508 type 1'.format(LEFT))
        self.assertEqual(self.command(0x0409, hci_addr(LEFT) + b'\x00'),
                         'Accept Connection Request {} role 0'.format(LEFT))
        self.assertEqual(self.event(0x12, b'\x00' + hci_addr(LEFT) + b'\x00'),
                         'Role Change {} status 0x00 role 0'.format(LEFT))
        complete = b'\x00' + (0x000b).to_bytes(2, 'little') + hci_addr(LEFT) + b'\x01\x00'
        self.assertEqual(self.event(0x03, complete),
                         'Connection Complete 000b {} status 0x00 type 1 encryption 0'.format(LEFT))
        self.assertEqual(self.event(0x17, hci_addr(LEFT)), 'Link Key Request {}'.format(LEFT))
        self.assertEqual(self.event(0x08, b'\x00\x0b\x00\x01'), 'Encryption Change 000b status 0x00 on 1')
        self.assertEqual(self.event(0x14, b'\x00\x0b\x00\x02\x18\x00'), 'Mode Change 000b status 0x00 mode 2 interval 24')
        # The right Joy-Con, 11:07:43.364: its own end of the link (Remote User Terminated Connection).
        self.assertEqual(self.event(0x05, b'\x00\x0c\x00\x13'), 'Disconnection Complete 000c status 0x00 reason 0x13')

    def test_link_key_reply_never_prints_the_key(self):
        key = bytes(range(0xa0, 0xb0))
        line = self.command(0x040b, hci_addr(RIGHT) + key)
        self.assertEqual(line, 'Link Key Request Reply {}'.format(RIGHT))
        self.assertNotIn('a0a1', line)
        notification = hci_addr(RIGHT) + key + b'\x04'
        line = self.event(0x18, notification)
        self.assertEqual(line, 'Link Key Notification {} key type 4'.format(RIGHT))
        self.assertNotIn('a0a1', line)

    def test_command_lines(self):
        self.assertEqual(self.command(0x0803, b'\x0b\x00\x18\x00\x0c\x00\x02\x00\x01\x00'),
                         'Sniff Mode 000b interval 12-24 attempt 2 timeout 1')
        self.assertEqual(self.command(0x0c1a, b'\x02'), 'Write Scan Enable 0x02 (inquiry scan 0, page scan 1)')
        self.assertEqual(self.command(0x080d, b'\x0b\x00\x05\x00'), 'Write Link Policy Settings 000b policy 0x0005')
        self.assertEqual(self.command(0x080f, b'\x05\x00'), 'Write Default Link Policy Settings policy 0x0005')
        self.assertEqual(self.command(0x0c05, b'\x02\x00\x02'), 'Set Event Filter [020002]')
        self.assertEqual(self.command(0x0406, b'\x0c\x00\x16'), 'Disconnect 000c reason 0x16')
        self.assertEqual(self.command(0x0409, b'\x01'), 'Accept Connection Request (0409) of 1 bytes, too short')
        self.assertTrue(self.lib.bt_command_traced(0x0409))
        self.assertTrue(self.lib.bt_command_traced(0x040b))
        self.assertFalse(self.lib.bt_command_traced(0x0c35))  # Host Number Of Completed Packets: every packet
        self.assertFalse(self.lib.bt_command_traced(0x200c))  # LE: the once-a-boot trace has those

    def test_le_connection_complete(self):
        params = bytes([0x01, 0x00, 0x40, 0x00, 0x00, 0x01]) + hci_addr(AIRPODS) + bytes([0x18, 0, 0, 0, 0x48, 0, 0])
        self.assertEqual(self.event(0x3e, params),
                         'LE Connection Complete 0040 {} (type 1) status 0x00 role 0 interval 24 latency 0 timeout 72'
                         .format(AIRPODS))
        self.assertIn('too short', self.event(0x3e, params[:10]))

    def test_short_events_say_so(self):
        self.assertEqual(self.event(0x03, b'\x00\x0b'), 'Connection Complete (0x03) of 2 bytes, too short')
        self.assertEqual(self.event(0x10, b'\x07'), 'Hardware Error 0x07')
        self.assertEqual(self.event(0x1a, b'\x01'), 'Data Buffer Overflow 0x01')

    def test_which_events_are_taken_whole(self):
        wanted = self.lib.bt_event_wanted
        for code in (0x03, 0x04, 0x05, 0x06, 0x08, 0x10, 0x12, 0x13, 0x14, 0x17, 0x18, 0x1a, 0x1b, 0x38):
            self.assertTrue(wanted(code, 0), hex(code))
        for code in (0x0e, 0x0f, 0xff, 0x0b, 0x2f):  # completions (matched whole already), vendor, features, EIR
            self.assertFalse(wanted(code, 0), hex(code))
        self.assertTrue(wanted(0x3e, 0x01))
        self.assertTrue(wanted(0x3e, 0x0a))
        self.assertFalse(wanted(0x3e, 0x02))  # advertising reports: hundreds a second, never held back

    # incoming gamepads accepted as central
    def accept(self, addr, role):
        params = (ctypes.c_uint8 * 7)(*(hci_addr(addr) + bytes([role])))
        cod = ctypes.c_uint32(0)
        changed = self.lib.bt_accept_as_central(params, 7, ctypes.byref(cod))
        return changed, params[6], cod.value

    def test_classes(self):
        gamepad = self.lib.bt_cod_is_gamepad
        self.assertTrue(gamepad(0x000508))   # Joy-Con (L), (R), r7v Connection Requests
        self.assertTrue(gamepad(0x002508))   # gamepad with Limited Discoverable / other service bits
        self.assertTrue(gamepad(0x000504))   # joystick
        self.assertFalse(gamepad(0x240418))  # AirPods: audio, headphones
        self.assertFalse(gamepad(0x002540))  # keyboard
        self.assertFalse(gamepad(0x002580))  # mouse
        self.assertFalse(gamepad(0x5a020c))  # phone

    def test_joycon_accepted_as_central(self):
        self.lib.bt_request_note(hci_addr(RIGHT), 0x000508)
        self.assertEqual(self.accept(RIGHT, 1), (1, 0, 0x000508))
        # The request is consumed: a second Accept (bluetoothd retrying) is left alone.
        self.assertEqual(self.accept(RIGHT, 1), (0, 1, 0))

    def test_others_left_alone(self):
        self.lib.bt_request_note(hci_addr(AIRPODS), 0x240418)
        self.assertEqual(self.accept(AIRPODS, 1), (0, 1, 0))
        self.lib.bt_request_note(hci_addr(LEFT), 0x000508)
        self.assertEqual(self.accept(LEFT, 0), (0, 0, 0))  # bluetoothd asked to become central itself
        self.assertEqual(self.accept(RIGHT, 1), (0, 1, 0))  # no Connection Request seen for it
        short = (ctypes.c_uint8 * 7)(*hci_addr(RIGHT), 1)
        self.assertEqual(self.lib.bt_accept_as_central(short, 6, None), 0)

    def test_requests_kept_per_address(self):
        note = self.lib.bt_request_note
        note(hci_addr(LEFT), 0x000508)   # slot 0
        note(hci_addr(RIGHT), 0x000508)  # slot 1
        note(hci_addr(LEFT), 0x240418)   # the same address again: slot 0 updated, not a second entry
        self.assertEqual(self.accept(LEFT, 1), (0, 1, 0))  # its newest class (audio) decides, and it is consumed
        note(hci_addr(LEFT), 0x000508)   # slot 2
        for i in range(2):               # slots 3 and 0 (the oldest): four entries at most
            note(bytes([i, 1, 2, 3, 4, 5]), 0x000508)
        self.assertEqual(self.accept(RIGHT, 1), (1, 0, 0x000508))
        self.assertEqual(self.accept(LEFT, 1), (1, 0, 0x000508))
        self.assertEqual(self.accept('05:04:03:02:01:01', 1), (1, 0, 0x000508))
        self.assertEqual(self.accept('05:04:03:02:01:00', 1), (1, 0, 0x000508))

    # the table
    def test_two_joycons_and_airpods_share_the_credits(self):
        lib = self.lib
        self.assertTrue(lib.bt_link_open(0x000b, ACL, hci_addr(LEFT), 0))
        self.assertTrue(lib.bt_link_open(0x000c, ACL, hci_addr(RIGHT), 0))
        self.assertTrue(lib.bt_link_open(0x000d, ACL, hci_addr(AIRPODS), 0))
        # 30 HID output reports to each Joy-Con, 200 A2DP packets to the AirPods; input reports from the Joy-Cons.
        for _ in range(30):
            lib.bt_link_tx(acl_header(0x000b))
            lib.bt_link_tx(acl_header(0x000c))
            lib.bt_link_rx(acl_header(0x000b))
            lib.bt_link_rx(acl_header(0x000c, flags=0x2))
        for _ in range(200):
            lib.bt_link_tx(acl_header(0x000d, flags=0x2))
        lib.bt_link_tx(acl_header(0x000d, flags=0x1))  # a continuation fragment takes a buffer as well
        # The controller completes them in one event per round, several handles each.
        self.assertEqual(self.completed((0x000b, 30), (0x000c, 29), (0x000d, 195)), 254)
        self.assertEqual(self.counts(0x000b)[0], (30, 30, 30, 30))
        self.assertEqual(self.counts(0x000c)[0], (30, 30, 29, 30))
        self.assertEqual(self.counts(0x000d)[0], (0, 201, 195, 201))
        # The right Joy-Con goes: its last counts say one packet never came back.
        self.assertEqual(self.close(0x000c), (30, 30, 29, 30))
        self.assertIsNone(self.close(0x000c))
        # A completion for it after its Disconnection Complete is a stray, not a negative count elsewhere.
        self.assertEqual(self.completed((0x000c, 1)), 1)
        self.assertEqual(self.counts(0x000b)[1], (0, 1))

    def test_peak_is_the_most_ever_outstanding(self):
        lib = self.lib
        lib.bt_link_open(0x0010, ACL, hci_addr(AIRPODS), 0)
        for _ in range(8):
            lib.bt_link_tx(acl_header(0x0010))
        self.completed((0x0010, 6))
        for _ in range(3):
            lib.bt_link_tx(acl_header(0x0010))
        self.assertEqual(self.counts(0x0010)[0], (0, 11, 6, 8))

    def test_handles_reused_and_table_full(self):
        lib = self.lib
        for handle in range(8):
            self.assertTrue(lib.bt_link_open(handle, ACL, hci_addr(LEFT), 0xff))
        self.assertFalse(lib.bt_link_open(0x0100, LE, hci_addr(RIGHT), 1))
        lib.bt_link_tx(acl_header(0x0100))
        self.assertEqual(self.counts(0x0003)[1], (1, 0))
        lib.bt_link_tx(acl_header(0x0003))
        self.assertTrue(lib.bt_link_open(0x0003, ACL, hci_addr(RIGHT), 0))  # the same handle again: counts restart
        self.assertEqual(self.counts(0x0003)[0], (0, 0, 0, 0))

    def test_malformed_completed_packets(self):
        self.lib.bt_link_open(0x000b, ACL, hci_addr(LEFT), 0)
        self.assertEqual(self.lib.bt_links_completed(b'\x02\x0b\x00\x01\x00', 5), -1)
        self.assertEqual(self.lib.bt_links_completed(b'', 0), -1)
        self.assertEqual(self.counts(0x000b)[0], (0, 0, 0, 0))
        # Handle bits above 11 (flags) are not part of the handle.
        self.assertEqual(self.lib.bt_links_completed(b'\x01\x0b\x30\x02\x00', 5), 2)
        self.assertEqual(self.counts(0x000b)[0], (0, 0, 2, 0))


if __name__ == '__main__':
    unittest.main()
