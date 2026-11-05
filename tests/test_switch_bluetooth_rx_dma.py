"""drivers/SwitchBluetooth's RX by APB DMA (SwitchBluetooth.cpp header "RX by APB DMA", SwitchBluetoothRxDma.h) as a
byte-timed host model of UART-D, driven by the driver's own framing and transfer-length code (the header compiled
through ctypes), at the peak load: two Joy-Cons' 59-byte HID reports at 120 Hz each, AirPods' A2DP (300-byte ACL bursts
and the Number Of Completed Packets events for what we send), and LE scanning for AirDrop (43-byte advertising reports),
all at once, the controller sending packets back to back whenever they queue up.

Against the PIO path as it is (tests/test_switch_bluetooth_rx.py's model, RX trigger 16) it checks: every byte reaches
the ring once and in order; the FIFO never needs RTS to hold the chip; far fewer interrupts and far less interrupt time
on CPU0; and no HID report's last byte waits longer than under PIO (an isolated packet is taken by EORD, 4 characters
after its last byte, as PIO's RX timeout; one followed at once by another by the end of count of a transfer that ends 5
bytes past it, where PIO's 16-byte trigger needs up to 16 more). Also: framing lost keeps transfers at the reach; a
transfer's visible words never go back; a guard that the driver uses the header and programs DMA mode as modelled.

Costs (microseconds, assumptions, the same entry/exit as the PIO model): interrupt entry 3.0 and exit 1.0; a UART or APB
DMA register read 0.25, write 0.1; a byte taken from memory 0.01, a cache line invalidated 0.03."""

import ctypes
import importlib.util
import random
import subprocess
import tempfile
import unittest
from collections import deque
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BLUETOOTH = ROOT / 'drivers/SwitchBluetooth'
DRIVER = BLUETOOTH / 'SwitchBluetooth.cpp'
APBDMA = ROOT / 'drivers/TegraAPBDMA/TegraAPBDMA.cpp'
HARNESS = ROOT / 'tests/switch_bluetooth_rx_dma_harness.c'
FLAGS = [
    '-std=c11',
    '-O1',
    '-g',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wconversion',
    '-Wsign-conversion',
    '-I',
    str(BLUETOOTH),
]

_spec = importlib.util.spec_from_file_location('pio_model', ROOT / 'tests/test_switch_bluetooth_rx.py')
PIO = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(PIO)

BYTE = PIO.BYTE  # us per byte at 3 Mbaud 8N1
TIMEOUT = PIO.TIMEOUT  # 4 characters: the RX timeout (PIO) and EORD (DMA mode)
ENTRY, EXIT = 3.0, 1.0
READ, WRITE, MEM, LINE = 0.25, 0.1, 0.01, 0.03
DMA_LATENCY = 0.5  # a byte from the FIFO to memory once the channel may take it
FIFO = 32
SECONDS = 1.0


class Lib:
    lib = None


def setUpModule():
    Lib.directory = tempfile.TemporaryDirectory(prefix='ioswitch-btrxdma-')
    library = Path(Lib.directory.name) / 'libbtrxdma.dylib'
    subprocess.run(['clang', *FLAGS, '-shared', '-fPIC', str(HARNESS), '-o', str(library)], check=True)
    lib = Lib.lib = ctypes.CDLL(str(library))
    u8, u32 = ctypes.c_uint8, ctypes.c_uint32
    lib.bt_frame_byte.argtypes = [u8]
    lib.bt_frame_left.argtypes = [ctypes.POINTER(u32)]
    lib.bt_rx_dma_length.restype = u32
    lib.bt_rx_dma_visible.argtypes = [u32, u32, ctypes.c_int, u32]
    lib.bt_rx_dma_visible.restype = u32
    lib.bt_rx_dma_reach.restype = u32
    lib.bt_rx_dma_words.restype = u32


# --- Traffic ---------------------------------------------------------------------------------------------------------
def acl(handle, payload):
    return bytes([2, handle & 0xFF, handle >> 8 | 0x20, len(payload) & 0xFF, len(payload) >> 8]) + payload


def event(code, params):
    return bytes([4, code, len(params)]) + params


