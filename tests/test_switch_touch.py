"""drivers/SwitchTouch over drivers/TegraI2C on the host (r17-touch): both real drivers, each its own translation unit,
compiled against tests/switch_touch_stubs.h over the models in tests/switch_touch_harness.cpp (Tegra210 I2C controllers
in packet mode, the FTM4 on I2C3, the MAX77620 on I2C5, APB DMA channel 4). One scenario per process (the drivers keep
boot-wide state):
  drag            1-finger drag at 120 Hz: per touch interrupt exactly the count read and the event read, each one chained
                  submission and one controller interrupt; reports stamped with the filter's time, strictly increasing;
                  the stats line (report interval, spans, I2C interrupts per interrupt)
  five-dma        5 fingers (40-byte event reads) by DMA: 3 interrupt actions per touch interrupt (the channel's end
                  of count before the controller's completion, which follows the STOP)
  five-dma-early  the same with the completion first
  five-pio        the same with a DMA channel that never moves: the check fails with one line, reads by PIO (11 actions)
  same-batch      down and up in one scan: two reports, the second a tick later
  deep            40 events queued: 32, then 8 on the next interrupt (the line stays low)
  untimed         HID without report times: one line, reports stamped at the post
  nack            the panel stops answering: the one-line panic
  dma-first-lost  the channel's first end of count never arrives: one line, DMA off (never-booted rule), no panic
  dma-lost-later  after it has arrived once, a lost one panics
  chain-broken    a controller without ALL_PACKETS_DONE: the chain check fails with one line, one packet per message
  dma-open-fails  the channel cannot be had: one line, PIO
  measure         drags of 1, 2 and 5 fingers: transactions, interrupt actions and INT-to-report per touch interrupt
  filter          the touch-down slop: a tap inside 3 px reports no travel, one that creeps out of it is reported where
                  the finger is, a jittering hold reports nothing, a slow drag is reported from its first motion and
                  ends at the finger (no time held, no offset carried)
  palm            a contact turning into a palm is lifted alone, a palm from the start is never a contact
  edge            the 40 px edge band, and the geometry line's edge-swipe start
  four-fingers    four fingers over three scans: all four down together, the arrival spread in the geometry line
Plus source guards for what the models cannot see (no waits in the interrupt path, the APB DMA register values, the
unchanged TegraAPBDMA API, I2C3 alone chained)."""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVERS = ROOT / "out/native-a57/root-v1/tree/drivers"
TOUCH = DRIVERS / "SwitchTouch/SwitchTouch.cpp"
HID = DRIVERS / "SwitchTouch/SwitchHIDDevice.cpp"
I2C = DRIVERS / "TegraI2C/TegraI2C.cpp"
DMA_H = DRIVERS / "TegraAPBDMA/TegraAPBDMA.h"
DMA = DRIVERS / "TegraAPBDMA/TegraAPBDMA.cpp"
HEADERS = ["IOKit/IOFilterInterruptEventSource.h", "IOKit/IOInterruptEventSource.h", "IOKit/IOLib.h", "IOKit/IOLocks.h",
           "IOKit/IOService.h", "IOKit/IOTimerEventSource.h", "IOKit/IOWorkLoop.h", "IOKit/IOReturn.h", "kern/clock.h",
           "os/log.h", "pexpert/pexpert.h", "libkern/OSAtomic.h"]
SCENARIOS = ["drag", "five-dma", "five-dma-early", "five-pio", "same-batch", "deep", "untimed", "nack", "dma-first-lost",
             "dma-lost-later", "chain-broken", "dma-open-fails", "measure", "filter", "palm", "edge", "four-fingers"]
FLAGS = ["-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all", "-g"]


def body(source, signature):
    return source.split(signature, 1)[1].split("\n}\n", 1)[0]


class SwitchTouchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        temp = pathlib.Path(cls.directory.name)
        for name in HEADERS:
            path = temp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "' + str(ROOT / "tests/switch_touch_stubs.h") + '"\n')
        objects = []
        units = [(I2C, ["-DTEGRA_I2C_HOST_MODEL", "-include", str(ROOT / "tests/tegra_i2c_host_model.h")]),
                 (TOUCH, []), (ROOT / "tests/switch_touch_harness.cpp", [])]
        for source, extra in units:
            obj = temp / (source.stem + ".o")
            result = subprocess.run(["xcrun", "clang++", *FLAGS, "-I", str(temp), *extra, "-c", str(source), "-o",
                                     str(obj)], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if result.returncode:
                raise AssertionError(f"compiling {source.name}:\n{result.stdout}")
            objects.append(str(obj))
        cls.binary = temp / "switch-touch"
        result = subprocess.run(["xcrun", "clang++", *FLAGS, *objects, "-o", str(cls.binary)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            raise AssertionError(f"linking:\n{result.stdout}")

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_scenario(self, name):
        result = subprocess.run([self.binary, name], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(f"PASS: switch touch {name}", result.stdout)
        return result.stdout

    def test_scenarios(self):
        for name in SCENARIOS:
            with self.subTest(name):
                self.run_scenario(name)

    def test_measure(self):
        out = self.run_scenario("measure")
        rows = dict(re.findall(r"measure (\d)-finger: 62 interrupts, (.*)", out))
        for fingers, actions in (("1", "2.00"), ("2", "2.00"), ("5", "3.00")):
            self.assertIn(f"2.00 transactions and {actions} I2C3 interrupt actions", rows[fingers])
        latency = {k: int(re.search(r"mean (\d+) us", v).group(1)) for k, v in rows.items()}
        # Two chained transactions at 400 kHz after the work loop's 40 us: under 0.6 ms for one finger (main's three
        # transactions of two packets each took 1.04 ms in this model).
        self.assertLess(latency["1"], 650)

    def test_touch_interrupt_path_guards(self):
        source = TOUCH.read_text()
        action = body(source, "void SwitchTouch::interruptOccurred(")
        for wait in ("IOSleep", "IODelay", "while (", "for (unsigned round"):
            self.assertNotIn(wait, action)
        self.assertEqual(action.count("eventCount(&count)"), 1, "one count read per interrupt")
        self.assertEqual(action.count("CmdReadAllEvents"), 1, "one event read per interrupt")
        filt = body(source, "bool SwitchTouch::filterInterrupt(")
        self.assertIn("mach_absolute_time()", filt)
        self.assertIn("return true;", filt)
        self.assertIn("IOFilterInterruptEventSource::filterInterruptEventSource(", source)
        flush = body(source, "void SwitchTouch::flush(")
        self.assertIn("switch_hid_post_at(hid, &report, sizeof(report), stamp)", flush)
        self.assertIn("if (stamp <= lastStamp) stamp = lastStamp + 1;", flush)
        self.assertIn('os_log(OS_LOG_DEFAULT, "SwitchTouch stats: %s", line);', source)
        self.assertIn('os_log(OS_LOG_DEFAULT, "SwitchTouch geometry: %s", line);', source)
        handle = body(source, "void SwitchTouch::handle(")
        leave = handle.split("if (type == EvLeave)", 1)[1].split("return;\n    }", 1)[0]
        self.assertNotIn("position(", leave, "a leave's coordinates are noise (hekate)")
        self.assertIn("kPalmStrength = 500", source)  # Linux ftm4 PRESSURE_MAX, hekate's palm rest
        self.assertNotIn('IOLog("SwitchTouch stats', source, "stats never on the console")

    def test_timed_posts_keep_edge_gestures(self):
        source = HID.read_text()
        post_at = body(source, "    IOReturn postAt(")
        self.assertIn("installEdge();", post_at)
        self.assertIn("installEdge();", body(source, "    IOReturn post(const void *report, size_t length)"))

    def test_i2c_guards(self):
        source = I2C.read_text()
        table = source.split("constexpr BusInfo kBus[kBuses] = {", 1)[1].split("};", 1)[0]
        entries = [e for e in re.split(r"\},\s*(?://[^\n]*)?\s*", table) if "0x" in e]
        chained = [e.split(",")[0].strip(" {\n") for e in entries if ", true," in e]
        self.assertEqual(chained, ["0x0500"], "I2C3 alone is chained")
        chain = body(source, "int chain(unsigned bus, Bus &b,")
        self.assertNotIn("HDR_IE", chain, "no per-packet interrupt in a chain (L4T multi-packet)")
        self.assertIn("INT_ALL_PACKETS_DONE", chain)
        self.assertIn("INT_ALL_PACKETS_DONE = 1u << 6", source)
        self.assertIn("constexpr unsigned kFifoWords = 8;", source)
        # Chain and DMA are off until the owner's checks pass (never booted).
        self.assertIn("const bool chained = b.chainOn &&", source)
        self.assertIn("const bool drainAtEnd = in && b.chainOn &&", source)
        self.assertIn("b.dmaOn = on && b.dma && b.chainOn;", source)
        # A first DMA end of count that never arrived is reported, not panicked on.
        wait = body(source, "int sleepForAction(")
        self.assertLess(wait.index("!tegra_apbdma_interrupts(b.dma)"), wait.index("DMA end of count lost"))

    def test_apbdma_api_backward_compatible(self):
        header = DMA_H.read_text()
        for line in ("TEGRA_APBDMA_CH_UARTB_RX = 0", "TEGRA_APBDMA_CH_UARTC_RX = 1", "TEGRA_APBDMA_CH_UARTD_TX = 2",
                     "TEGRA_APBDMA_REQ_UARTB = 9", "TEGRA_APBDMA_REQ_UARTC = 10", "TEGRA_APBDMA_REQ_UARTD = 19",
                     "void tegra_apbdma_start_ring(struct tegra_apbdma_channel *ch, uint32_t apb, unsigned req, "
                     "uint32_t pa, uint32_t size);",
                     "void tegra_apbdma_start_once(struct tegra_apbdma_channel *ch, uint32_t apb, unsigned req, "
                     "uint32_t pa, uint32_t size);",
                     "bool tegra_apbdma_alloc(uint32_t size, struct tegra_apbdma_buffer *out);"):
            self.assertIn(line, header)
        self.assertIn("TEGRA_APBDMA_REQ_I2C3 = 23", header)  # tegra210.dtsi i2c@7000c500 dmas
        self.assertIn("TEGRA_APBDMA_CH_I2C3_RX = 4", header)  # 3: r17-iodma's UART-D RX
        rx = body(DMA.read_text(), "void tegra_apbdma_start_rx_words(")
        # Linux tegra20-apb-dma.c for DMA_DEV_TO_MEM, 4-byte slave width, maxburst 1: APB 32-bit with the address
        # fixed, AHB 32-bit single-word bursts, once, flow-controlled on the request line, no DIR bit.
        self.assertIn("APBSEQ_BUS_WIDTH_32 | APBSEQ_WRAP_WORD_1", rx)
        self.assertIn("AHBSEQ_INTR_ENB | AHBSEQ_BUS_WIDTH_32 | AHBSEQ_BURST_1", rx)
        self.assertIn("CSR_IE_EOC | CSR_ONCE | CSR_FLOW", rx)
        self.assertNotIn("CSR_DIR", rx)
        self.assertIn("APBSEQ_BUS_WIDTH_32 = 2u << 28", DMA.read_text())


if __name__ == "__main__":
    unittest.main()
