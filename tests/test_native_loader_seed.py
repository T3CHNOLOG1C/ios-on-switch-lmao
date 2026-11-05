"""native/loader/seed.c (the per-boot /chosen/random-seed from SE2's DRBG) against a host model of CAR's clocks, SE2's
registers and DRBG, the AHB write queue and the counter: the shipped case, the entropy clock off, and every failure
leaving the pinned seed (plus the counter) with a status for XNU's log line."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeLoaderSeedTests(unittest.TestCase):
    def test_seed_mix_against_the_model(self):
        with tempfile.TemporaryDirectory(prefix="ioswitch-seed-") as directory:
            executable = Path(directory) / "seed"
            subprocess.run(["clang", "-std=c11", "-DIOSW_SEED_HOST_TEST=1", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-I", str(ROOT / "native/loader"), str(ROOT / "native/loader/seed.c"),
                            str(ROOT / "tests/native_loader_seed_harness.c"), "-o", str(executable)], check=True)
            output = subprocess.check_output([str(executable)], text=True)
            self.assertIn("PASS seed 13 scenarios", output)


if __name__ == "__main__":
    unittest.main()
