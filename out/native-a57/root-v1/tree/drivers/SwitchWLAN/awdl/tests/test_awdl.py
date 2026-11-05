"""Build the AWDL core (drivers/SwitchWLAN/awdl) for the Mac with clang, AddressSanitizer and UBSan, and run its host
tests (tests/test_awdl.c): frame parsing of a hand-built MIF and fuzzed frames, PSF/MIF round trip, data
encapsulation, synchronisation and election over a simulated air of two and three nodes with skewed clocks, a
two-channel sequence, the TSF clock, the IO80211 user-client event ring and awdl0's Apple80211 requests.
"""
from pathlib import Path
import subprocess
import tempfile

here = Path(__file__).resolve().parent
awdl = here.parent
sources = [awdl / 'awdl_wire.c', awdl / 'awdl_core.c', awdl / 'awdl_bcm.c', awdl / 'awdl_apple80211.c',
           here / 'test_awdl.c', here / 'test_native.c']
with tempfile.TemporaryDirectory() as directory:
    binary = Path(directory) / 'test_awdl'
    subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-I', str(awdl),
                    *map(str, sources), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
