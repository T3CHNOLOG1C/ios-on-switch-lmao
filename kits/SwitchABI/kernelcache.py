"""What extract.py reads of a kernelcache: classes (the ipsw CLI's C++ view), PAC discriminators, code and strings."""

import bisect
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parents[2] / 'translate/arm64e/xnu/prelink26'))
from kcinfo import KC

M64 = (1 << 64) - 1
Slot = tuple[int, int, int | None]
_SIPHASH_KEY = bytes.fromhex('b5d4c9eb79104a796fec8b1b428781d4')


def _rotl(x: int, b: int) -> int:
    return ((x << b) | (x >> (64 - b))) & M64


def siphash24(data: bytes, key: bytes = _SIPHASH_KEY) -> int:
    k0, k1 = struct.unpack('<QQ', key)
    v = [k0 ^ 0x736F6D6570736575, k1 ^ 0x646F72616E646F6D, k0 ^ 0x6C7967656E657261, k1 ^ 0x7465646279746573]

    def rounds(n: int) -> None:
        for _ in range(n):
            v[0] = (v[0] + v[1]) & M64
            v[1] = _rotl(v[1], 13) ^ v[0]
            v[0] = _rotl(v[0], 32)
            v[2] = (v[2] + v[3]) & M64
            v[3] = _rotl(v[3], 16) ^ v[2]
            v[0] = (v[0] + v[3]) & M64
            v[3] = _rotl(v[3], 21) ^ v[0]
            v[2] = (v[2] + v[1]) & M64
            v[1] = _rotl(v[1], 17) ^ v[2]
            v[2] = _rotl(v[2], 32)

    full = len(data) - len(data) % 8
    for i in range(0, full, 8):
        m = struct.unpack_from('<Q', data, i)[0]
        v[3] ^= m
        rounds(2)
        v[0] ^= m
    last = (len(data) & 0xFF) << 56
    for i, c in enumerate(data[full:]):
        last |= c << (8 * i)
    v[3] ^= last
    rounds(2)
    v[0] ^= last
    v[2] ^= 0xFF
    rounds(4)
    return v[0] ^ v[1] ^ v[2] ^ v[3]


def discriminator(name: str) -> int:
    """Clang's __builtin_ptrauth_string_discriminator: the PAC diversity of a vtable slot whose root method is name."""
    return siphash24(name.encode()) % 0xFFFF + 1


