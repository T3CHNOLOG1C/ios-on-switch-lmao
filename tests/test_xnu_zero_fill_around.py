"""The zero-fill fault-around (XNU osfmk/vm/vm_fault.c ioswitch_zero_fill_around, r17-faults) enters exactly the absent
pages of the faulting page's 16 KiB group that lie in its map entry and VM object, at the matching offsets, and nothing
outside the plain case (fresh, unshadowed, uncopied, pagerless, non-purgeable anonymous memory in a user pmap, not
executable): compiled from the XNU source against a model of the object and pmap (tests/xnu_zero_fill_around_harness.c)."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
XNU = Path(os.environ.get('IOSWITCH_XNU_WORK', ROOT / 'out/native-a57/root-v1/tree/.cache/xnu-work'))
FAULT = XNU / 'osfmk/vm/vm_fault.c'


@unittest.skipUnless(FAULT.is_file(), 'needs the XNU checkout')
class ZeroFillAroundTests(unittest.TestCase):
    def test_group_bounds_and_plain_case(self):
        source = FAULT.read_text()
        start = source.index('#ifndef IOSWITCH_ZERO_FILL_AROUND')
        end = source.index('uint64_t vm_fault_resilient_media_initiate')
        with tempfile.TemporaryDirectory(prefix='ioswitch-zfa-') as directory:
            directory = Path(directory)
            (directory / 'fault_around.inc').write_text(source[start:end])
            executable = directory / 'zfa'
            subprocess.run(['clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                            '-fsanitize=address,undefined', '-I', str(directory),
                            str(ROOT / 'tests/xnu_zero_fill_around_harness.c'), '-o', str(executable)], check=True)
            self.assertIn('PASS 18 zero-fill fault-around cases', subprocess.check_output([str(executable)], text=True))


if __name__ == '__main__':
    unittest.main()
