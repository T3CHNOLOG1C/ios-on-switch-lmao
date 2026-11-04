"""native/loader/emc.c (the loader's EMC raise) against a host model of the BPMP mailbox, I2C5 and the counter:
the raise itself, the kernel's later no-op, and every refusal leaving the clock and channel as they were."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeLoaderEmcTests(unittest.TestCase):
    def test_raise_and_refusals_against_the_model(self):
        with tempfile.TemporaryDirectory(prefix="ioswitch-emc-") as directory:
            executable = Path(directory) / "emc"
            subprocess.run(["clang", "-std=c11", "-DLOADER_EMC_HOST_TEST=1", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-I", str(ROOT / "native/loader"), str(ROOT / "native/loader/emc.c"),
                            str(ROOT / "tests/native_loader_emc_harness.c"), "-o", str(executable)], check=True)
            output = subprocess.check_output([str(executable)], text=True)
            self.assertIn("PASS emc 13 scenarios", output)


if __name__ == "__main__":
    unittest.main()
