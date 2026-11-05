#!/usr/bin/env python3
"""Minimal MH_FILESET reader (kernelcache): entries, per-entry segments/sections, symtab, chained fixups."""
import struct

class Fileset:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        self.ncmds = struct.unpack_from("<I", d, 16)[0]
        self.entries = []          # (id, vmaddr, fileoff)
        self.segs = []             # top-level segments
        self.fixups = None
        p = 32
        for _ in range(self.ncmds):
            cmd, size = struct.unpack_from("<II", d, p)
            if cmd == 0x80000035:
                vm, fo, no = struct.unpack_from("<QQI", d, p + 8)
                self.entries.append((d[p + no:p + size].split(b"\0")[0].decode(), vm, fo))
            elif cmd == 0x19:
                self.segs.append(self.seg(p))
            elif cmd == 0x80000034:
                self.fixups = struct.unpack_from("<II", d, p + 8)
            p += size

    def seg(self, p):
        d = self.d
        name, va, vs, fo, fs, maxp, initp, ns, fl = struct.unpack_from("<16s4Q4I", d, p + 8)
        secs = []
        for k in range(ns):
            sp = p + 72 + 80 * k
            sn, sg, a, s, o, al, ro, nr, f = struct.unpack_from("<16s16sQQIIIII", d, sp)
            secs.append(dict(name=sn.rstrip(b"\0").decode(), seg=sg.rstrip(b"\0").decode(), va=a, size=s, off=o, flags=f))
        return dict(name=name.rstrip(b"\0").decode(), va=va, size=vs, off=fo, filesize=fs, maxprot=maxp, initprot=initp,
                    sections=secs, cmd_off=p)

    def entry_cmds(self, fo):
        d = self.d
        n = struct.unpack_from("<I", d, fo + 16)[0]
        p = fo + 32
        for _ in range(n):
            cmd, size = struct.unpack_from("<II", d, p)
            yield cmd, p, size
            p += size

    def entry_segs(self, fo):
        return [self.seg(p) for cmd, p, size in self.entry_cmds(fo) if cmd == 0x19]

    def symbols(self, fo):
        """(name, n_type, n_value) of an entry's LC_SYMTAB"""
        d = self.d
        out = []
        for cmd, p, size in self.entry_cmds(fo):
            if cmd == 2:
                symoff, nsyms, stroff, strsize = struct.unpack_from("<4I", d, p + 8)
                for i in range(nsyms):
                    strx, typ, sect, desc, val = struct.unpack_from("<IBBHQ", d, symoff + 16 * i)
                    e = d.find(b"\0", stroff + strx)
                    out.append((d[stroff + strx:e].decode(errors="replace"), typ, val))
        return out

    def va2off(self, va):
        for s in self.segs:
            if s["va"] <= va < s["va"] + s["filesize"]:
                return s["off"] + va - s["va"]
        return None
