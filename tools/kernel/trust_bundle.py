"""trust_bundle.py OUT STATIC.im4p SYSTEM.im4p: the static trust cache bundle the kernel window carries for the root and
OS cryptex (vendor/unixnu's build-original-system-trust.py: u32 count 2, u32 offsets, the 'trst' payload of
StaticTrustCache, the 'trcs' payload of Cryptex1,SystemTrustCache)."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from im4p import im4p_fields  # noqa: E402


def payload(path, kind):
    fields = im4p_fields(Path(path).read_bytes())
    if fields[1][1].decode() != kind:
        sys.exit("{} is a {} IM4P, not {}".format(path, fields[1][1].decode(), kind))
    return next(value for tag, value in fields[1:] if tag == 0x04)


out, static, system = sys.argv[1:]
static, system = payload(static, "trst"), payload(system, "trcs")
Path(out).write_bytes(struct.pack("<3I", 2, 12, 12 + len(static)) + static + system)
