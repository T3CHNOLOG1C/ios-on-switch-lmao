"""USB2 host mode on the USB-C port: SwitchPD's power-role decisions (drivers/SwitchUSB/SwitchPDPolicy.h) and the host
role's register arithmetic (drivers/SwitchUSB/XusbPorts.h), compiled from the same headers the kernel uses.

The power role runs the BM92T36 traces r7w recorded on hardware (11-05 12:47: a keyboard, mouse and trackpad through a
USB-C adapter, then the Mac) through a model of SwitchPD.cpp's apply/enter, and checks: the source rails (OCP,
v_vdd5v3, VCONN) are on only in Reattach and Source, so they are up before the switches go back on for the proven
sink-only partner and never on at boot, for a charger or for the Mac (dual-role); a re-attach whose VSAFE is our own
(SPDSRC1/2 closed, as with the rails up first) sources, a VSAFE nobody of ours drives does not; and over a fuzzed walk
nothing but a classified re-attach ever sources, and nothing sources without host mode.

The host interrupts (INTID 71 xHCI, 72 falcon mailbox) were never booted: until a line has reached its handler once,
a missing delivery (an event waiting, or nothing at all after the controller's first start) turns host mode off with
one line, never a panic; once it has fired, a lost interrupt panics only when the controller asked for it (IMAN.IE and
USBCMD.INTE) and nothing consumed the event; the routing the kernel relies on (DT GIC_SPI 39/40 level-high through the
LIC, IPFS as tegra_xusb_config) is checked against the OLED DT and the driver sources.

The host role: the USB 2.0 ports come from the xHCI Supported Protocol capabilities (pad 0 = the first one; a broken
list ends the walk), PORTSC writes never set a write-1 bit (PED, PR, the change bits) and only move PP, and the pad
controller values for each role (ID grounded with the VBUS override dropped first, floating for the XUDC; the charger
detect pull overrides and sources off, port 0's over-current pin off) match xusb-tegra210.c."""
import ctypes
import random
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TREE = ROOT / '.'
USB = TREE / 'drivers/SwitchUSB'
HARNESS = ROOT / 'tests/switch_usb_host_harness.c'
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wsign-conversion', '-I', str(USB)]

EMPTY, SINK, CLASSIFY, REATTACH, SOURCE, HELD = range(6)
NONE, SAW_DETACH, ENTER, START, END = range(5)

# BM92T36 bits (bm92txx.c).
SRC_FAULT, PLUGPULL, CONTRACT = 1 << 1, 1 << 3, 1 << 12
SPDSRC2, INSERT, VSAFE, CSIDE, SRC_MODE, SPDSNK, SPDSRC1 = 1 << 3, 1 << 7, 1 << 10, 1 << 11, 1 << 12, 1 << 14, 1 << 15
DFP, UFP = 2 << 8, 1 << 8
OTG_INSERT = 1 << 13

# r7w (live-1105-1249, kernel log): STATUS1/STATUS2 as SwitchPD logged them.
R7W_SINK_ATTACH = (0x1280, 0x2000)       # SRC_MODE | DFP | INSERT, OTG_INSERT: the keyboard's adapter as our sink
R7W_SINK_ATTACH_CC2 = (0x1A80, 0x2000)   # the same on CC2
R7W_DETACH_SOURCING = (0x8400, 0)        # SPDSRC1 | VSAFE, no INSERT: unplugged while we sourced
R7W_MAC_SOURCES = (0x0D80, 0)            # the Mac took the source role with our switches off: UFP, VSAFE, INSERT
R7W_CHARGER_AT_BOOT = (0x4580, 0x0001)   # SPDSNK | VSAFE | UFP | INSERT
REATTACH_RAILS_UP = (0x1280 | SPDSRC1 | VSAFE, 0x2000)  # what the re-attach shows once our switch closes onto v_vdd5v3


class Lib:
    lib = None