def peak(seconds=SECONDS, seed=1):
    """(nominal start us, kind, bytes) for the peak mix, sorted."""
    rng = random.Random(seed)
    packets = []
    for handle, phase in ((0x0040, 0.0), (0x0041, 4100.0)):  # two Joy-Cons, 120 Hz, a 0x30 report each
        t = phase
        while t < seconds * 1e6:
            packets.append((t + rng.uniform(-400, 400), 'hid', acl(handle, bytes([0xA1, 0x30]) + bytes(52))))
            t += 1e6 / 120
    t = 0.0
    while t < seconds * 1e6:  # AirPods: 300-byte ACL bursts and completions for what we send, ~150 a second each
        packets.append((t + rng.uniform(0, 500), 'acl', acl(0x0042, bytes(295))))
        packets.append((t + rng.uniform(0, 6000), 'nocp', event(0x13, bytes([1, 0x42, 0, 1, 0]))))
        t += 1e6 / 150
    t = 0.0
    while t < seconds * 1e6:  # LE scanning in a crowded room: ~300 advertising reports a second
        t += rng.expovariate(300 / 1e6)
        packets.append((t, 'adv', event(0x3E, bytes([2, 1]) + bytes(38))))
    packets.sort(key=lambda p: p[0])
    return packets


def serialize(packets):
    """The controller sends one packet at a time: each starts when it is due or when the last has gone, back to back.
    Returns the arrival list [(time, packet, last, value)] and each packet's (start, length, kind)."""
    arrivals, spans = [], []
    free = 0.0
    for index, (due, kind, data) in enumerate(packets):
        start = max(due, free)
        for b, value in enumerate(data):
            arrivals.append((start + (b + 1) * BYTE, index, b == len(data) - 1, value))
        free = start + len(data) * BYTE
        spans.append((start, len(data), kind))
    return arrivals, spans


# --- DMA mode --------------------------------------------------------------------------------------------------------
def simulate_dma(arrivals, lib):
    """The driver's DMA mode on one CPU: a transfer runs from word 0 with the length the header gives; the CPU takes
    what moved at EORD (4 characters of idle after data) and at the end of count, which also starts the next transfer.
    Returns per byte the time it was taken, interrupt counts, CPU time, the FIFO's peak and the order of bytes taken."""
    lib.bt_frame_reset()
    n = len(arrivals)
    taken_at = [None] * n
    order = []
    fifo = deque()
    trans, length = [], int(lib.bt_rx_dma_length())
    taken = 0
    eoc = None  # time the transfer's last word landed (end of count pending)
    eord = None  # time EORD latched (line idle 4 characters after data)
    last_rx, data_since = -1e18, False
    cpu_free, cpu = 0.0, 0.0
    irqs = {'eord': 0, 'eoc': 0}
    fifo_peak = 0
    next_free = 0.0  # when the channel can move the next byte
    i = 0

    def move(now):
        nonlocal eoc, next_free
        t = max(now, next_free)
        while fifo and len(trans) < length:
            t += DMA_LATENCY
            trans.append(fifo.popleft())
            if len(trans) == length:
                eoc = t
        next_free = t

    def take(now):
        nonlocal taken
        count = len(trans) - taken
        for k in trans[taken:]:
            taken_at[k] = now
            order.append(k)
            lib.bt_frame_byte(arrivals[k][3])
        taken = len(trans)
        return count * MEM + ((count * 4 + 63) // 64) * LINE

    while True:
        t_arr = arrivals[i][0] if i < n else float('inf')
        due = [x for x in (eoc, eord) if x is not None]
        if data_since and eord is None:
            due.append(last_rx + TIMEOUT)
        t_irq = min(due) if due else float('inf')
        start = max(t_irq, cpu_free)
        if t_arr == float('inf') and t_irq == float('inf'):
            break
        if t_arr <= start:
            if data_since and eord is None and t_arr >= last_rx + TIMEOUT:
                eord = last_rx + TIMEOUT
            fifo.append(i)
            fifo_peak = max(fifo_peak, len(fifo))
            last_rx, data_since = t_arr, True
            move(t_arr)
            i += 1
            continue
        if data_since and eord is None and last_rx + TIMEOUT <= start:
            eord = last_rx + TIMEOUT
        now = start + ENTRY
        if eoc is not None and (eord is None or eoc <= eord):  # the channel's end of count
            irqs['eoc'] += 1
            cost = 2 * READ + WRITE + take(now)
            trans, taken, eoc = [], 0, None
            length = int(lib.bt_rx_dma_length())
            cost += READ + 2 * WRITE + 7 * WRITE  # stop (CSR, STATUS) and the seven writes that start a transfer
            move(now + cost)
        else:  # EORD: mask/unmask (3 IER writes), IIR twice, the channel's count and status, LSR
            irqs['eord'] += 1
            cost = 2 * READ + 3 * WRITE + 2 * READ + READ
            if eoc is not None and eoc <= now:
                cost += take(now)
            else:
                cost += take(now)
            eord = None
            data_since = last_rx > (start - TIMEOUT) and last_rx + TIMEOUT > start
        cpu += ENTRY + cost + EXIT
        cpu_free = now + cost + EXIT
    return taken_at, irqs, cpu, fifo_peak, order


def latencies(arrivals, spans, taken_at, kind):
    out = []
    for k, (t, packet, last, _) in enumerate(arrivals):
        if last and spans[packet][2] == kind:
            out.append(taken_at[k] - t)
    return out


def pio(spans):
    """PIO as it is (tests/test_switch_bluetooth_rx.py's model at the 16-byte trigger) on the same serialized stream."""
    irqs, per = PIO.run([(start, length) for start, length, _ in spans], 16)
    total = sum(length for _, length, _ in spans)
    cpu = irqs * (ENTRY + EXIT + 2 * READ) + total * 0.35  # IIR twice per interrupt; LSR + RBR per byte (0.35)
    waits = {i: w for i, (_, w) in per.items()}
    return irqs, cpu, waits


def percentile(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))]


