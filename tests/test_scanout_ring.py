"""drivers/TegraDisplayFlip/TegraScanoutRing.h (window A's three scanout buffers, latest frame wins) on the host, against a model
of DC A's frame-boundary latch in tests/scanout_ring_harness.cpp: no frame written into a buffer DC A scans or may take, a frame
ready before a boundary on screen at it, every latch state read right, and one late frame no longer leaving every later frame a
frame behind (it does with two buffers, the r7q pattern); against a controller that does not take a replaced START_ADDR as
modelled, one fallback to double buffering with presents going on and no START_ADDR stored while a request is out."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DISPLAY = ROOT / "out/native-a57/root-v1/tree/drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp"


class ScanoutRingTests(unittest.TestCase):
    def test_ring_against_latch_model(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "scanout-ring"
            compile_result = subprocess.run(
                ["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                 str(ROOT / "tests/scanout_ring_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: scanout ring", result.stdout)
            self.assertIn("0 with three", result.stdout)
            self.assertIn("fell back to double buffering in 200 of 200 runs", result.stdout)

    def test_display_uses_the_ring(self):
        """Every flip, latch check and copy target in the display goes through the ring: the two-buffer state is gone."""
        source = DISPLAY.read_text()
        self.assertIn('#include "../TegraDisplayFlip/TegraScanoutRing.h"', source)
        for gone in ("flipPending", "front ^ 1", "front ^= 1"):
            self.assertNotIn(gone, source)
        flip = source.split("    void flip()\n", 1)[1].split("\n    }\n", 1)[0]
        present = source.split("    void presentEntry(int i)\n", 1)[1].split("\n    }\n", 1)[0]
        self.assertIn("presentEntry(target)", flip)  # r14-direct: scan buffers and direct slots share presentEntry
        self.assertIn("ring.ready(", present)
        check = source.split("    void checkFlip(bool atFrameEnd)\n", 1)[1].split("\n    }\n", 1)[0]
        self.assertIn("ring.latch(", check)
        self.assertIn("panic(", check)  # a flip never taken (a lost frame end) is still one line and a panic
        # A START_ADDR the model does not explain while flips are replaced is one line and a fallback (never-booted rule); only
        # once double-buffered again is it the old panic.
        self.assertIn("recoverFlip(", check)
        self.assertIn("if (!ring.replace)", check)


if __name__ == "__main__":
    unittest.main()