def setUpModule():
    Lib.directory = tempfile.TemporaryDirectory(prefix='ioswitch-usbhost-')
    library = Path(Lib.directory.name) / 'libusbhost.dylib'
    subprocess.run(['clang', *FLAGS, '-shared', '-fPIC', str(HARNESS), '-o', str(library)], check=True)
    lib = Lib.lib = ctypes.CDLL(str(library))
    i, u16, u32, uint = ctypes.c_int, ctypes.c_uint16, ctypes.c_uint32, ctypes.c_uint
    ip, buf, size = ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_size_t
    lib.pd_decide.argtypes = [i, u16, u16, u16, i, ip, ip, buf, size]
    lib.pd_decide.restype = i
    lib.pd_window_end.argtypes = [i, i, u16, ip, buf, size]
    lib.pd_window_end.restype = i
    lib.pd_switches_on.argtypes = [i, i]
    lib.pd_rails_on.argtypes = [i]
    lib.pd_vbus_ours.argtypes = [u16]
    lib.pd_phase_name.argtypes = [i]
    lib.pd_phase_name.restype = ctypes.c_char_p
    lib.fake_write.argtypes = [u32, u32]
    lib.find_ports.argtypes = [u32, uint, ctypes.POINTER(uint)]
    lib.find_ports.restype = u32
    for name in ('portsc_power',):
        getattr(lib, name).argtypes = [u32, i]
        getattr(lib, name).restype = u32
    lib.portsc_offset.argtypes = [u32, uint]
    lib.portsc_offset.restype = u32
    lib.portsc_keep.restype = u32
    for name in ('vbus_id_drop_vbus', 'vbus_id_host', 'vbus_id_device', 'chrg_ctl0_host', 'chrg_ctl1_host',
                 'oc_map_host'):
        getattr(lib, name).argtypes = [u32]
        getattr(lib, name).restype = u32
    lib.id_name.argtypes = [u32]
    lib.id_name.restype = ctypes.c_char_p
    lib.irq_judge.argtypes = [u32, u32, i, i, u32, u32, i, i, i, uint]
    lib.irq_judge.restype = i
    lib.xhci_pending.argtypes = [u32, u32]
    lib.xhci_deliverable.argtypes = [u32, u32]


def tearDownModule():
    Lib.directory.cleanup()


def decide(phase, alert, status1, status2, allow=True):
    nxt, cancel, why = ctypes.c_int(), ctypes.c_int(), ctypes.create_string_buffer(256)
    act = Lib.lib.pd_decide(phase, alert, status1, status2, int(allow), ctypes.byref(nxt), ctypes.byref(cancel), why,
                            len(why))
    return act, nxt.value, bool(cancel.value), why.value.decode()


def window_end(phase, saw, status1):
    nxt, why = ctypes.c_int(), ctypes.create_string_buffer(256)
    act = Lib.lib.pd_window_end(phase, int(saw), status1, ctypes.byref(nxt), why, len(why))
    return act, nxt.value, why.value.decode()


