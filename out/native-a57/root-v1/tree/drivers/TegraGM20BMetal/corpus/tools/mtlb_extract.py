#!/usr/bin/env python3
"""Extract functions' bitcode from a metallib (AIR64 slice of a universal one too) and disassemble with llvm-dis.
usage: mtlb_extract.py <metallib> <out dir> [name ...]   (no names: list functions)"""
import struct, sys, subprocess
from pathlib import Path
d = Path(sys.argv[1]).read_bytes()
if d[:4] == b'\xca\xfe\xba\xbe':
    for i in range(struct.unpack_from('>I', d, 4)[0]):
        cpu, sub, off, size, al = struct.unpack_from('>5I', d, 8 + 20 * i)
        if cpu == 0x01000017: d = d[off:off + size]; break
assert d[:4] == b'MTLB'
fl_off, fl_size, pub_off, pub_size, priv_off, priv_size, bc_off, bc_size = struct.unpack_from('<8Q', d, 0x18)
count, = struct.unpack_from('<I', d, fl_off)
o = fl_off + 4
funcs = []
for _ in range(count):
    esize, = struct.unpack_from('<I', d, o)
    p, end = o + 4, o + esize
    info = {}
    while p < end:
        tag = d[p:p + 4]; p += 4
        if tag == b'ENDT': break
        ln, = struct.unpack_from('<H', d, p); p += 2
        val = d[p:p + ln]; p += ln
        if tag == b'NAME': info['name'] = val.rstrip(b'\0').decode()
        elif tag == b'TYPE': info['type'] = val[0]
        elif tag == b'OFFT': info['offt'] = struct.unpack_from('<3Q', val)
        elif tag == b'MDSZ': info['mdsz'], = struct.unpack_from('<Q', val)
    funcs.append(info)
    o = end
out = Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
want = sys.argv[3:]
for f in funcs:
    if not want:
        print(f.get('type'), f['name']); continue
    if f['name'] not in want: continue
    start = bc_off + f['offt'][2]
    bc = d[start:start + f['mdsz']]
    (out / (f['name'] + '.bc')).write_bytes(bc)
    r = subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-dis', str(out / (f['name'] + '.bc')), '-o', str(out / (f['name'] + '.ll'))], capture_output=True, text=True)
    print(f['name'], 'ok' if r.returncode == 0 else r.stderr[:300])
