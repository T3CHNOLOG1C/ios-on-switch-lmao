"""drivers/TegraGM20B/TegraGM20BPushScan.h (the kernel's tegra-gpu-stats method-stream reader and the push profile's classes) on the
host, over streams laid out as NVK writes them for GM20B (tests/gm20b_push_scan_harness.cpp): every counter of a Core
Animation-like submission, a multisampled pass, a copy, a clear and a dispatch, and no profile stamp after a push whose method
continues into the next one, nor after a push the kernel could not read or one holding an opcode NVK never writes."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
GM20B = ROOT / "out/native-a57/root-v1/tree/drivers/TegraGM20B"


class PushScanTests(unittest.TestCase):
    def test_scan_against_nvk_streams(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "push-scan"
            compile_result = subprocess.run(
                ["xcrun", "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                 str(ROOT / "tests/gm20b_push_scan_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: push scan", result.stdout)

    def test_kernel_uses_the_header(self):
        """The user client reads pushes only through the header the harness tests, and profiles only what it read."""
        client = (GM20B / "TegraGM20BUserClient.cpp").read_text()
        self.assertIn('#include "TegraGM20BPushScan.h"', client)
        self.assertNotIn("void pushMethod(", client)
        self.assertNotIn("void pushParse(", client)
        scan = client.split("void AGXDeviceUserClient::scanPushes(", 1)[1].split("\n}\n", 1)[0]
        self.assertIn("scanPush(c, p, whole, nullptr, 0)", scan)
        self.assertIn("prof->cls[i] = cls;", scan)
        gpu = (GM20B / "gpu.c").read_text()
        exec_ = gpu.split("int tgm_chan_exec(", 1)[1].split("\n}\n", 1)[0]
        # a stamp only after a push whose class is not SKIP, only on a profiled submission
        self.assertIn("if (prof && prof->cls[i] != TGM_PC_SKIP) {", exec_)


if __name__ == "__main__":
    unittest.main()
