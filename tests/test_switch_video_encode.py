"""Hardware video encode (drivers/SwitchVideoEncoders over drivers/TegraNVENC), on the host:
- AVE (H264H7.videoencoder) itself on this Mac's VideoToolbox over a fake TegraNVENC client whose engine is a lossless
  stand-in for NVENC: every submit accepted by the kernel's own checker (TegraNVENCCheck.h), the encode check
  (EncodeCheck.c, the one the Switch runs once per boot) decoding every frame back exactly with Apple's decoder in three
  input layouts, sync samples where they belong, P_Skip from the reference the setup names; the SPS/PPS/avcC parsing
  back with AVD's H264Syntax.c; the checker refusing each kind of bad submit; the rate control on a synthetic encoder;
- the kernel's E3 vectors (TegraNVENC/selftest/E3Vectors.inc) being exactly what the plug-in's H264NVENC.c and AVD's
  H264NVDEC.c produce today (test/e3gen)."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DRIVERS = ROOT / "out/native-a57/root-v1/tree/drivers"
ENC = DRIVERS / "SwitchVideoEncoders"
AVD = DRIVERS / "SwitchVideoDecoders/AVD"
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all"]
FRAMEWORKS = ["-framework", "CoreFoundation", "-framework", "CoreMedia", "-framework", "CoreVideo", "-framework",
              "VideoToolbox"]


def build_and_run(name, sources, extra=()):
    with tempfile.TemporaryDirectory(prefix="ioswitch-encode-") as directory:
        executable = Path(directory) / name
        subprocess.run(["clang", *FLAGS, *[str(s) for s in sources], *extra, "-o", str(executable)], check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        return result.returncode, result.stdout, result.stderr


class SwitchVideoEncodeTests(unittest.TestCase):
    def test_ave_plugin_on_videotoolbox(self):
        code, out, err = build_and_run("ave", [ENC / "test/ave_host.c", ENC / "AVE/H264NVENC.c",
                                               ENC / "AVE/EncodeCheck.c", AVD / "H264Syntax.c"], FRAMEWORKS)
        self.assertEqual(code, 0, out + err)
        self.assertIn("PASS ave", out)
        self.assertEqual(out.count(" ok 30/30 frames encoded (0 errors) by a hardware encoder, 30 decoded (0 errors), "
                                   "sync samples 0 10 15 25, PSNR min Y 99.0 U 99.0 V 99.0 dB"), 3, out)

    def test_e3_vectors_are_current(self):
        code, out, err = build_and_run("e3gen", [ENC / "test/e3gen.c", ENC / "AVE/H264NVENC.c", AVD / "H264NVDEC.c",
                                                 AVD / "H264Syntax.c"])
        self.assertEqual(code, 0, err)
        self.assertEqual(out, (DRIVERS / "TegraNVENC/selftest/E3Vectors.inc").read_text(),
                         "regenerate TegraNVENC/selftest/E3Vectors.inc with SwitchVideoEncoders/test/e3gen")


if __name__ == "__main__":
    unittest.main()
