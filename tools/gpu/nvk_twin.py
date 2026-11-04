"""The NVK host twin with the device's pipeline cache identity, as the seed and the raw pack both run it."""
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def lc_uuid(path, value=None):
    """The LC_UUID of the thin 64-bit Mach-O at `path`, after setting it to `value` (16 bytes) when given."""
    data = bytearray(Path(path).read_bytes())
    magic, _, _, _, ncmds = struct.unpack_from('<5I', data)
    at = 32
    for _ in range(ncmds if magic == 0xfeedfacf else 0):
        cmd, size = struct.unpack_from('<2I', data, at)
        if cmd == 0x1b:   # LC_UUID
            if value is not None:
                data[at + 8:at + 24] = value
                Path(path).write_bytes(data)
            return bytes(data[at + 8:at + 24])
        at += size
    sys.exit('%s: no LC_UUID' % path)


def make_twin(nvk, twin, out, env):
    """Copy the host twin to `out`, ad hoc signed. The two dylibs' .source files must be equal (the same Mesa tree and
    -Dnvk-build-id); without a build id NVK hashes its own LC_UUID into the pipeline cache UUID, so the twin takes the
    iOS dylib's."""
    sources = [Path(str(p) + '.source') for p in (nvk, twin)]
    if not all(p.is_file() for p in sources) or sources[0].read_text() != sources[1].read_text():
        sys.exit('%s and %s missing or different: the twin is not of the iOS dylib\'s sources' % tuple(sources))
    shutil.copyfile(twin, out)
    Path(out).chmod(0o755)
    pinned = any(line.split()[1:] for line in sources[0].read_text().splitlines() if line.startswith('nvk-build-id'))
    if not pinned:
        lc_uuid(out, lc_uuid(nvk))
    result = subprocess.run(['/usr/bin/codesign', '--force', '--sign', '-', str(out)], env=env, capture_output=True, text=True)
    if result.returncode:
        sys.exit('codesign %s: %s' % (out, result.stderr))
