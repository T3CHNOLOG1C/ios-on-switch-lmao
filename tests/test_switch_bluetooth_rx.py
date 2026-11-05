"""drivers/SwitchBluetooth's RX path on UART-D, as a host model (no driver code runs here): interrupts per HCI packet
and how long a packet's last byte waits in the FIFO, for the RX trigger the driver uses ("more than 16", FCR 0xC0, Linux
serial-tegra's PIO setting) against the old one ("more than 8", FCR 0x80); and a guard that the driver programs that
trigger with hardware flow control on.

UART (Tegra 16550, 32-byte FIFO, 3 Mbaud 8N1): a byte lands every 10 bit times (3.33 us). The data interrupt is raised
once the FIFO holds more than the trigger; the RX timeout once it holds anything and no byte has arrived for 4
character times. The filter (SwitchBluetooth::filter -> drainRx) starts `entry` after the line rises, reads LSR+RBR
per byte (`perByte`), keeps going while LSR shows data (so it takes bytes that land meanwhile), re-reads IIR and
returns (`exit`). RTS flow control is not modelled: it only pauses the chip at a full FIFO, which these rates never
reach."""
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVER = ROOT / "out/native-a57/root-v1/tree/drivers/SwitchBluetooth/SwitchBluetooth.cpp"
BYTE = 10 / 3e6 * 1e6  # us per byte at 3 Mbaud, 8N1
TIMEOUT = 4 * BYTE


def run(packets, trigger, entry=3.0, perByte=0.35, exit_=1.0):
    """packets: list of (start_us, length). Returns (interrupts, per-packet [interrupts, wait of the last byte])."""
    arrivals = []  # (time, packet index, is last byte)
    for i, (start, length) in enumerate(packets):
        for b in range(length):
            arrivals.append((start + (b + 1) * BYTE, i, b == length - 1))
    arrivals.sort()
    fifo = []  # arrival times of bytes in the FIFO (with packet index, last flag)
    t = 0.0
    k = 0
    interrupts = 0
    perPacket = {i: [0, None] for i in range(len(packets))}
    busyUntil = 0.0
    lastArrival = -1e9
    while k < len(arrivals) or fifo:
        # Next byte arrival, and when the interrupt line would rise with the FIFO as it is.
        nextArrival = arrivals[k][0] if k < len(arrivals) else float("inf")
        rise = float("inf")
        if len(fifo) > trigger:
            rise = max(t, busyUntil)
        elif fifo:
            rise = max(lastArrival + TIMEOUT, busyUntil)
        if nextArrival <= rise:
            t = nextArrival
            fifo.append(arrivals[k])
            lastArrival = t
            k += 1
            continue
        # The interrupt: the filter drains until the FIFO is empty, taking bytes that land while it runs.
        t = rise + entry
        interrupts += 1
        touched = set()
        while True:
            while k < len(arrivals) and arrivals[k][0] <= t:
                fifo.append(arrivals[k])
                lastArrival = arrivals[k][0]
                k += 1
            if not fifo:
                break
            at, pkt, last = fifo.pop(0)
            t += perByte
            touched.add(pkt)
            if last:
                perPacket[pkt][1] = t - at
        for pkt in touched:
            perPacket[pkt][0] += 1
        t += exit_
        busyUntil = t
    return interrupts, perPacket



def joycons(hz):
    """Two Joy-Cons' HID input reports 0x30 (H4 1 + ACL 4 + L2CAP 4 + 0xA1 + 49 = 59 bytes) at `hz` each."""
    return [(i * (1e6 / (2 * hz)), 59) for i in range(4 * hz)]


class BluetoothRxTests(unittest.TestCase):
    def per_packet(self, packets, trigger):
        irqs, per = run(packets, trigger)
        return irqs / len(packets), max(w for _, w in per.values()), per

    def test_fewer_interrupts_per_packet(self):
        cases = {
            "Joy-Con reports at 2 x 60 Hz": (joycons(60), 0.70),
            "Joy-Con reports at 2 x 120 Hz": (joycons(120), 0.70),
            "LE advertising reports": ([(i * 45000, 43) for i in range(40)], 0.70),
            "300-byte ACL bursts": ([(i * 2000, 300) for i in range(50)], 0.60),
        }
        for name, (packets, ratio) in cases.items():
            old, _, _ = self.per_packet(packets, 8)
            new, _, _ = self.per_packet(packets, 16)
            self.assertLessEqual(new, old * ratio, f"{name}: {old:.2f} -> {new:.2f} interrupts per packet")

    def test_no_wait_beyond_the_rx_timeout(self):
        # A packet's last byte is taken by a data interrupt or by the RX timeout, 4 characters after it: never later
        # than that timeout plus the filter's entry and its drain, at either trigger.
        bound = TIMEOUT + 3.0 + 64 * 0.35
        for packets in (joycons(60), joycons(120), [(i * 1000, 7 + (i & 1)) for i in range(200)],
                        [(i * 45000, 43) for i in range(40)]):
            for trigger in (8, 16):
                _, worst, per = self.per_packet(packets, trigger)
                self.assertLess(worst, bound)
                self.assertTrue(all(w is not None for _, w in per.values()), "every packet's last byte is taken")
            # The new trigger's worst case is the old one's (both end on the RX timeout).
            self.assertLessEqual(self.per_packet(packets, 16)[1], self.per_packet(packets, 8)[1] + TIMEOUT)

    def test_driver_programs_the_trigger(self):
        source = DRIVER.read_text()
        self.assertNotIn("FCR_RX_TRIG_8", source)
        fcr_writes = [line for line in source.splitlines() if "FCR_RX_TRIG_16 | FCR_TX_TRIG_16" in line]
        self.assertEqual(len(fcr_writes), 3, "setupUart, resetRxFifoLocked and leaving RX DMA mode")
        self.assertIn("FCR_RX_TRIG_16 = 3u << 6", source)
        self.assertIn("MCR_CTS_EN | MCR_RTS_EN | MCR_RTS", source)  # flow control holds the chip past a full FIFO


if __name__ == "__main__":
    unittest.main()