def sx(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class Fileset:
    """An arm64e MH_FILESET kernelcache, with the ipsw CLI's class table and inheritance."""

    def __init__(self, path: Path, ipsw: str):
        self.kc = KC(str(path))
        self.data = self.kc.d
        self.classes, self.inheritance = _ipsw_classes(path, ipsw)
        self.extents = sorted(
            (s['va'], s['va'] + s['size'], bid)
            for bid, _, fo in self.kc.entries
            for s in self.kc.entry_segs(fo)
            if s['size'] and s['name'] != '__LINKEDIT'
        )
        self._starts = {}

    def owner(self, address: int) -> str | None:
        i = bisect.bisect_right(self.extents, (address, M64, '')) - 1
        return self.extents[i][2] if i >= 0 and address < self.extents[i][1] else None

    def word(self, address: int) -> int:
        return struct.unpack_from('<I', self.data, self.kc.va2off(address))[0]

    def slots(self, cls: str) -> list[Slot]:
        return [(m['index'], int(m['target'], 16), m.get('pac')) for m in self.classes[cls]['methods']]

    def text(self, bundle: str) -> tuple[int, int]:
        _, fo = self.kc.entry[bundle]
        seg = next(s for s in self.kc.entry_segs(fo) if s['initprot'] & 4 and s['size'])
        return seg['va'], seg['va'] + seg['size']

    def function_starts(self, bundle: str) -> list[int]:
        if bundle not in self._starts:
            _, fo = self.kc.entry[bundle]
            base = next(s for s in self.kc.entry_segs(fo) if s['name'] == '__TEXT')['va']
            starts = []
            for cmd, at, _ in self.kc.entry_cmds(fo):
                if cmd == 0x26:
                    off, size = struct.unpack_from('<II', self.data, at + 8)
                    i, address = off, base
                    while i < off + size:
                        value, shift = 0, 0
                        while True:
                            byte = self.data[i]
                            i += 1
                            value |= (byte & 0x7F) << shift
                            shift += 7
                            if not byte & 0x80:
                                break
                        if not value:
                            break
                        address += value
                        starts.append(address)
            self._starts[bundle] = starts
        return self._starts[bundle]

    def function(self, address: int) -> tuple[int, int]:
        bundle = self.owner(address)
        starts = self.function_starts(bundle)
        i = bisect.bisect_right(starts, address) - 1
        end = starts[i + 1] if i + 1 < len(starts) else self.text(bundle)[1]
        return starts[i], end

    def words(self, lo: int, hi: int) -> list[tuple[int, int]]:
        at = self.kc.va2off(lo)
        return [(lo + 4 * k, struct.unpack_from('<I', self.data, at + 4 * k)[0]) for k in range((hi - lo) // 4)]

    def strings(self, text: str) -> list[int]:
        """Addresses of NUL-terminated strings that start with text."""
        out, i, pattern = [], 0, text.encode()
        while (i := self.data.find(pattern, i)) >= 0:
            if i == 0 or self.data[i - 1] == 0:
                out.append(_file_to_va(self.kc.segs, i))
            i += 1
        return [a for a in out if a is not None]


def _file_to_va(segments: list[dict[str, int | str]], offset: int) -> int | None:
    for s in segments:
        if s['off'] <= offset < s['off'] + s['filesize']:
            return s['va'] + offset - s['off']
    return None


def _ipsw_classes(path: Path, ipsw: str) -> tuple[dict[str, dict[str, object]], dict[str, dict[str, int | str]]]:
    with tempfile.TemporaryDirectory() as home:
        env = dict(os.environ, HOME=home)
        cpp = subprocess.run(
            [ipsw, 'kernel', 'cpp', '--json', '--methods', str(path)], env=env, check=True, capture_output=True
        ).stdout
        tree = subprocess.run(
            [ipsw, 'kernel', 'cpp', '--inheritance', str(path)], env=env, check=True, capture_output=True, text=True
        ).stdout
    classes = {c['class']: c for c in json.loads(cpp)}
    inheritance = {}
    for line in re.sub(r'\x1b\[[0-9;]*m', '', tree).splitlines():
        m = re.match(r'init=(\S+) size=(\S+) parent=(\S+) meta=(\S+) metavtab=\S+ vtab=(\S+) (\S+)\s+\((\S+)\)', line)
        if m:
            inheritance[m.group(6)] = {
                'size': int(m.group(2), 16),
                'parent': int(m.group(3), 16),
                'meta': int(m.group(4), 16),
                'vtable': int(m.group(5), 16),
                'bundle': m.group(7),
            }
    return classes, inheritance


def branch(pc: int, word: int) -> tuple[int, bool] | None:
    """(target, link) of a B or BL."""
    if (word & 0x7C000000) == 0x14000000:
        return pc + 4 * sx(word & 0x3FFFFFF, 26), bool(word >> 31)
    return None


def references(words: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """(pc, address) of each ADRP-based address: ADRP then ADD or an unsigned-offset load/store on its register."""
    pages, out = {}, []
    for pc, w in words:
        if (w & 0x9F000000) == 0x90000000:
            imm = ((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3
            pages[w & 31] = (pc & ~0xFFF) + (sx(imm, 21) << 12)
            continue
        rn = (w >> 5) & 31
        if rn not in pages:
            continue
        if (w & 0xFF800000) == 0x91000000:
            value = pages[rn] + (((w >> 10) & 0xFFF) << (12 if (w >> 22) & 1 else 0))
            out.append((pc, value))
            pages[w & 31] = value
        elif (w & 0x3B000000) == 0x39000000:
            out.append((pc, pages[rn] + ((w >> 10) & 0xFFF) * (1 << (w >> 30))))
    return out