class Port:
    """SwitchPD.cpp's apply/enter/startSourcing/endSourcing/windowFired, with the hardware as recorded outputs."""

    def __init__(self, allow=True, boost_result=0):
        self.phase, self.allow, self.saw, self.window = EMPTY, allow, False, False
        self.switches = self.rails = self.boost = False
        self.boost_result = boost_result  # 0 ok, -2 the charger sees VBUS that is not ours, -1 I2C
        self.events, self.lines, self.status1 = [], [], 0

    def _set_switches(self, on):
        if on != self.switches:
            self.switches = on
            self.events.append(('switches', on))

    def _set_rails(self, on):
        if on != self.rails:
            self.rails = on
            self.events.append(('rails', on))

    def enter(self, nxt, why):
        self.phase, self.saw = nxt, False
        switches = bool(Lib.lib.pd_switches_on(nxt, int(self.allow)))
        rails = bool(Lib.lib.pd_rails_on(nxt))
        if not switches:
            self._set_switches(False)
        self._set_rails(rails)
        self.lines.append(why)
        if switches:
            self._set_switches(True)
        if nxt in (CLASSIFY, REATTACH):
            self.window = True

    def start(self):
        self.phase = SOURCE
        ours = bool(Lib.lib.pd_vbus_ours(self.status1))
        result = 0 if ours and self.boost_result == -2 else self.boost_result
        if result == -2:
            self.end(SINK, 'not sourcing: the charger sees VBUS that is not ours (sink until unplugged)')
        elif result:
            self.end(HELD, 'not sourcing: the BQ24193 boost failed')
        else:
            self.boost = True
            self.lines.append('sourcing')

    def end(self, nxt, why):
        self.boost = False
        self.enter(nxt, why)

    def apply(self, act, nxt, cancel, why):
        if cancel:
            self.window = False
        if act == SAW_DETACH:
            self.saw = True
        elif act == ENTER:
            self.enter(nxt, why)
        elif act == START:
            self.start()
        elif act == END:
            self.end(nxt, why)

    def alert(self, status, alert=PLUGPULL):
        self.status1 = status[0]
        self.apply(*decide(self.phase, alert, status[0], status[1], self.allow))
        self.check()

    def window_ends(self, status):
        if not self.window:
            return
        self.window = False
        act, nxt, why = window_end(self.phase, self.saw, status[0])
        if act == NONE:
            return
        self.status1 = status[0]
        self.apply(act, nxt, False, why)
        self.apply(*decide(self.phase, 0, status[0], status[1], self.allow))
        self.check()

    def check(self):
        assert self.rails == (self.phase in (REATTACH, SOURCE)), (self.phase, self.rails)
        assert self.boost == (self.phase == SOURCE), (self.phase, self.boost)
        if not self.allow:
            assert self.phase not in (CLASSIFY, REATTACH, SOURCE) and not self.rails and not self.switches


