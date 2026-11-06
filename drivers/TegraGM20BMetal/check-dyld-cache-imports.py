#!/usr/bin/env python3
"""Check that every import of the plugin's binaries exists in the target's dyld shared cache (iPadOS 17.7.11 21H461).

The bundle and NVK then load there although they are linked against a newer iOS SDK.

usage: check-dyld-cache-imports.py <dyld cache dir> <Mach-O>...
  <dyld cache dir>  System/Library/Caches/com.apple.dyld of the OS cryptex
                    (vendor/unixnu-qemu/.cache/firmware/target-volumes/044-76034-038.dmg, attached read-only)
Imports come from `dyld_info -imports`, libraries from `otool -L`; each library's exports are read from the cache's
export tries, following LC_REEXPORT_DYLIB. Flat-namespace and weak-def-coalesce imports are skipped (resolved elsewhere
or weak); a missing weak import is listed but not fatal. Exit status 1 if anything non-weak is missing.
"""

import mmap
import re
import struct
import subprocess
import sys
from pathlib import Path


class Cache:
    def __init__(self, directory: str):
        files = sorted(
            p
            for p in Path(directory).glob('dyld_shared_cache_arm64*')
            if not p.name.endswith(('.symbols', '.map', '.atlas'))
        )
        self.maps = []  # (va, end, fileoff, data)
        self.main = None
        for p in files:
            f = p.open('rb')
            data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            if data[:16].rstrip(b'\0') != b'dyld_v1   arm64':
                continue
            moff, mcount = struct.unpack_from('<2I', data, 0x10)
            for i in range(mcount):
                va, size, off, _, _ = struct.unpack_from('<3Q2I', data, moff + 32 * i)
                self.maps.append((va, va + size, off, data))
            if p.name == 'dyld_shared_cache_arm64':
                self.main = data
        h = self.main
        ioff, icount = struct.unpack_from('<2I', h, 0x1C0)
        if not icount:
            ioff, icount = struct.unpack_from('<2I', h, 0x18)
        self.images = {}
        for i in range(icount):
            addr, _, _, poff, _ = struct.unpack_from('<3Q2I', h, ioff + 32 * i)
            path = h[poff : h.find(b'\0', poff)].decode()
            self.images[path] = addr
        self.cache = {}

    def read(self, va: int, n: int) -> bytes:
        for lo, hi, off, data in self.maps:
            if lo <= va < hi:
                return data[off + va - lo : off + va - lo + n]
        raise KeyError(hex(va))

    def macho(self, path: str) -> list[tuple[int, bytes]]:
        va = self.images[path]
        magic, _, _, _, ncmds, sizeofcmds = struct.unpack('<6I', self.read(va, 24))
        assert magic == 0xFEEDFACF
        lc = self.read(va + 32, sizeofcmds)
        cmds, at = [], 0
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<2I', lc, at)
            cmds.append((cmd, lc[at : at + size]))
            at += size
        return cmds

    def exports(self, path: str, seen: set[str] | None = None) -> set[str]:
        if path in self.cache:
            return self.cache[path]
        seen = seen or set()
        if path in seen:
            return set()
        seen.add(path)
        cmds = self.macho(path)
        linkedit = trie = None
        reexports = []
        for cmd, body in cmds:
            if cmd == 0x19 and body[8:24].rstrip(b'\0') == b'__LINKEDIT':
                vmaddr, _vmsize, fileoff = struct.unpack_from('<3Q', body, 24)
                linkedit = (vmaddr, fileoff)
            elif cmd == 0x80000033:  # LC_DYLD_EXPORTS_TRIE
                trie = struct.unpack_from('<2I', body, 8)
            elif cmd in (0x22, 0x80000022):  # LC_DYLD_INFO(_ONLY)
                off, size = struct.unpack_from('<2I', body, 40)
                if size:
                    trie = (off, size)
            elif cmd == 0x8000001F:  # LC_REEXPORT_DYLIB
                noff = struct.unpack_from('<I', body, 8)[0]
                reexports.append(body[noff : body.index(b'\0', noff)].decode())
        names = set()
        if trie and trie[1]:
            data = self.read(linkedit[0] + trie[0] - linkedit[1], trie[1])

            def uleb(at: int) -> tuple[int, int]:
                v = s = 0
                while True:
                    b = data[at]
                    at += 1
                    v |= (b & 0x7F) << s
                    s += 7
                    if b < 0x80:
                        return v, at

            stack = [(0, b'')]
            while stack:
                node, prefix = stack.pop()
                size, at = uleb(node)
                if size:
                    names.add(prefix.decode('latin1'))
                at += size
                count = data[at]
                at += 1
                for _ in range(count):
                    end = data.index(b'\0', at)
                    edge = data[at:end]
                    at = end + 1
                    child, at = uleb(at)
                    stack.append((child, prefix + edge))
        for r in reexports:
            if r in self.images:
                names |= self.exports(r, seen)
            else:
                print(f'  (re-export {r} not in cache)')
        self.cache[path] = names
        return names


def main() -> None:
    cache = Cache(sys.argv[1])
    bad = 0
    for binary in sys.argv[2:]:
        libs = [
            line.split(' (compat')[0].strip()
            for line in subprocess.check_output(['otool', '-L', binary], text=True).splitlines()[1:]
        ]
        leaf = {}
        for lib in libs:
            name = Path(lib).name
            leaf[name.split('.')[0]] = lib
        missing = []
        for line in subprocess.check_output(['dyld_info', '-imports', binary], text=True).splitlines():
            m = re.match(r'\s+0x[0-9A-F]+\s+(\S+)\s+\(from ([^)]*)\)(.*)', line)
            if not m:
                continue
            sym, frm, rest = m.groups()
            if frm.startswith('<'):  # flat-namespace / weak-def-coalesce: resolved elsewhere or weak
                continue
            install = leaf.get(frm)
            if install is None or install.startswith('@rpath'):
                missing.append((sym, frm, 'library not found'))
                continue
            if install not in cache.images:
                missing.append((sym, frm, install + ' not in cache'))
                continue
            if sym not in cache.exports(install):
                missing.append((sym, frm, 'weak' if 'weak' in rest else 'MISSING'))
        print(binary, len(missing), 'unresolved')
        for s in missing:
            print('  ', *s)
        bad += sum(1 for s in missing if s[2] != 'weak')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
