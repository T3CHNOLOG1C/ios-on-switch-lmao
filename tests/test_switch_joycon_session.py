"""The rail Joy-Con's HID session rule (drivers/SwitchJoyCon/SwitchJoyConSession.h), and the r7p9d gamecontrollerd
connection storm it ends, reproduced on the host.

The header is compiled and driven through ctypes. Around it runs a model of the three parties, each step the 21H461
code path it is named after (GameController.framework, IOKit's IOHIDEventSystemClient, JoyConHIDServicePlugin):
  * gamecontrollerd: its HID monitor's cache of service IDs, refreshed on a backboardd restart the way
    __IOHIDEventSystemClientRefresh does (an ID it knew is kept with no callback, a new one gets the matching callback,
    a vanished one is dropped with none); the manager server parking connections for IDs it has not seen
    (_hidqueue_pushPendingDriverConnection:forRegistryID:); _GCNintendoJoyConDeviceManager's claimed, pending and
    physical devices (acceptDriverConnection:/acceptFilterConnection: invalidate when there is no pending device,
    claimHIDService: block .109 moves pending to physical, block .110 retires a physical device with the same
    identifier, relinquishHIDService: on removal; the driver-connection invalidation handler only clears it);
  * backboardd: one JoyConHIDServicePlugin per Joy-Con service, which opens the HID device, and whose filter and
    plugin connections reconnect at once when gamecontrollerd cancels them;
  * the kernel: SwitchJoyConDevice registry IDs, the session the device keeps (the header), and the rail's republish.
With the session rule off (the code before r13) a respring is the storm; with it, gamecontrollerd takes the new devices
and refuses nothing."""
import ctypes
import heapq
import itertools
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JOYCON = ROOT / 'out/native-a57/root-v1/tree/drivers/SwitchJoyCon'
HARNESS = ROOT / 'tests/switch_joycon_session_harness.c'
FLAGS = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wconversion', '-Wsign-conversion', '-I', str(JOYCON)]
NONE, CLOSED, REPLACED = 0, 1, 2
BUDGET = 4000  # connections: far past any healthy run (four per respring), well short of the storm's 68094


class Session(ctypes.Structure):
    _fields_ = [('client', ctypes.c_size_t), ('pid', ctypes.c_int32), ('ended', ctypes.c_bool)]


class Guard(ctypes.Structure):
    _fields_ = [('at', ctypes.c_uint64), ('pid', ctypes.c_int32)]


class Device:
    """SwitchJoyConDevice: a registry ID and, with the rule on, the header's session (handleOpen/handleClose)."""

    def __init__(self, world, side, serial):
        self.world, self.side, self.serial = world, side, serial
        self.id = world.next_id
        world.next_id += 1
        self.session = Session()
        self.attached = True  # until switch_joycon_hid_destroy's detachOutput

    def open(self, client, pid, name):
        if self.world.rule and self.attached:
            event = self.world.lib.jc_session_open(ctypes.byref(self.session), client, pid, name == 'backboardd')
            if event:
                self.world.rail.session_ended(self.side, self.session.pid, pid)

    def close(self, client):
        if self.world.rule and self.attached:
            if self.world.lib.jc_session_close(ctypes.byref(self.session), client):
                self.world.rail.session_ended(self.side, self.session.pid, 0)


class Rail:
    """SwitchJoyConRail: publishes a running Joy-Con's device; sessionEnded marks the side and wakes the work loop."""

    def __init__(self, world):
        self.world = world
        self.devices, self.marks, self.guards = {}, {}, {}
        self.republished, self.off, self.logged = 0, False, []

    def publish(self, side):
        device = Device(self.world, side, {'L': '48:f1:eb:b6:cf:96', 'R': '48:f1:eb:b6:9e:d1'}[side])
        self.devices[side] = device
        self.world.devices[device.id] = device
        device.open(0, 0, 'kernel_task')  # IOHIDEventDummyService: a kernel client
        server = self.world.backboardd
        if server and server.alive:
            self.world.at(1, server.add_service, device)

    def destroy(self, side):  # dropHid
        device = self.devices.pop(side, None)
        if device:
            device.attached = False
            del self.world.devices[device.id]
            server = self.world.backboardd
            if server and server.alive:
                self.world.at(0.5, server.remove_service, device.id)
        self.marks.pop(side, None)

    def session_ended(self, side, served, opener):
        self.marks[side] = (served, opener)
        self.world.at(0.1, self.work)  # interruptOccurred on the software source

    def work(self):  # sessionAction -> republish(Side &)
        for side in list(self.marks):
            served, opener = self.marks.pop(side)
            if self.off or side not in self.devices:
                continue
            guard = self.guards.setdefault(side, Guard())
            if not self.world.lib.jc_republish_allowed(ctypes.byref(guard), served, int(self.world.now)):
                self.off = True
                self.logged.append(f'backboardd[{served}] ended a second HID session on Joy-Con ({side})')
                continue
            self.destroy(side)
            self.publish(side)
            self.republished += 1


