#!/usr/bin/env python3
"""Which dyld-cache images reference the Metal selectors that compile or link shaders at run time.

By their __objc_selrefs, resolved through the cache.
usage: runtime_compile_callers.py <annot.py dir> <dyld cache dir> <extracted images root> <image list>
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
import annot

annot.CACHE = Path(sys.argv[2])
WANT = [
    'newLibraryWithSource:options:error:',
    'newLibraryWithSource:options:completionHandler:',
    'newLibraryWithStitchedDescriptor:error:',
    'newLibraryWithStitchedDescriptor:completionHandler:',
    'newLibraryWithDescriptorSPI:error:',
    'newLibraryWithImageFilterFunctionsSPI:imageFilterFunctionInfo:error:',
    'newLibraryWithCIFilters:imageFilterFunctionInfo:error:',
    'newLibraryWithDAG:functions:error:',
    'newDynamicLibrary:error:',
    'newDynamicLibraryWithURL:error:',
    'newFunctionWithDescriptor:error:',
    'newIntersectionFunctionWithDescriptor:error:',
    'newVisibleFunctionTableWithDescriptor:',
]
cache = annot.Cache()
root = Path(sys.argv[3])


def selrefs(path: Path) -> list[tuple[int, int]]:
    d = path.read_bytes()[:0x10000]
    if len(d) < 32 or struct.unpack_from('<I', d, 0)[0] != 0xFEEDFACF:
        return []
    ncmds = struct.unpack_from('<I', d, 16)[0]
    off = 32
    out = []
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<2I', d, off)
        if cmd == 0x19:
            nsect = struct.unpack_from('<I', d, off + 64)[0]
            so = off + 72
            for _s in range(nsect):
                if d[so : so + 16].split(b'\0')[0] == b'__objc_selrefs':
                    out.append(struct.unpack_from('<2Q', d, so + 32))
                so += 80
        off += csz
    return out


for line in open(sys.argv[4]):
    addr, image = line.split(None, 1)
    image = image.strip()
    p = root / image.lstrip('/')
    if not p.is_file():
        continue
    hits = set()
    for addr, size in selrefs(p):
        for i in range(0, size, 8):
            v = cache.ptr(addr + i)
            if v:
                s = cache.cstr(v, 80)
                if s in WANT:
                    hits.add(s)
    if hits:
        print('%s\t%s' % (image, ' '.join(sorted(hits))), flush=True)
