"""The compositor's vsync phase (r17-touch): drivers/NativeMobileDisplay/VsyncPhase.h against the model compositor in
tests/vsync_phase_harness.cpp (r8a's quiet windows, periodic and rare stalls, a 20 Hz client, a copying compositor, one
too slow for any phase), and guards on how NativeLegacyDisplay drives it: V_PULSE2 programmed as L4T's
tegra_dc_init_vpulse2_int does, through assembly registers only (the flips' own GENERAL_ACT_REQ latches them: the phase
never writes STATE_CONTROL), at most one vsync a frame, V_BLANK carrying it until V_PULSE2 has fired, a never-firing
V_PULSE2 turning the phase off with one line (no panic), and the pacing line's "rate" cause for promptly answered swaps."""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DISPLAY = ROOT / "out/native-a57/root-v1/tree/drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp"


def body(source, signature):
    return source.split(signature, 1)[1].split("\n    }\n", 1)[0]


class VsyncPhaseTests(unittest.TestCase):
    def test_model_compositor(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "vsync-phase"
            build = subprocess.run(["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                                    str(ROOT / "tests/vsync_phase_harness.cpp"), "-o", str(binary)],
                                   text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(build.returncode, 0, build.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: vsync phase", result.stdout)
            quiet = re.search(r"quiet\s+off: latch ([\d.]+) ms.*phase: latch ([\d.]+) ms", result.stdout)
            self.assertTrue(quiet and float(quiet.group(1)) > 12 and float(quiet.group(2)) < 4.5, result.stdout)

    def test_display_guards(self):
        source = DISPLAY.read_text()
        # L4T dc_reg.h / tegra_dc_init_vpulse2_int (T21x).
        for line in ("kVpulse2Int = 1u << 5", "kDcDispSignalOptions0 = 0x400", "kVpulse2Enable = 1u << 19",
                     "kDcDispVpulse2Control = 0x422", "kDcDispVpulse2Position = 0x423", "kVpulse2Last = 1u << 8"):
            self.assertIn(line, source)
        program = body(source, "    void programPhase(uint32_t leadUs)")
        registers = body(source, "    void writePhaseRegisters()")
        for code in (program, registers):
            self.assertNotIn("kDcStateControl", code, "the phase never asks for a latch of its own")
            self.assertNotIn("kDcRegActControl", code)
        self.assertIn("phaseLine | (phaseLine + 1) << 16", registers)
        deliver = body(source, "    void deliverVsync(uint64_t stamp, uint64_t continuous, uint32_t lead)")
        self.assertIn("if (deliveredFrame == frameEnds) return;", deliver)
        notify = body(source, "    void vblankNotify()")
        self.assertIn("if (phaseDelivering()) return;", notify)
        self.assertIn("vpulseSeen", body(source, "    bool phaseDelivering() const"))
        present = body(source, "    void notePhasePresent(uint64_t stamp, const FrameMark &mark)")
        self.assertIn("phaseFailed = true;", present)
        self.assertNotIn("panic(", present, "a V_PULSE2 that never fired costs one line, not the boot")
        self.assertIn('IOLog("IOSwitch display: vsync phase off for this boot', present)
        self.assertIn("kLongRate", body(source, "    void noteLong(uint32_t frames, const FrameMark &mark)"))
        # A folded action (V_PULSE2 and the frame end together) counts the vsync against that frame, not the next.
        tick = body(source, "    void vblankTick()")
        self.assertLess(tick.index("if (status & kVpulse2Int) vpulseNotify();"),
                        tick.index("if (status & kFrameEndInt) frameEnds = frameEnds + 1;"))
        # noteStatsWindow zeroes stReplaced each window: that is no replacement.
        presented = source.split("    void notePresent(uint64_t stamp, uint64_t latchedAt, const FrameMark &mark)", 1)[1]
        self.assertLess(presented.index("if (stReplaced < phaseReplacedSeen) phaseReplacedSeen = 0;"),
                        presented.index("if (stReplaced != phaseReplacedSeen)"))
        # The irregular gap at a source or line change is never passed on as the interval.
        self.assertIn("!moved && vblankLast", deliver)
        # SC7 resets the DC: the phase's registers go back before finishResume's latch request.
        resume = source.split("    bool finishResume()", 1)[1]
        self.assertLess(resume.index("writePhaseRegisters()"), resume.index("kGeneralActReq | kWinAActReq"))


if __name__ == "__main__":
    unittest.main()