class PowerRoleTest(unittest.TestCase):
    def test_outputs_per_phase(self):
        lib = Lib.lib
        for phase in range(6):
            self.assertEqual(bool(lib.pd_rails_on(phase)), phase in (REATTACH, SOURCE))
            self.assertFalse(lib.pd_switches_on(phase, 0))
            self.assertEqual(bool(lib.pd_switches_on(phase, 1)), phase not in (SINK, CLASSIFY))
        self.assertEqual(lib.pd_phase_name(REATTACH), b'reattach')

    def test_keyboard_through_an_adapter(self):
        """r7w's plug-in, with the re-attach as the rails-first order makes it: sourced, rails before switches."""
        for attach in (R7W_SINK_ATTACH, R7W_SINK_ATTACH_CC2):
            port = Port()
            port.enter(EMPTY, 'start')
            self.assertTrue(port.switches)
            self.assertFalse(port.rails)
            port.events.clear()
            port.alert(attach)
            self.assertEqual(port.phase, CLASSIFY)
            self.assertEqual(port.events, [('switches', False)])
            port.alert((0, 0))  # it left with our switches off
            self.assertTrue(port.saw)
            port.events.clear()
            port.window_ends((0, 0))
            self.assertEqual(port.phase, REATTACH)
            self.assertEqual(port.events, [('rails', True), ('switches', True)])
            port.alert(REATTACH_RAILS_UP)
            self.assertEqual(port.phase, SOURCE)
            self.assertTrue(port.boost and port.rails and port.switches)
            port.events.clear()
            port.alert(R7W_DETACH_SOURCING)
            self.assertEqual(port.phase, EMPTY)
            self.assertEqual(port.events, [('rails', False)])
            self.assertTrue(port.switches)
            self.assertIn('detached', port.lines[-1])

    def test_reattach_before_our_switch_closes(self):
        """r7w's own re-attach status (no VSAFE yet) sources too."""
        act, nxt, cancel, _ = decide(REATTACH, PLUGPULL, *R7W_SINK_ATTACH)
        self.assertEqual((act, nxt, cancel), (START, SOURCE, True))

    def test_vsafe_that_is_not_ours(self):
        act, nxt, cancel, why = decide(REATTACH, PLUGPULL, SRC_MODE | VSAFE | DFP | INSERT, OTG_INSERT)
        self.assertEqual((act, nxt, cancel), (ENTER, SINK, True))
        self.assertIn('not ours', why)
        self.assertTrue(Lib.lib.pd_vbus_ours(SPDSRC2 | VSAFE))
        self.assertFalse(Lib.lib.pd_vbus_ours(VSAFE | INSERT | SRC_MODE))
        # In Empty the rails are off: any VSAFE is someone else's.
        act, nxt, _, why = decide(EMPTY, PLUGPULL, *REATTACH_RAILS_UP)
        self.assertEqual((act, nxt), (ENTER, SINK))

    def test_charger_sees_vbus_that_is_not_ours(self):
        port = Port(boost_result=-2)
        port.enter(REATTACH, 'proven')
        port.alert(R7W_SINK_ATTACH)  # no switch closed yet: the charger's VBUS is foreign
        self.assertEqual(port.phase, SINK)
        self.assertFalse(port.rails or port.switches or port.boost)
        port = Port(boost_result=-2)
        port.enter(REATTACH, 'proven')
        port.alert(REATTACH_RAILS_UP)  # our switch closed: what it sees is v_vdd5v3
        self.assertEqual(port.phase, SOURCE)

    def test_mac_is_dual_role(self):
        port = Port()
        port.enter(EMPTY, 'start')
        port.alert(R7W_SINK_ATTACH_CC2)
        port.alert(R7W_MAC_SOURCES)
        self.assertEqual(port.phase, SINK)
        self.assertFalse(port.window)
        self.assertNotIn(('rails', True), port.events)
        self.assertIn('dual-role', port.lines[-1])

    def test_charger_at_boot_and_no_host_mode(self):
        port = Port()
        port.alert(R7W_CHARGER_AT_BOOT, alert=0)
        self.assertEqual(port.phase, SINK)
        port = Port(allow=False)
        port.alert(R7W_SINK_ATTACH)
        self.assertEqual(port.phase, SINK)
        self.assertIn('host mode off', port.lines[-1])

    def test_classify_windows(self):
        self.assertEqual(window_end(CLASSIFY, False, R7W_SINK_ATTACH[0])[:2], (ENTER, SINK))  # no detach: no proof
        self.assertEqual(window_end(CLASSIFY, True, R7W_SINK_ATTACH[0])[:2], (ENTER, SINK))   # back already
        act, nxt, why = window_end(CLASSIFY, True, 0)
        self.assertEqual((act, nxt), (ENTER, REATTACH))
        self.assertIn('rails', why)
        act, nxt, why = window_end(REATTACH, False, 0)
        self.assertEqual((act, nxt), (ENTER, EMPTY))
        self.assertIn('rails off', why)
        for phase in (EMPTY, SINK, SOURCE, HELD):
            self.assertEqual(window_end(phase, True, 0)[0], NONE)

    def test_leaving_source(self):
        self.assertEqual(decide(SOURCE, CONTRACT, *R7W_SINK_ATTACH)[:2], (END, HELD))
        self.assertEqual(decide(SOURCE, SRC_FAULT, *R7W_SINK_ATTACH)[:2], (END, HELD))
        self.assertEqual(decide(SOURCE, 0, R7W_SINK_ATTACH[0] | 1, 0x2000)[:2], (END, HELD))
        self.assertEqual(decide(SOURCE, PLUGPULL, *R7W_MAC_SOURCES)[:2], (END, SINK))
        self.assertEqual(decide(SOURCE, 0, *REATTACH_RAILS_UP)[0], NONE)
        self.assertEqual(decide(HELD, PLUGPULL, 0, 0)[:2], (ENTER, EMPTY))

    def test_fuzzed_walk(self):
        rng = random.Random(1105)
        statuses = [(0, 0), R7W_SINK_ATTACH, R7W_SINK_ATTACH_CC2, R7W_DETACH_SOURCING, R7W_MAC_SOURCES,
                    R7W_CHARGER_AT_BOOT, REATTACH_RAILS_UP, (SRC_MODE | VSAFE | DFP | INSERT, OTG_INSERT),
                    (SRC_MODE | DFP | INSERT, 0), (INSERT | UFP, 0)]
        for allow in (True, False):
            for boost in (0, -1, -2):
                port = Port(allow=allow, boost_result=boost)
                port.enter(EMPTY, 'start')
                for _ in range(4000):
                    status = rng.choice(statuses)
                    before = port.phase
                    if rng.random() < 0.25:
                        port.window_ends(status)
                    else:
                        alert = rng.choice([0, PLUGPULL, PLUGPULL, CONTRACT, SRC_FAULT, PLUGPULL | CONTRACT])
                        port.alert(status, alert)
                        if port.phase == SOURCE and before != SOURCE:
                            s1, s2 = status
                            self.assertEqual(before, REATTACH)
                            self.assertTrue(s1 & INSERT and s1 & SRC_MODE and s2 & OTG_INSERT)
                            self.assertTrue(not s1 & VSAFE or s1 & (SPDSRC1 | SPDSRC2))
                        if before == SOURCE and not status[0] & INSERT:
                            self.assertNotEqual(port.phase, SOURCE)
                    if port.phase == SOURCE:
                        self.assertTrue(port.rails and port.switches)


