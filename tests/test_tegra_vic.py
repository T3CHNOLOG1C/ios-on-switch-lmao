"""TegraVIC (VIC 4.0 as IOSurfaceAccelerator's AppleM2ScalerCSCDriver), on the host:
- drivers/TegraVIC/test/vic_host.c: the configuration packing against libdrm's vic40.h bitfields, the colour matrices
  through the engine's arithmetic against the float BT.601/BT.709 conversions, the jobs for Screen Recording's and
  AirPlay's clone requests and Core Animation's border clears, IOSurfaceAccelerator requests (0x1b0 bytes) translated,
  and every refusal of the request translator and the checker;
- the vendored firmware is the file firmware/SOURCE names, with the falcon and FCE headers TegraVIC.cpp parses."""
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VIC = ROOT / "out/native-a57/root-v1/tree/drivers/TegraVIC"
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all"]


class TegraVICTests(unittest.TestCase):
    def test_host(self):
        with tempfile.TemporaryDirectory(prefix="ioswitch-vic-") as directory:
            executable = Path(directory) / "vic_host"
            subprocess.run(["clang", *FLAGS, str(VIC / "test/vic_host.c"), "-lm", "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.startswith("PASS vic"), result.stdout)

    def test_firmware(self):
        blob = (VIC / "firmware/nvidia/tegra210/vic04_ucode.bin").read_bytes()
        self.assertEqual(hashlib.sha256(blob).hexdigest(),
                         "01ab2263d3ca54ce6abd425d0df8bedf3a33a59bfc51f9bec1d67207e74867f9")
        self.assertIn("01ab2263d3ca54ce6abd425d0df8bedf3a33a59bfc51f9bec1d67207e74867f9",
                      (VIC / "firmware/SOURCE").read_text())
        magic, version, size, os_header, bin_data, bin_size, fce_header, fce_data = struct.unpack_from("<8I", blob)
        self.assertEqual((magic, version, size), (0x10de, 1, len(blob)))
        code, code_size, data, data_size = struct.unpack_from("<4I", blob, os_header)
        self.assertTrue(code_size and code + code_size <= bin_size and data + data_size <= bin_size)
        self.assertEqual((fce_header, fce_data), (0x3c00, 0x3d00))
        fce_size = struct.unpack_from("<I", blob, fce_header + 8)[0]
        self.assertEqual(fce_size, 0x3c0)
        self.assertLessEqual(fce_data + fce_size, len(blob))


if __name__ == "__main__":
    unittest.main()
