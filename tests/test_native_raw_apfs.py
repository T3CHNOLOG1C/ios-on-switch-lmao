"""Only the new raw backend, real C plus bounded inert transport callbacks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RawApfsTests(unittest.TestCase):
    def test_backend(self):
        compiler = shutil.which("clang")
        if compiler is None:
            self.skipTest("clang unavailable")
        with tempfile.TemporaryDirectory(prefix="ioswitch-raw-apfs-") as directory:
            binary = Path(directory) / "raw-apfs-test"
            subprocess.run([
                compiler, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "native/storage"),
                str(ROOT / "tests/raw_apfs_test.c"),
                str(ROOT / "native/storage/raw_apfs.c"), "-o", str(binary)
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