class PeakLoad(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.packets = peak()
        cls.arrivals, cls.spans = serialize(cls.packets)
        cls.dma = simulate_dma(cls.arrivals, Lib.lib)
        cls.pio = pio(cls.spans)
        taken_at, irqs, cpu, fifo_peak, _ = cls.dma
        pio_irqs, pio_cpu, waits = cls.pio
        hid_dma = latencies(cls.arrivals, cls.spans, taken_at, 'hid')
        hid_pio = [waits[i] for i, s in enumerate(cls.spans) if s[2] == 'hid']
        busy = sum(length for _, length, _ in cls.spans) * BYTE / (SECONDS * 1e6)
        print(
            f'\nBluetooth RX at peak ({len(cls.spans)} packets/s, line {busy:.0%} busy): '
            f'PIO {pio_irqs} interrupts/s, {pio_cpu / 1e4:.2f}% of CPU0; '
            f'DMA {irqs["eord"] + irqs["eoc"]} interrupts/s ({irqs["eord"]} EORD, {irqs["eoc"]} end of count), '
            f'{cpu / 1e4:.2f}% of CPU0; FIFO peak {fifo_peak}; HID report last-byte wait us PIO p50/p99/max '
            f'{percentile(hid_pio, 0.5):.1f}/{percentile(hid_pio, 0.99):.1f}/{max(hid_pio):.1f}, DMA '
            f'{percentile(hid_dma, 0.5):.1f}/{percentile(hid_dma, 0.99):.1f}/{max(hid_dma):.1f}'
        )

    def test_every_byte_once_in_order(self):
        taken_at, _, _, _, order = self.dma
        self.assertEqual(order, list(range(len(self.arrivals))))
        self.assertTrue(all(t is not None for t in taken_at))

    def test_fifo_never_needs_rts(self):
        self.assertLess(self.dma[3], FIFO // 2)

    def test_fewer_interrupts_and_less_cpu(self):
        _, irqs, cpu, _, _ = self.dma
        pio_irqs, pio_cpu, _ = self.pio
        self.assertLess(irqs['eord'] + irqs['eoc'], 0.6 * pio_irqs)
        self.assertLess(cpu, 0.35 * pio_cpu)

    def test_no_hid_report_waits_longer(self):
        taken_at = self.dma[0]
        _, _, waits = self.pio
        hid_dma = latencies(self.arrivals, self.spans, taken_at, 'hid')
        hid_pio = [waits[i] for i, s in enumerate(self.spans) if s[2] == 'hid']
        self.assertLessEqual(max(hid_dma), max(hid_pio))
        self.assertLessEqual(percentile(hid_dma, 0.99), percentile(hid_pio, 0.99))
        # The bound by construction: EORD 4 characters on, or a transfer's end the reach on, then the interrupt, behind
        # at most one other handler.
        bound = max(TIMEOUT, Lib.lib.bt_rx_dma_reach() * BYTE) + 2 * (ENTRY + EXIT + 4.0) + DMA_LATENCY
        every = latencies(self.arrivals, self.spans, taken_at, 'hid') + latencies(
            self.arrivals, self.spans, taken_at, 'adv'
        )
        self.assertLess(max(every), bound)


class Transfers(unittest.TestCase):
    def setUp(self):
        Lib.lib.bt_frame_reset()

    def feed(self, data):
        return [Lib.lib.bt_frame_byte(b) for b in data]

    def test_lengths_follow_the_framing(self):
        lib, reach = Lib.lib, Lib.lib.bt_rx_dma_reach()
        self.assertEqual(lib.bt_rx_dma_length(), reach)  # between packets
        report = acl(0x40, bytes(54))
        self.feed(report[:16])  # a transfer of 16 ended inside the report: its end and the reach past it
        self.assertEqual(lib.bt_rx_dma_length(), len(report) - 16 + reach)
        self.assertEqual(self.feed(report[16:])[-1], 1)
        self.assertEqual(lib.bt_rx_dma_length(), reach)
        self.feed(event(0x0E, bytes(4))[:2])  # in an event's header: its remainder (the length byte), then the reach
        self.assertEqual(lib.bt_rx_dma_length(), 1 + reach)
        big = acl(0x40, bytes(4000))
        Lib.lib.bt_frame_reset()
        self.feed(big[:5])
        self.assertEqual(lib.bt_rx_dma_length(), lib.bt_rx_dma_words())  # within the buffer

    def test_lost_framing_keeps_the_reach(self):
        self.feed(bytes([0x07]))  # no H4 packet type
        self.assertEqual(Lib.lib.bt_rx_dma_length(), Lib.lib.bt_rx_dma_reach())
        Lib.lib.bt_frame_reset()
        self.feed(acl(0x40, bytes(100))[:10])
        Lib.lib.bt_frame_lose()
        self.assertEqual(Lib.lib.bt_rx_dma_length(), Lib.lib.bt_rx_dma_reach())

    def test_packet_ends(self):
        stream = (
            event(0x0E, bytes([1, 3, 0x0C, 0])) + acl(0x40, bytes(3)) + event(0x13, b'') + bytes([3, 1, 0, 2, 9, 9])
        )
        ends = [i for i, done in enumerate(self.feed(stream)) if done]
        self.assertEqual(ends, [6, 14, 17, 23])

    def test_visible_words(self):
        v = Lib.lib.bt_rx_dma_visible
        self.assertEqual(v(40, 12 * 4, 0, 0), 12)
        self.assertEqual(v(40, 0, 1, 12), 40)  # ended: the whole transfer
        self.assertEqual(v(40, 0, 0, 12), 12)  # the count read as reloaded: never back
        self.assertEqual(v(40, 4 * 60, 0, 0), 40)  # never past the transfer


class DriverGuard(unittest.TestCase):
    def test_driver_uses_the_header_as_modelled(self):
        source = DRIVER.read_text()
        self.assertIn('#include "SwitchBluetoothRxDma.h"', source)
        self.assertIn('switch_bt_h4_frame_byte(&rxFrame, byte)', source)
        self.assertIn('rxDmaLength = switch_bt_rx_dma_length(&rxFrame);', source)
        self.assertIn('FCR_ENABLE | FCR_DMA | FCR_RX_TRIG_1 | FCR_TX_TRIG_16', source)
        self.assertIn('FCR_RX_TRIG_1 = 0u << 6', source)
        self.assertIn('IER_RX_DMA = IER_RLSI | IER_RTOIE | IER_EORD', source)
        self.assertIn('tegra_apbdma_start_rx_words(rxDma', source)
        # The channel reads RBR 32 bits a word, one request a word.
        apb = APBDMA.read_text()
        body = apb[apb.index('void tegra_apbdma_start_rx_words') :]
        body = body[: body.index('\n}\n')]
        self.assertIn('APBSEQ_BUS_WIDTH_32 | APBSEQ_WRAP_WORD_1', body)
        self.assertIn('AHBSEQ_BURST_1', body)
        self.assertNotIn('CSR_DIR', body.replace('// device to memory', ''))


if __name__ == '__main__':
    unittest.main()
