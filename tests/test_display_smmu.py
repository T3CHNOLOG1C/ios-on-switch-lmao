"""The shared Tegra SMMU (drivers/TegraSMMU) and the display's SMMU probe (drivers/NativeMobileDisplay/DisplaySmmuProbe.h) on the
host: TegraSMMUDomain's entries, tables and translation, and the tables TegraVIC builds with it byte for byte against the
algorithm TegraVIC had before (tests/smmu_domain_harness.cpp); the probe against a model of DC A, its CRC unit and the SMMU,
passing on a working unit and failing at its own stage, undone, for each fault (tests/display_probe_harness.cpp). Source
guards: the MC's SMMU registers are programmed only by TegraSMMU.cpp, and the display's probe runs through the same
DisplaySmmuProbe::perform the harness drives."""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
DRIVERS = ROOT / "out/native-a57/root-v1/tree/drivers"


def run_harness(test, name, marker):
    with tempfile.TemporaryDirectory() as directory:
        binary = pathlib.Path(directory) / name
        compile_result = subprocess.run(
            ["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", str(ROOT / "tests" / name),
             "-o", str(binary)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        test.assertEqual(compile_result.returncode, 0, compile_result.stdout)
        result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        test.assertEqual(result.returncode, 0, result.stdout)
        test.assertIn(marker, result.stdout)
        return result.stdout


class DisplaySmmuTests(unittest.TestCase):
    def test_smmu_domain(self):
        run_harness(self, "smmu_domain_harness.cpp", "PASS: smmu domain")

    def test_display_probe(self):
        out = run_harness(self, "display_probe_harness.cpp", "PASS: display probe")
        self.assertIn("working unit                       passed", out)

    def test_one_owner_of_the_smmu_registers(self):
        """PTB_ASID/PTB_DATA/TLB_FLUSH/PTC_FLUSH are global: only TegraSMMU.cpp touches them (under its lock)."""
        owner = DRIVERS / "TegraSMMU/TegraSMMU.cpp"
        for path in DRIVERS.rglob("*.cpp"):
            if path == owner or "NativeAdreno" in str(path):
                continue
            text = path.read_text(errors="replace")
            self.assertIsNone(re.search(r"MC_SMMU_(PTB_ASID|PTB_DATA|TLB_FLUSH|PTC_FLUSH)", text), str(path))
        vic = (DRIVERS / "TegraVIC/TegraVIC.cpp").read_text()
        self.assertIn("tegra_smmu_attach(", vic)
        self.assertIn("TegraSMMUDomain", vic)

    def test_display_probe_wiring(self):
        source = (DRIVERS / "NativeMobileDisplay/NativeLegacyDisplay.cpp").read_text()
        self.assertIn("probe.perform(", source)
        self.assertIn('abortProbe("the display suspended (SC7)")', source)
        self.assertIn('abortProbe("the display stopped")', source)
        kick = source.split("    void kickOnce()\n", 1)[1].split("\n    }\n", 1)[0]
        self.assertIn("if (probe.active() || probeRequest)", kick)  # swaps wait while window A is the probe's
        # A probe START_ADDR request is under the flip deadline like a flip: a lost DC A interrupt panics, never freezes.
        deadline = source.split("    void checkFlipDeadline()\n", 1)[1].split("\n    }\n", 1)[0]
        self.assertIn("!probeRequest", deadline)
        self.assertIn("flipTimer->setTimeoutMS(kFlipDeadlineMS)", source.split("struct ProbeIo", 1)[1].split("};", 1)[0])


if __name__ == "__main__":
    unittest.main()
