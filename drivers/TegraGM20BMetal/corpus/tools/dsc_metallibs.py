#!/usr/bin/env python3
"""Find metallibs (MTLB containers) embedded in a dyld shared cache's subcaches.

Writes each to <out>/<image>@<addr>.metallib.
usage: dsc_metallibs.py <dir with dyld_shared_cache_arm64*> <out dir>
"""

import mmap
import re
import struct
import sys
from pathlib import Path

d, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
main = d / 'dyld_shared_cache_arm64'


def maps(path: Path) -> tuple[mmap.mmap, list[tuple[int, int, int]]]:
    f = path.open('rb')
    h = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    moff, mcount = struct.unpack_from('<2I', h, 0x10)
    ms = [struct.unpack_from('<3Q2I', h, moff + 32 * i)[:3] for i in range(mcount)]
    return h, ms


h, _ = maps(main)
ioff, icount = struct.unpack_from('<2I', h, 0x1C0)
images = []
for i in range(icount):
    addr, _, _, poff, _ = struct.unpack_from('<3Q2I', h, ioff + 32 * i)
    images.append((addr, h[poff : h.find(b'\0', poff)].decode()))
images.sort()
import bisect

keys = [a for a, _ in images]
found = 0
for sub in sorted(d.glob('dyld_shared_cache_arm64*')):
    if sub.suffix in ('.symbols',) or sub.name.endswith('.map'):
        continue
    hh, ms = maps(sub)
    for m in re.finditer(b'MTLB', hh):
        o = m.start()
        if o + 0x58 > len(hh):
            continue
        plat, vmaj, vmin, ftype, os_ = struct.unpack_from('<HHHBB', hh, o + 4)
        (size,) = struct.unpack_from('<Q', hh, o + 16)
        if vmaj not in (1, 2) or ftype > 3 or not (0x58 < size < 512 << 20) or o + size > len(hh):
            continue
        addr = next((a + (o - fo) for a, sz, fo in ms if fo <= o < fo + sz), None)
        if addr is None:
            continue
        k = bisect.bisect_right(keys, addr) - 1
        image = images[k][1] if k >= 0 else '?'
        name = image.rsplit('/', 1)[-1]
        dst = out / ('%s@%x.metallib' % (name, addr))
        dst.write_bytes(hh[o : o + size])
        found += 1
        print('%s\t%x\t%d\tv%d.%d type%d os%d\t%s' % (sub.name, addr, size, vmaj, vmin, ftype, os_, image))
print('found', found, file=sys.stderr)