class HostRoleTest(unittest.TestCase):
    def setUp(self):
        Lib.lib.fake_reset()

    def caps(self, entries, xecp=0x500):
        """entries: (cap id, next in dwords, major, first port, count), laid out from xecp; an entry after one whose
        next is 0 still gets its own place (unreachable)."""
        lib = Lib.lib
        lib.fake_write(0x10, (xecp >> 2) << 16 | 0x1)  # HCCPARAMS1: xECP, AC64
        offset = xecp
        for cap, nxt, major, first, count in entries:
            lib.fake_write(offset, major << 24 | nxt << 8 | cap)
            lib.fake_write(offset + 4, 0x20425355)  # "USB "
            lib.fake_write(offset + 8, count << 8 | first)
            offset += (nxt or 4) * 4

    def find(self, major, window=0x2000):
        reads = ctypes.c_uint()
        value = Lib.lib.find_ports(window, major, ctypes.byref(reads))
        return (value >> 16, value & 0xffff), reads.value

    def test_tegra_layout(self):
        """T210 XUSB: legacy support, USB 3 ports 1-4, USB 2 ports 5-9 (pads 0-3, HSIC)."""
        self.caps([(1, 4, 0, 0, 0), (2, 5, 3, 1, 4), (2, 0, 2, 5, 5)])
        self.assertEqual(self.find(2)[0], (5, 5))
        self.assertEqual(self.find(3)[0], (1, 4))
        self.assertEqual(Lib.lib.portsc_offset(0x20, 5), 0x20 + 0x400 + 0x40)

    def test_broken_lists(self):
        self.caps([(1, 0, 0, 0, 0), (2, 0, 2, 5, 5)])  # the list ends before the USB 2 capability
        self.assertEqual(self.find(2)[0], (0, 0))
        self.caps([(2, 0x80, 3, 1, 4)], xecp=0x1f00)  # next points outside the window
        self.assertEqual(self.find(2, window=0x2000)[0], (0, 0))
        Lib.lib.fake_reset()
        self.assertEqual(self.find(2)[0], (0, 0))  # no xECP at all
        Lib.lib.fake_reset()
        Lib.lib.fake_write(0x10, (0x100 >> 2) << 16)
        for k in range(200):  # a long chain of other capabilities: the walk stops after 64
            Lib.lib.fake_write(0x100 + 4 * k, 1 << 8 | 0xc0)
        result, reads = self.find(2)
        self.assertEqual(result, (0, 0))
        self.assertLessEqual(reads, 1 + 64)

    def test_portsc_writes(self):
        lib = Lib.lib
        write1 = (1 << 1) | (1 << 4) | (1 << 16) | (0x7f << 17) | (1 << 31)  # PED, PR, LWS, changes, WPR
        rng = random.Random(9)
        for _ in range(2000):
            v = rng.getrandbits(32)
            for on in (0, 1):
                out = lib.portsc_power(v, on)
                self.assertEqual(out & write1, 0)
                self.assertEqual(bool(out & (1 << 9)), bool(on))
                self.assertEqual(out & ~(1 << 9), v & lib.portsc_keep() & ~(1 << 9))
        # A powered, connected high-speed port with a connect change pending: the change bit is not cleared by us.
        self.assertEqual(lib.portsc_power(0x00020e03, 0), 0x00000c01)

    def test_vbus_id(self):
        lib = Lib.lib
        xudc_on_bus = (1 << 12) | (1 << 14) | (1 << 16) | (8 << 18)  # XudcHardware connect(): VBUS on, ID floating
        self.assertEqual(lib.vbus_id_drop_vbus(xudc_on_bus), (1 << 12) | (1 << 16) | (8 << 18))
        host = lib.vbus_id_host(xudc_on_bus)
        self.assertEqual(host, (1 << 12) | (1 << 16))
        self.assertEqual(lib.id_name(host), b'grounded')
        device = lib.vbus_id_device(host)
        self.assertEqual(device, (1 << 12) | (1 << 16) | (8 << 18))
        self.assertEqual(lib.id_name(device), b'floating')
        self.assertEqual(lib.vbus_id_host(0xffffffff) & (0xf << 18), 0)
        self.assertEqual(lib.vbus_id_host(0xffffffff) & (1 << 14), 0)

    def test_charger_detect_and_oc(self):
        lib = Lib.lib
        # utmi_pad_charger_detect_on leaves: pull overrides enabled with value 0, PD_CHG clear, sources off.
        detect_on_ctl1 = (1 << 16) | (1 << 18) | (1 << 20) | (1 << 22) | 0x80
        self.assertEqual(lib.chrg_ctl1_host(detect_on_ctl1), 0x80)
        self.assertEqual(lib.chrg_ctl0_host(0xffffffff), 0xffffffff & ~(0xf << 9))
        self.assertEqual(lib.chrg_ctl0_host(1), 1)  # PD_CHG as found
        self.assertEqual(lib.oc_map_host(0xfff0), 0xffff)
        self.assertEqual(lib.oc_map_host(0xffff), 0xffff)