class Plugin:
    """JoyConHIDServicePlugin with GamepadHIDServiceFilter on one service: opens the HID device in initWithService,
    the filter connects ~14 ms later and the plugin after the Joy-Con's device-info reply (~80 ms, r7p9d)."""

    def __init__(self, server, device):
        self.server, self.device, self.world = server, device, server.world
        self.client = next(self.world.clients)
        self.connections, self.cancelled = {}, False

    def start(self):
        self.device.open(self.client, self.server.pid, 'backboardd')
        self.world.at(14, self.connect, 'filter')
        self.world.at(80, self.connect, 'driver')

    def connect(self, kind):
        if self.cancelled:
            return
        self.world.connections += 1
        connection = (self, kind, next(self.world.clients))
        self.connections[kind] = connection
        self.world.at(1, self.world.gcd.incoming, connection)

    def interrupted(self, connection):  # "interrupted.  Attempting to re-establish."
        if not self.cancelled and self.connections.get(connection[1]) is connection:
            self.world.at(0.5, self.connect, connection[1])

    def cancel(self, closes_after):  # service removed, or backboardd gone (its clients close as the task goes)
        self.cancelled = True
        for connection in self.connections.values():
            self.world.gcd.peer_invalidated(connection)
        if closes_after is not None:
            self.world.at(closes_after, self.device.close, self.client)


class Backboardd:
    """backboardd's HID event system: a service and a plugin for every Joy-Con device, live add/remove to monitors."""

    def __init__(self, world, pid):
        self.world, self.pid, self.alive, self.services = world, pid, True, {}

    def start(self):
        for device in list(self.world.devices.values()):
            self.add_service(device)
        self.world.at(70, self.world.gcd.server_up, self)  # "IOHIDEventSystemServer died. Reestablishing connection."

    def add_service(self, device):
        if not self.alive or device.id not in self.world.devices or device.id in self.services:
            return
        plugin = Plugin(self, device)
        self.services[device.id] = plugin
        plugin.start()
        if self.world.gcd.server is self:
            self.world.at(2, self.world.gcd.service_added, device.id)

    def remove_service(self, ident):
        plugin = self.services.pop(ident, None)
        if plugin:
            plugin.cancel(closes_after=0.2)
            if self.world.gcd.server is self:
                self.world.at(1, self.world.gcd.service_removed, ident)

    def die(self, closes_lost=False):
        self.alive = False
        for plugin in self.services.values():
            plugin.cancel(closes_after=None if closes_lost else 80)
        self.services = {}
        self.world.gcd.server = None


class Gamecontrollerd:
    def __init__(self, world):
        self.world = world
        self.server, self.cache = None, {}
        self.known, self.parked = set(), {}
        self.claimed, self.pending, self.physical = set(), {}, {}

    def server_up(self, server):  # __IOHIDEventSystemClientRefresh -> SetMatchingMultiple
        if not server.alive:
            return
        old, self.server = self.cache, server
        self.cache = {ident: old.get(ident, object()) for ident in server.services}
        for ident in server.services:
            if ident not in old:  # RefreshServiceCallback: only IDs the cache did not hold
                self.service_added(ident)

    def service_added(self, ident):  # "HID Service Added" -> claimHIDService:
        device = self.world.devices.get(ident)
        if not device or ident in self.claimed:
            return
        self.cache.setdefault(ident, object())
        self.known.add(ident)
        self.claimed.add(ident)
        self.pending[ident] = {'identifier': device.serial}
        for connection in self.parked.pop(ident, []):
            self.accept(connection)

    def service_removed(self, ident):  # "HID Service Removed" -> relinquishHIDService:
        self.cache.pop(ident, None)
        self.relinquish(ident)

    def relinquish(self, ident):
        self.claimed.discard(ident)
        self.pending.pop(ident, None)
        self.physical.pop(ident, None)

    def incoming(self, connection):  # acceptIncomingDriverConnection: block .323
        ident = connection[0].device.id
        if ident not in self.known:
            self.parked.setdefault(ident, []).append(connection)
        else:
            self.accept(connection)

    def accept(self, connection):  # acceptDriverConnection:/acceptFilterConnection:forHIDService:
        plugin, kind, _ = connection
        ident = plugin.device.id
        pending = self.pending.get(ident) if ident in self.claimed else None
        if pending is None:  # objectForKey: nil -> -[connection invalidate]
            self.world.rejected += 1
            plugin.interrupted(connection)
            return
        pending[kind] = connection
        if kind == 'driver':  # "[JoyCon Physical Device (Pending)] Prepare Device", then block .109
            for other, device in list(self.physical.items()):  # block .110: the same identifier
                if device['identifier'] == pending['identifier']:
                    self.relinquish(other)
            self.physical[ident] = {'identifier': pending['identifier'], 'driver': connection}
            del self.pending[ident]

    def peer_invalidated(self, connection):  # the device's invalidation handler only clears the connection
        for device in self.physical.values():
            if device['driver'] is connection:
                device['driver'] = None


