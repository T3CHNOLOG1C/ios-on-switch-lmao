#!/usr/bin/env python3
"""Which dyld-cache image has a segment covering each given address. usage: dsc_owner.py <dsc dir> <hexaddr>..."""

import mmap, struct, sys
from pathlib import Path

d = Path(sys.argv[1])
want = [int(a, 16) for a in sys.argv[2:]]
subs = []
for p in sorted(d.glob('dyld_shared_cache_arm64*')):
    if p.name.endswith('.symbols'):
        continue
    f = p.open('rb')
    h = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    moff, mcount = struct.unpack_from('<2I', h, 0x10)
    subs.append((h, [struct.unpack_from('<3Q', h, moff + 32 * i) for i in range(mcount)]))


def read(addr, n):
    for h, ms in subs:
        for a, sz, fo in ms:
            if a <= addr < a + sz:
                return h[fo + addr - a : fo + addr - a + n]
    return None


h = subs[0][0] if False else None
main = (d / 'dyld_shared_cache_arm64').open('rb')
mh = mmap.mmap(main.fileno(), 0, access=mmap.ACCESS_READ)
ioff, icount = struct.unpack_from('<2I', mh, 0x1C0)
for i in range(icount):
    addr, _, _, poff, _ = struct.unpack_from('<3Q2I', mh, ioff + 32 * i)
    path = mh[poff : mh.find(b'\0', poff)].decode()
    hdr = read(addr, 32)
    if not hdr:
        continue
    ncmds, sizeofcmds = struct.unpack_from('<II', hdr, 16)
    cmds = read(addr + 32, sizeofcmds)
    o = 0
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', cmds, o)
        if cmd == 0x19:
            seg = cmds[o + 8 : o + 24].rstrip(b'\0').decode()
            vm, vs = struct.unpack_from('<QQ', cmds, o + 24)
            (nsect,) = struct.unpack_from('<I', cmds, o + 64)
            for w in want:
                if vm <= w < vm + vs:
                    sect = ''
                    for s in range(nsect):
                        so = o + 72 + 80 * s
                        sa, ss = struct.unpack_from('<QQ', cmds, so + 32)
                        if sa <= w < sa + ss:
                            sect = cmds[so : so + 16].rstrip(b'\0').decode()
                    print('%x\t%s\t%s,%s' % (w, path, seg, sect))
        o += cs
