"""Hardware video decode (drivers/SwitchVideoDecoders over drivers/TegraNVDEC), on the host:
- BlockLinear.c's de-swizzle against the per-byte GOB formula;
- H264NVDEC over test/VDECClip.h's clip: every submit accepted by the kernel's own checker (TegraNVDECCheck.h), pictures
  out in x264's display order, and the checker refusing each kind of bad submit;
- AVD.videodecoder itself on this Mac's VideoToolbox over a fake TegraNVDEC client: the plug-in's class layout, session
  calls and property answers, and its fall-back onto Apple's software decoder when the arena has no room;
- the clip check (vdectest) against Apple's own decoders, which proves the clip and its x264 CRCs."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "out/native-a57/root-v1/tree/drivers/SwitchVideoDecoders"
AVD = VIDEO / "AVD"
FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined",
         "-fno-sanitize-recover=all"]
FRAMEWORKS = ["-framework", "CoreFoundation", "-framework", "CoreMedia", "-framework", "CoreVideo", "-framework",
              "VideoToolbox"]


def build_and_run(name, sources, extra=(), args=()):
    with tempfile.TemporaryDirectory(prefix="ioswitch-video-") as directory:
        executable = Path(directory) / name
        subprocess.run(["clang", *FLAGS, *[str(s) for s in sources], *extra, "-o", str(executable)], check=True)
        result = subprocess.run([str(executable), *args], capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr


class SwitchVideoTests(unittest.TestCase):
    def test_block_linear_copy(self):
        code, output = build_and_run("blocklinear", [VIDEO / "test/blocklinear_host.c", AVD / "BlockLinear.c"])
        self.assertEqual(code, 0, output)
        self.assertIn("OK", output)

    def test_h264_clip_through_the_kernel_checker(self):
        code, output = build_and_run("h264", [VIDEO / "test/h264_host.c", AVD / "H264Syntax.c", AVD / "H264NVDEC.c"],
                                     args=["--clip"])
        self.assertEqual(code, 0, output)
        self.assertIn("PASS h264 clip: 30 access units, 30 pictures", output)

    def test_avd_plugin_on_videotoolbox(self):
        code, output = build_and_run("avd", [VIDEO / "test/avd_host.c", AVD / "H264NVDEC.c", AVD / "H264Syntax.c",
                                             AVD / "BlockLinear.c", AVD / "ClipCheck.c"], FRAMEWORKS)
        self.assertEqual(code, 0, output)
        self.assertIn("PASS avd", output)

    def test_clip_check_on_apple_decoders(self):
        # Only the software run is required: a Mac (or sandbox) without a reachable hardware decoder fails the other.
        _, output = build_and_run("vdectest", [VIDEO / "test/vdectest.c", AVD / "ClipCheck.c"], FRAMEWORKS)
        self.assertIn("software: software decoder, 30/30 frames match x264", output)


if __name__ == "__main__":
    unittest.main()
