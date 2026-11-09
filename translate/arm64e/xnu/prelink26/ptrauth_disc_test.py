"""ptrauth_disc.py against clang's __builtin_ptrauth_string_discriminator, compiled for arm64e and run on this Mac."""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptrauth_disc import discriminators

NAMES = [
    "",
    "a",
    "IOService",
    "__ZTV9IOService",
    "_vn_default_error",
    "__ZN8OSObject9MetaClassE",
    "exactly8",
    "sixteen bytes!!!",
    "seventeen bytes!!",
    "the quick brown fox jumps over the lazy dog, at length, past any single block",
]


class Discriminators(unittest.TestCase):
    def test_matches_clang(self):
        with tempfile.TemporaryDirectory() as t:
            src = os.path.join(t, "d.c")
            with open(src, "w") as f:
                f.write("#include <stdio.h>\nint main(void){\n")
                for n in NAMES:
                    f.write('printf("%%u\\n", (unsigned)__builtin_ptrauth_string_discriminator("%s"));\n' % n)
                f.write("return 0;}\n")
            subprocess.run(["xcrun", "clang", "-arch", "arm64e", "-O0", "-o", os.path.join(t, "d"), src], check=True)
            run = subprocess.run([os.path.join(t, "d")], capture_output=True, text=True, check=True)
        ours = discriminators(NAMES)
        self.assertEqual([ours[n] for n in NAMES], [int(v) for v in run.stdout.split()])


if __name__ == "__main__":
    unittest.main()