FINE, CONFIRM, OFF, PANIC = range(4)


def judge(armed, now, first=False, stage=1):
    """armed/now: (count, erdp, pending, deliverable)."""
    return Lib.lib.irq_judge(armed[0], armed[1], int(armed[2]), int(armed[3]), now[0], now[1], int(now[2]),
                             int(now[3]), int(first), stage)


class InterruptCheckTest(unittest.TestCase):
    def run_check(self, armed, stage1, stage2, first=False):
        """SwitchXHCI::checkFired: stage 1 at 2 s, stage 2 50 ms later against the stage-1 sample."""
        verdict = judge(armed, stage1, first, 1)
        if verdict != CONFIRM:
            return verdict
        return judge(stage1, stage2, first, 2)

    def test_never_fired_first_start(self):
        """r7w: host up, device on the port, no port event and INTID 71 never fired: one line, host off, no panic."""
        quiet = (0, 0x1000, False, True)
        self.assertEqual(self.run_check(quiet, quiet, quiet, first=True), OFF)
        waiting = (0, 0x1000, True, True)
        self.assertEqual(self.run_check(quiet, waiting, waiting, first=True), OFF)
        self.assertEqual(self.run_check(quiet, waiting, waiting, first=False), OFF)  # a later role, event stuck
        masked = (0, 0x1000, True, False)  # IP set but IE/INTE off: still never delivered, still not a panic
        self.assertEqual(self.run_check(quiet, masked, masked, first=True), OFF)

    def test_delivered(self):
        before = (0, 0x1000, False, True)
        self.assertEqual(self.run_check(before, (1, 0x1040, False, True), (1, 0x1040, False, True), first=True), FINE)
        # fired before the check was armed (the first PSC came at once): proven, nothing waiting
        self.assertEqual(self.run_check((1, 0x1040, False, True), (1, 0x1040, False, True), (1, 0x1040, False, True),
                                        first=True), FINE)
        # delivered between stage 1 and stage 2
        self.assertEqual(self.run_check(before, (0, 0x1000, True, True), (1, 0x1040, False, True), first=True), FINE)

    def test_proven_line_lost(self):
        armed = (3, 0x1100, False, True)
        stuck = (3, 0x1100, True, True)
        self.assertEqual(self.run_check(armed, stuck, stuck), PANIC)
        # the controller masked its interrupter (IE or INTE off): not a lost interrupt
        self.assertEqual(self.run_check(armed, (3, 0x1100, True, False), (3, 0x1100, True, False)), FINE)
        # the event was consumed (ERDP moved) or delivered
        self.assertEqual(self.run_check(armed, (3, 0x1140, True, True), (3, 0x1140, True, True)), FINE)
        self.assertEqual(self.run_check(armed, (4, 0x1100, True, True), (4, 0x1100, True, True)), FINE)
        # nothing due on a later role of a proven line
        self.assertEqual(self.run_check(armed, armed, armed), FINE)

    def test_no_evidence(self):
        quiet = (0, 0x1000, False, True)
        self.assertEqual(self.run_check(quiet, quiet, quiet, first=False), FINE)

    def test_mailbox(self):
        """INTID 72: an SMI waiting with no handler run, never fired: host off; fired before: panic."""
        self.assertEqual(self.run_check((0, 0, False, True), (0, 0, True, True), (0, 0, True, True)), OFF)
        self.assertEqual(self.run_check((2, 0, False, True), (2, 0, True, True), (2, 0, True, True)), PANIC)
        self.assertEqual(self.run_check((0, 0, False, True), (0, 0, False, True), (0, 0, False, True)), FINE)

    def test_stage_one_never_acts(self):
        for count in (0, 5):
            for pending in (False, True):
                for first in (False, True):
                    self.assertIn(judge((count, 0, False, True), (count, 0, pending, True), first, 1), (FINE, CONFIRM))

    def test_register_bits(self):
        lib = Lib.lib
        self.assertTrue(lib.xhci_pending(1, 0))      # IMAN.IP
        self.assertTrue(lib.xhci_pending(0, 1 << 3))  # USBSTS.EINT
        self.assertFalse(lib.xhci_pending(2, 1))      # IE, HCH
        self.assertTrue(lib.xhci_deliverable(2, 1 << 2))
        self.assertFalse(lib.xhci_deliverable(2, 0))
        self.assertFalse(lib.xhci_deliverable(1, 1 << 2))


