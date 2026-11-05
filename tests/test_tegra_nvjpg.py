"""TegraNVJPG (NVJPG 1.0 as the AppleJPEGDriver hardware JPEG codec), on the host:
- drivers/TegraNVJPG/test/nvjpg_host.c: TegraNVJPGJpeg.h's parser against libjpeg-turbo's parse of a matrix of files
  libjpeg-turbo writes (sizes, sampling, restart intervals, quality, optimised tables), the decoder setup made from it,
  every refusal (progressive, arithmetic, 12-bit, 16-bit tables, CMYK, Adobe RGB, 4:1:1, non-interleaved and multiple
  scans, DNL, oversize, corrupt tables, truncated headers), the output targets and the JFIF matrix, mutation fuzzing
  under ASan/UBSan, and the boot self-test's vectors (their reference planes are libjpeg-turbo's decode of the committed
  JPEGs). Without Homebrew's jpeg-turbo the libjpeg-turbo comparisons are skipped, the rest still runs;
- drivers/TegraNVJPG/test/request_host.c: the AppleJPEGDriver decode request's translation and refusals;
- the vendored firmware and NVIDIA headers are the files their SOURCE files name."""
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
JPG = ROOT / "drivers/TegraNVJPG"
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all"]
TURBO = Path("/opt/homebrew/opt/jpeg-turbo")


def build_and_run(source, extra=()):
    with tempfile.TemporaryDirectory(prefix="ioswitch-nvjpg-") as directory:
        executable = Path(directory) / source.stem
        subprocess.run(["clang", *FLAGS, *extra, str(source), "-lm", "-o", str(executable)], check=True)
        return subprocess.run([str(executable)], capture_output=True, text=True)


class TegraNVJPGTests(unittest.TestCase):
    def test_parser_and_vectors(self):
        extra = []
        if (TURBO / "lib/libjpeg.a").exists():
            extra = ["-DHAVE_LIBJPEG", "-I", str(TURBO / "include"), str(TURBO / "lib/libjpeg.a")]
        result = build_and_run(JPG / "test/nvjpg_host.c", extra)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.startswith("PASS nvjpg"), result.stdout)
        if extra:
            self.assertIn("libjpeg-turbo", result.stdout)

    def test_request(self):
        result = build_and_run(JPG / "test/request_host.c")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(result.stdout.startswith("PASS request"), result.stdout)

    def test_firmware(self):
        blob = (JPG / "firmware/nvidia/tegra210/nvjpg.bin").read_bytes()
        digest = hashlib.sha256(blob).hexdigest()
        self.assertEqual(digest, "85bec091379f6bfd5873e9b657396065024e39b11e72c6f7f86e49a30aa830fe")
        self.assertIn(digest, (JPG / "firmware/SOURCE").read_text())
        magic, version, size, os_header, bin_data, bin_size, fce_header = struct.unpack_from("<7I", blob)
        self.assertEqual((magic, version, size, os_header, bin_data), (0x10de, 1, len(blob), 0x100, 0x200))
        self.assertEqual(fce_header, 0xa5a5a5a5)  # no FCE section: nothing to hand over, unlike VIC
        code, code_size, data, data_size, apps = struct.unpack_from("<5I", blob, os_header)
        self.assertEqual((code, code_size, data, data_size, apps), (0, 0x300, 0x3d00, 0x200, 13))
        self.assertLessEqual(bin_data + bin_size, len(blob))
        self.assertEqual((JPG / "firmware/LICENCE.nvidia").read_bytes(),
                         (JPG.parent / "TegraNVDEC/firmware/LICENCE.nvidia").read_bytes())

    def test_headers(self):
        source = (JPG / "nvidia/SOURCE").read_text()
        for name, prefix, suffix in (("nvjpg_drv.h", "b719a992", "4278"), ("cle7d0.h", "144aa394", "8776")):
            digest = hashlib.sha256((JPG / "nvidia" / name).read_bytes()).hexdigest()
            self.assertTrue(digest.startswith(prefix) and digest.endswith(suffix), name)
            self.assertIn(f"{prefix}...{suffix}", source)


if __name__ == "__main__":
    unittest.main()
