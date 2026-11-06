#!/usr/bin/env python3
"""The static trust cache bundle the kernel window carries.

    trust.py --static BUNDLE --app IM4P [--module FILE]... --out BUNDLE

The IPSW's static bundle covers the root and the OS cryptex (//ipsw:static_system_trust). The App cryptex (Safari,
webbookmarksd, ...) brings its own trust cache, which ignition would load on iOS; it is appended, then each --module in
order: the overlay's own code (overlay.py trust; an empty file adds nothing) and the GPU bundle's (//gpu:trustcache).
"""

import argparse
import struct
from pathlib import Path


def im4p_payload(data: bytes) -> bytes:
    """Return the payload of an IM4P (SEQUENCE { "IM4P", type, description, OCTET STRING payload, ... })."""

    def header(at: int) -> tuple[int, int, int]:
        tag, length, at = data[at], data[at + 1], at + 2
        if length & 0x80:
            count = length & 0x7F
            length, at = int.from_bytes(data[at : at + count], 'big'), at + count
        return tag, at, length

    _, at, length = header(0)
    end, fields = at + length, []
    while at < end:
        tag, start, length = header(at)
        fields.append((tag, data[start : start + length]))
        at = start + length
    if fields[0][1] != b'IM4P':
        raise SystemExit('not an IM4P')
    return next(value for tag, value in fields if tag == 0x04)


def trust_cache_bundle(bundle: bytes, *modules: bytes) -> bytes:
    """`bundle` (u32 count, u32 offsets, version-2 modules) with `modules` appended."""
    count = struct.unpack_from('<I', bundle)[0]
    starts = struct.unpack_from(f'<{count}I', bundle, 4)
    existing = [bundle[s:e] for s, e in zip(starts, (*starts[1:], len(bundle)))]
    merged = existing + [m for m in modules if m not in existing]
    offset, offsets = 4 + 4 * len(merged), []
    for module in merged:
        offsets.append(offset)
        offset += len(module)
    return struct.pack(f'<I{len(merged)}I', len(merged), *offsets) + b''.join(merged)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('static', 'app', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--module', action='append', default=[], type=Path)
    args = parser.parse_args()
    modules = [im4p_payload(args.app.read_bytes())] + [m for m in (p.read_bytes() for p in args.module) if m]
    args.out.write_bytes(trust_cache_bundle(args.static.read_bytes(), *modules))


if __name__ == '__main__':
    main()