class InterruptRoutingTest(unittest.TestCase):
    """The routing SwitchXHCI's comment claims, against the OLED DT (as decompiled in L4T's nx-platform sources: the
    xusb node's interrupts) and the driver sources."""

    def test_driver_constants(self):
        host = (USB / 'XusbHost.h').read_text()
        self.assertIn('kHostIntid = 32 + 39, kMailboxIntid = 32 + 40', host)  # DT <0 0x27 4>, <0 0x28 4>: SPI 39, 40
        xhci = (USB / 'SwitchXHCI.cpp').read_text()
        self.assertIn('ioswitch_interrupt_nub(xhost::kHostIntid, false)', xhci)      # level, as IRQ_TYPE_LEVEL_HIGH
        self.assertIn('ioswitch_interrupt_nub(xhost::kMailboxIntid, false)', xhci)

    def test_ipfs_as_tegra_xusb_config(self):
        source = (USB / 'XusbHost.cpp').read_text()
        power = re.search(r'bool Controller::powerOn\(.*?\n}\n', source, re.S).group(0)
        order = ['rmw(ipfs, IPFS_CONFIGURATION, 0, IPFS_EN_FPCI)', 'rmw(fpci, XUSB_CFG_4, CFG_4_BASE_MASK',
                 'rmw(fpci, XUSB_CFG_1, 0, CFG_1_ENABLES)', 'rmw(ipfs, IPFS_INTR_MASK, 0, IPFS_IP_INT_MASK)',
                 'w(ipfs, IPFS_CLKGATE_HYSTERESIS, 0x80)']
        positions = [power.index(step) for step in order]
        self.assertEqual(positions, sorted(positions))
        self.assertIn('IPFS_CONFIGURATION = 0x180', source)
        self.assertIn('IPFS_INTR_MASK = 0x188', source)
        self.assertIn('IPFS_IP_INT_MASK = 1u << 16', source)
        self.assertIn('IPFS_INTR_ENABLE = 0x198', source)
        self.assertNotIn('w(ipfs, IPFS_INTR_ENABLE', source)  # Linux never writes it
        self.assertNotIn('rmw(ipfs, IPFS_INTR_ENABLE', source)

    def test_gic_level_spi_with_lic(self):
        gic = (TREE / 'drivers/ArmGIC/ArmGIC.cpp').read_text()
        init = re.search(r'void ArmGIC::initVector\(.*?\n}\n', gic, re.S).group(0)
        self.assertIn('isEdge(intid) ? (read32(config) | edgeBit) : (read32(config) & ~edgeBit)', init)
        self.assertIn('ICTLR_CPU_IEP_CLASS', init)
        enable = re.search(r'void ArmGIC::enableVector\(.*?\n}\n', gic, re.S).group(0)
        self.assertLess(enable.index('ICTLR_CPU_IER_SET'), enable.index('writeBit(GICD_ISENABLER, intid)'))
        self.assertIn('uint32_t ioswitch_interrupt_count(uint32_t intid)', gic)

    def test_no_panic_before_proof(self):
        xhci = (USB / 'SwitchXHCI.cpp').read_text()
        check = re.search(r'void SwitchXHCI::checkFired\(.*?\n}\n', xhci, re.S).group(0)
        self.assertEqual(check.count('panic('), 1)
        self.assertLess(check.index('if (verdict == XUSB_IRQ_PANIC)'), check.index('panic('))
        self.assertIn('switch_usb_host_failed()', check)


