"""The rotated text console's 16-byte LDP/STP paths (XNU osfmk/console/video_console.c, r9-preboot round 3) give the
same pixels as the 32-bit loops they replaced. Needs an arm64 host (the helpers are arm64 inline assembly)."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CONSOLE = ROOT / 'out/native-a57/root-v1/tree/.cache/xnu-work/osfmk/console/video_console.c'


@unittest.skipUnless(CONSOLE.is_file() and platform.machine() in ('arm64', 'aarch64'), 'needs the XNU checkout, arm64')
class XnuConsoleWideTests(unittest.TestCase):
    def test_wide_paths_match_the_32_bit_loops(self):
        source = CONSOLE.read_text()
        start = source.index('static inline boolean_t\nvc_rotate3_wide(')
        end = source.index('static void\nvc_clear_screen(')
        with tempfile.TemporaryDirectory(prefix='ioswitch-console-') as directory:
            directory = Path(directory)
            (directory / 'helpers.inc').write_text(source[start:end])
            executable = directory / 'wide'
            subprocess.run(['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', str(directory), str(ROOT / 'tests/xnu_console_wide_harness.c'), '-o', str(executable)],
                           check=True)
            self.assertIn('PASS 201 scroll/clear cases, 20000 glyph columns',
                          subprocess.check_output([str(executable)], text=True))


if __name__ == '__main__':
    unittest.main()
