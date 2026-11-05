"""Direct scanout (NativeLegacyDisplay, r14-direct): the replace/retire policy against Core Animation's three framebuffers and
their recycling on swap completion (tests/direct_policy_harness.cpp: CA never draws into a surface DC A scans or may take,
no deadlock, every frame on screen at the first boundary after its rendering; retire-at-offer is caught as the hazard), and
source guards that the driver implements that policy: direct frames only after the SMMU probe passed, a fence-only GPU
present per frame, retirement at the latch in queue order never past the frame in flight, no early retire or collapse once
direct scanout is on, layouts never replaced across pitch and block-linear."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DISPLAY = ROOT / "drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp"
GPU = ROOT / "drivers/TegraGM20B/gpu.c"


def body(source, signature):
    return source.split(signature, 1)[1].split("\n    }\n", 1)[0]


class DirectScanoutTests(unittest.TestCase):
    def test_policy_against_core_animation(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "direct-policy"
            compile_result = subprocess.run(
                ["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                 str(ROOT / "tests/direct_policy_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: direct policy", result.stdout)

    def test_driver_follows_the_policy(self):
        source = DISPLAY.read_text()
        # only after the probe passed, in this boot
        start = body(source, "    bool startDirect()\n")
        self.assertIn("directAttached = directOn = latchRetire = true", start)
        self.assertIn("d->probe.stage == DisplaySmmuProbe::kDone && d->startDirect()", source)
        # a fence-only present, no retire at the offer
        offer = body(source, "    bool offerDirect(")
        self.assertIn("TEGRA_SCANOUT_COPY_FENCE_ONLY", offer)
        self.assertNotIn("retireSwap", offer)
        # retire at the latch, in queue order, never past the frame in flight
        retire = body(source, "    void retireThrough(uint64_t seq)\n")
        self.assertIn("at > seq", retire)
        self.assertIn("head == gpuRequest", retire)
        self.assertIn("retireThrough(entrySeq[asked])", source)
        self.assertIn("retireThrough(entrySeq[replaced])", source)
        # no early retire or collapse once retiring at the latch
        self.assertIn("bootEarlyRetire && !latchRetire && retireSwap", source)
        self.assertIn("asyncCopy == 2 && !latchRetire && retireSwap", source)
        self.assertIn("if (bootEarlyRetire && !latchRetire) {", source)
        finish = body(source, "    void finishTransfer(")
        self.assertLess(finish.index("if (latchRetire)"), finish.index("retireSwap"))
        # never replace across layouts
        present = body(source, "    void presentEntry(int i)\n")
        self.assertIn("sameLayout(i, ring.pending)", present)

    def test_fence_only_present_has_no_copy(self):
        gpu = GPU.read_text()
        copy = gpu.split("int tgm_present_copy(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("if (dst_va) {", copy)
        self.assertLess(copy.index("if (dst_va) {"), copy.index("NVB0B5_LAUNCH_DMA_NON_PIPELINED"))


if __name__ == "__main__":
    unittest.main()
