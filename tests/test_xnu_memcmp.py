import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PATCH = ROOT / "out/native-a57/root-v1/tree/patches/xnu-ioswitch.diff"


def new_file(patch, path):
    """A file the XNU patch adds, as the patch has it."""
    marker = "diff --git a/{0} b/{0}\n".format(path)
    hunk = patch.split(marker, 1)[1].split("\ndiff --git ", 1)[0]
    assert "new file mode" in hunk.split("@@", 1)[0], path
    body = hunk.split("\n@@", 1)[1].split("\n", 1)[1]
    return "".join(line[1:] + "\n" for line in body.split("\n") if line.startswith("+"))


def changed_lines(patch, path):
    marker = "diff --git a/{0} b/{0}\n".format(path)
    hunk = patch.split(marker, 1)[1].split("\ndiff --git ", 1)[0]
    return "\n".join(line[1:] for line in hunk.split("\n") if line.startswith("+") and not line.startswith("+++"))


class XnuMemcmpTests(unittest.TestCase):
    """r17-launch: XNU's arm64 memcmp/bcmp compare a word at a time (osfmk/arm64/ioswitch_memcmp.h) with the byte loops'
    exact results; AMFI's entitlement-key lookups spent 2.9 s per boot in the byte loop (kprof r7v)."""

    def test_matches_byte_loops(self):
        header = new_file(PATCH.read_text(), "osfmk/arm64/ioswitch_memcmp.h")
        with tempfile.TemporaryDirectory() as directory:
            (pathlib.Path(directory) / "ioswitch_memcmp.h").write_text(header)
            binary = pathlib.Path(directory) / "xnu-memcmp"
            compile_result = subprocess.run(
                ["xcrun", "clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined", "-g",
                 "-I", directory, str(ROOT / "tests/xnu_memcmp_harness.c"), "-o", str(binary)],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(compile_result.returncode, 0, compile_result.stdout)
            result = subprocess.run([binary], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("PASS: xnu memcmp/bcmp match the byte loops", result.stdout)

    def test_subrs_uses_it_on_arm64_only(self):
        """The harness's two functions are subrs.c's arm64 bodies; other architectures keep the byte loops."""
        added = changed_lines(PATCH.read_text(), "osfmk/device/subrs.c")
        self.assertIn("#include <arm64/ioswitch_memcmp.h>", added)
        self.assertIn("len -= ioswitch_cmp_first_difference((const unsigned char *)pa, (const unsigned char *)pb, len);",
                      added)
        self.assertIn("const size_t i = ioswitch_cmp_first_difference(p1, p2, n);", added)
        self.assertIn("return i < n ? p1[i] - p2[i] : 0;", added)
        self.assertEqual(len(re.findall(r"#if defined\(__arm64__\)\n", added + "\n")), 2)
        harness = (ROOT / "tests/xnu_memcmp_harness.c").read_text()
        for line in ("len -= ioswitch_cmp_first_difference((const unsigned char *)pa, (const unsigned char *)pb, len);",
                     "const size_t i = ioswitch_cmp_first_difference(p1, p2, n);",
                     "return i < n ? p1[i] - p2[i] : 0;"):
            self.assertIn(line, harness)


if __name__ == "__main__":
    unittest.main()