class KernelSourcesTest(unittest.TestCase):
    """The kernel files use these headers, in the order the tests assume."""

    def test_switchpd_uses_the_policy(self):
        source = (USB / 'SwitchPD.cpp').read_text()
        self.assertIn('#include "SwitchPDPolicy.h"', source)
        self.assertIn('switch_pd_decide(', source)
        self.assertIn('switch_pd_window_end(', source)
        enter = re.search(r'void SwitchPD::enter\(.*?\n}\n', source, re.S).group(0)
        self.assertLess(enter.index('if (!switches) setSwitches(false)'), enter.index('setRails(rails)'))
        self.assertLess(enter.index('setRails(rails)'), enter.index('if (switches) setSwitches(true)'))

    def test_host_role_uses_the_port_helpers(self):
        source = (USB / 'XusbHost.cpp').read_text()
        self.assertIn('#include "XusbPorts.h"', source)
        for name in ('xhci_find_ports(', 'xhci_portsc_power(', 'xusb_vbus_id_host(', 'xusb_chrg_ctl1_host('):
            self.assertIn(name, source)
        role = (USB / 'SwitchUSB.cpp').read_text()
        apply = re.search(r'void applyRole\(.*?\n}\n', role, re.S).group(0)
        # The pad goes to the host before the controller first starts (it powers its ports itself then).
        self.assertLess(apply.index('switchxhci_set_host_role(true)'), apply.index('switchxhci_start('))


if __name__ == '__main__':
    unittest.main()