class World:
    def __init__(self, lib, rule):
        self.lib, self.rule = lib, rule
        self.now, self.queue, self.order = 0.0, [], itertools.count()
        self.clients = itertools.count(0x1000)
        self.next_id = 0x100000256
        self.devices, self.backboardd = {}, None
        self.connections = self.rejected = 0
        self.rail, self.gcd = Rail(self), Gamecontrollerd(self)

    def at(self, delay, fn, *args):
        heapq.heappush(self.queue, (self.now + delay, next(self.order), fn, args))

    def run(self, until):
        while self.queue and self.queue[0][0] <= until and self.connections < BUDGET:
            self.now, _, fn, args = heapq.heappop(self.queue)
            fn(*args)
        self.now = max(self.now, until) if self.connections < BUDGET else self.now

    def start_backboardd(self, pid):
        self.backboardd = Backboardd(self, pid)
        self.backboardd.start()

    def boot(self):
        for side in 'LR':
            self.rail.publish(side)
        self.start_backboardd(52)
        self.run(2000)

    def respring(self, pid, closes_lost=False, gap=400):
        self.backboardd.die(closes_lost)
        self.run(self.now + gap)
        self.start_backboardd(pid)
        self.run(self.now + 5000)

    def healthy(self):
        """gamecontrollerd has one live controller per seated Joy-Con: the current device, driven by the plugin of
        the backboardd that runs now."""
        devices = {device.id: device for device in self.rail.devices.values()}
        return (set(self.gcd.physical) == set(devices) and not self.gcd.pending and
                all(device['driver'] and device['driver'][0].server is self.backboardd
                    for device in self.gcd.physical.values()))


