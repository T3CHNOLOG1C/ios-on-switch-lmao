import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
TREE = ROOT / "."
DRIVER = TREE / "drivers/NativeIOSwitchBlock"
# copypv's flag values, from the XNU this driver links against (exported header first, then the source tree's).
PMAP_HEADERS = [ROOT / "out/native-a57/root-v1/tree/build/xnu/ios-obj/EXPORT_HDRS/osfmk/vm/pmap.h", ROOT / "out/native-a57/root-v1/tree/.cache/xnu-work/osfmk/vm/pmap.h"]
FLAGS = ("cppvPsnk", "cppvPsrc", "cppvFsnk", "cppvFsrc", "cppvNoModSnk", "cppvNoRefSrc", "cppvKmap")


def pmap_flags():
    for header in PMAP_HEADERS:
        if header.exists():
            text = header.read_text()
            values = {name: re.search(r"#define\s+%s\s+(\d+)" % name, text) for name in FLAGS}
            if all(values.values()):
                return {name: int(match.group(1)) for name, match in values.items()}
    raise unittest.SkipTest("no XNU vm/pmap.h to take copypv's flags from")


class NativeSdClientCopyTests(unittest.TestCase):
    def test_segment_walk_without_the_sink_flush(self):
        flags = pmap_flags()
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "sd-client-copy"
            compile_result = subprocess.run(
                ["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-g", "-I", str(DRIVER)] + ["-D%s=%d" % item for item in flags.items()] +
                [str(ROOT / "tests/native_sd_client_copy_harness.cpp"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: sd client copy", result.stdout)

    def test_driver_moves_request_bytes_only_through_the_helper(self):
        """readBytes/writeBytes (and their flush) stay only behind boot-arg ioswitch-sd-copy-flush."""
        source = (DRIVER / "NativeIOSwitchBlock.cpp").read_text()
        uses = [line.strip() for line in source.splitlines() if re.search(r"->\s*(readBytes|writeBytes)\(", line)]
        self.assertEqual(len(uses), 1, uses)
        self.assertIn("copyFlush", source.split(uses[0])[0].rsplit("bool stage(", 1)[1])
        self.assertIn("ioswitchClientCopy(memory, at, length, toClient, copypv, from)", source)


if __name__ == "__main__":
    unittest.main()