@unittest.skipUnless((JOYCON / 'SwitchJoyConSession.h').is_file(), 'needs drivers/SwitchJoyCon')
class SwitchJoyConSessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='ioswitch-joycon-')
        library = Path(cls.directory.name) / 'libsession.dylib'
        subprocess.run(['clang', *FLAGS, '-shared', '-fPIC', str(HARNESS), '-o', str(library)], check=True)
        lib = cls.lib = ctypes.CDLL(str(library))
        lib.jc_session_open.argtypes = [ctypes.POINTER(Session), ctypes.c_size_t, ctypes.c_int32, ctypes.c_int]
        lib.jc_session_close.argtypes = [ctypes.POINTER(Session), ctypes.c_size_t]
        lib.jc_republish_allowed.argtypes = [ctypes.POINTER(Guard), ctypes.c_int32, ctypes.c_uint64]
        lib.jc_republish_gap_ms.restype = ctypes.c_uint
        lib.jc_session_size.restype = lib.jc_guard_size.restype = ctypes.c_size_t

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def open(self, session, client, pid, backboardd=True):
        return self.lib.jc_session_open(ctypes.byref(session), client, pid, int(backboardd))

    def close(self, session, client):
        return self.lib.jc_session_close(ctypes.byref(session), client)

    def test_layout(self):
        self.assertEqual(self.lib.jc_session_size(), ctypes.sizeof(Session))
        self.assertEqual(self.lib.jc_guard_size(), ctypes.sizeof(Guard))

    def test_first_backboardd_client_is_the_session(self):
        session = Session()
        self.assertEqual(self.open(session, 0, 0), NONE)  # a kernel client (IOHIDEventDummyService)
        self.assertEqual(self.open(session, 0x10, 27, backboardd=False), NONE)  # SpringBoard
        self.assertEqual(session.client, 0)
        self.assertEqual(self.open(session, 0x20, 52), NONE)
        self.assertEqual((session.client, session.pid), (0x20, 52))
        self.assertEqual(self.open(session, 0x21, 52), NONE)  # another client of the same backboardd
        self.assertEqual(self.close(session, 0x21), NONE)
        self.assertEqual(self.close(session, 0x10), NONE)
        self.assertEqual(self.close(session, 0), NONE)
        self.assertFalse(session.ended)

    def test_close_by_the_serving_client_ends_it_once(self):
        session = Session()
        self.open(session, 0x20, 52)
        self.assertEqual(self.close(session, 0x20), CLOSED)
        self.assertEqual(session.pid, 52)
        self.assertEqual(self.close(session, 0x20), NONE)
        self.assertEqual(self.open(session, 0x30, 272), NONE)  # the device is about to be replaced

    def test_a_different_backboardd_ends_it(self):
        session = Session()
        self.open(session, 0x20, 52)
        self.assertEqual(self.open(session, 0x30, 272), REPLACED)
        self.assertEqual(session.pid, 52)  # the session that ended
        self.assertEqual(self.close(session, 0x20), NONE)  # the late close of the old one
        self.assertEqual(self.open(session, 0x31, 300), NONE)

    def test_guard(self):
        gap = self.lib.jc_republish_gap_ms()
        self.assertEqual(gap, 10000)
        guard = Guard()
        allowed = lambda pid, now: bool(self.lib.jc_republish_allowed(ctypes.byref(guard), pid, now))  # noqa: E731
        self.assertTrue(allowed(52, 0))  # the first, even at boot time 0
        self.assertTrue(allowed(272, 1))  # a new backboardd at once: always
        self.assertTrue(allowed(300, 2))
        self.assertFalse(allowed(300, 2 + gap - 1))  # the same backboardd again within the gap
        guard = Guard()
        self.assertTrue(allowed(52, 1000))
        self.assertTrue(allowed(52, 1000 + gap))

    def test_storm_reproduced_without_the_rule(self):
        world = World(self.lib, rule=False)
        world.boot()
        self.assertTrue(world.healthy())
        self.assertEqual((world.connections, world.rejected), (4, 0))
        world.respring(272)
        # Same registry IDs: no Added, no claim, no pending device, every connection refused and redone.
        self.assertGreaterEqual(world.connections, BUDGET)
        self.assertGreater(world.rejected, BUDGET - 10)
        self.assertFalse(world.healthy())

    def test_respring_with_the_rule(self):
        world = World(self.lib, rule=True)
        world.boot()
        before = {side: device.id for side, device in world.rail.devices.items()}
        self.assertEqual(world.rail.republished, 0)
        world.respring(272)
        self.assertEqual(world.rejected, 0)
        self.assertEqual(world.connections, 8)
        self.assertEqual(world.rail.republished, 2)
        self.assertTrue(all(world.rail.devices[side].id != before[side] for side in 'LR'))
        self.assertTrue(world.healthy())
        self.assertFalse(set(before.values()) & set(world.gcd.physical))  # block .110 retired the stale ones
        world.respring(300, gap=100)  # again at once, as a crash on the next screen recording would
        self.assertEqual(world.rejected, 0)
        self.assertTrue(world.healthy())
        self.assertFalse(world.rail.off)

    def test_respring_whose_closes_are_lost(self):
        world = World(self.lib, rule=True)
        world.boot()
        world.respring(272, closes_lost=True)
        # The new backboardd's open of the old device ends its session; the old device's plugin may be refused a
        # few times before its service is removed, never more.
        self.assertLessEqual(world.rejected, 4)
        self.assertLess(world.connections, 20)
        self.assertEqual(world.rail.republished, 2)
        self.assertTrue(world.healthy())

    def test_display_cycle_is_not_a_session_end(self):
        world = World(self.lib, rule=True)
        world.boot()
        for side in 'LR':  # pause() then run(): the rail's own destroy detaches the device before its closes
            world.rail.destroy(side)
        world.run(world.now + 100)
        for side in 'LR':
            world.rail.publish(side)
        world.run(world.now + 2000)
        self.assertEqual(world.rail.republished, 0)
        self.assertEqual(world.rejected, 0)
        self.assertTrue(world.healthy())

    def test_a_plugin_that_drops_every_device_turns_republishing_off(self):
        world = World(self.lib, rule=True)
        world.boot()
        server = world.backboardd
        for _ in range(3):  # the same backboardd closing the devices it serves, again and again
            for plugin in list(server.services.values()):
                world.at(0, plugin.device.close, plugin.client)
            world.run(world.now + 500)
        self.assertTrue(world.rail.off)
        self.assertEqual(world.rail.republished, 2)  # once per Joy-Con, then refused
        self.assertEqual(len(world.rail.logged), 1)


if __name__ == '__main__':
    unittest.main()
