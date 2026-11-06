"""Two Mach-O kernels that may differ only in __LINKEDIT: symbol names, and what the linker derives from them.

    bazel run //tools/repro:linkedit -- OLD NEW [--removed SYMBOL]...

Checks, and refuses unless all hold:
1. every byte of every segment but __LINKEDIT is the same, except the load commands that only describe __LINKEDIT
   (its segment command, LC_SYMTAB, LC_DYSYMTAB, the linkedit data commands) and LC_UUID; and except, anywhere else, a
   span that is exactly the old UUID where the new one has the new UUID (raw or as its hex string), or the prelink
   info's _PrelinkKCID (tools/prelink/emit_prelink.py: a hash of the whole native kernel, its __LINKEDIT included);
2. the symbols match as (address, type, section, description) and only their names differ; a symbol named by
   --removed may be gone, if it is absolute (no section: no byte of either kernel can depend on it).
Prints the UUIDs and every renamed symbol (old -> new) per address.
"""

import argparse
import collections
import struct
import sys
import uuid
from collections.abc import Collection
from pathlib import Path

LC_SEGMENT_64, LC_SYMTAB, LC_DYSYMTAB, LC_UUID = 0x19, 0x2, 0xB, 0x1B
# Commands whose payload is offsets and sizes into __LINKEDIT (function starts, data in code, code signature, split
# info, dyld info, chained fixups, exports trie).
LINKEDIT_DATA = {0x1D, 0x1E, 0x22, 0x80000022, 0x26, 0x29, 0x2B, 0x2E, 0x80000033, 0x80000034}

SymbolKey = tuple[int, int, int, int]
Symbol = tuple[SymbolKey, str]
Span = tuple[int, int]
Findings = tuple[list[str], list[str]]


def parse(data: bytes) -> dict:
    magic, _, _, _, ncmds, sizeofcmds = struct.unpack_from('<6I', data)
    if magic != 0xFEEDFACF:
        raise SystemExit('not a 64-bit Mach-O')
    at, segments, symtab, image_uuid, masked = 32, [], None, None, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == LC_SEGMENT_64:
            name = data[at + 8 : at + 24].rstrip(b'\0').decode()
            _, _, fileoff, filesize = struct.unpack_from('<4Q', data, at + 24)
            segments.append((name, fileoff, filesize))
            if name == '__LINKEDIT':
                masked.append((at, at + size))
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from('<4I', data, at + 8)
            masked.append((at, at + size))
        elif cmd == LC_UUID:
            image_uuid = data[at + 8 : at + 24]
            masked.append((at, at + size))
        elif cmd == LC_DYSYMTAB or cmd in LINKEDIT_DATA:
            masked.append((at, at + size))
        at += size
    if at != 32 + sizeofcmds:
        raise SystemExit('load command sizes do not add up')
    return {'segments': segments, 'symtab': symtab, 'uuid': image_uuid, 'masked': masked}


def symbols(data: bytes, symtab: tuple[int, int, int, int]) -> list[Symbol]:
    symoff, nsyms, stroff, _ = symtab
    out = []
    for i in range(nsyms):
        strx, kind, sect, desc, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
        end = data.index(b'\0', stroff + strx)
        out.append(((value, kind, sect, desc), data[stroff + strx : end].decode(errors='replace')))
    return out


def uuid_spans(image_uuid: bytes) -> list[bytes]:
    """Return the forms a UUID takes in a kernel's bytes: raw, and as the hex string the prelink info carries."""
    text = str(uuid.UUID(bytes=bytes(image_uuid)))
    return [bytes(image_uuid), text.upper().encode(), text.encode()]


def differing_spans(old: bytes, new: bytes, lo: int, hi: int) -> list[Span]:
    spans, at = [], lo
    while at < hi:
        if old[at] != new[at]:
            start = at
            while at < hi and old[at] != new[at]:
                at += 1
            spans.append((start, at))
        else:
            at += 1
    return spans


def uuid_explains(old: bytes, new: bytes, span: Span, forms: list[tuple[bytes, bytes]]) -> bool:
    """Tell whether a differing span lies inside one place holding the old UUID in OLD and the new UUID in NEW."""
    start, end = span
    for old_form, new_form in forms:
        width = len(old_form)
        for base in range(max(0, end - width), start + 1):
            if old[base : base + width] == old_form and new[base : base + width] == new_form:
                return True
    return False


KCID = b'<key>_PrelinkKCID</key><data>'


def kcid_explains(old: bytes, new: bytes, span: Span) -> bool:
    """Tell whether a differing span is inside the base64 of the prelink info's KCID, at one place in both kernels."""
    start, end = span
    at = old.rfind(KCID, 0, start + 1)
    if at < 0 or new[at : at + len(KCID)] != KCID:
        return False
    data = at + len(KCID)
    close = old.find(b'</data>', data)
    return close >= end and new.find(b'</data>', data) == close


def segment_differences(old: bytes, new: bytes, a: dict, b: dict) -> Findings:
    """Return the report lines and failures for every segment but __LINKEDIT."""
    report, failures = [], []
    forms = list(zip(uuid_spans(a['uuid']), uuid_spans(b['uuid']), strict=True)) if a['uuid'] and b['uuid'] else []
    for (name, fileoff, filesize), (_, _, new_size) in zip(a['segments'], b['segments'], strict=True):
        if name == '__LINKEDIT':
            continue
        if filesize != new_size:
            failures.append(f'{name}: {filesize:#x} bytes, now {new_size:#x}')
            continue
        for span in differing_spans(old, new, fileoff, fileoff + filesize):
            start, end = span
            if any(lo <= start and end <= hi for lo, hi in a['masked']):
                continue
            if uuid_explains(old, new, span, forms):
                report.append(f'{name} +{start - fileoff:#x}: the UUID ({end - start} bytes)')
            elif kcid_explains(old, new, span):
                report.append(f'{name} +{start - fileoff:#x}: the prelink info KCID ({end - start} bytes)')
            else:
                failures.append(f'{name} +{start - fileoff:#x}..+{end - fileoff:#x} differs')
    return report, failures


def drop_removed(old_symbols: list[Symbol], new_symbols: list[Symbol], removed: Collection[str]) -> Findings:
    """Take each absolute symbol named by --removed that NEW lacks out of OLD's symbols."""
    report, failures = [], []
    for key, name in [(k, n) for k, n in old_symbols if n in removed]:
        if key[1] & 0x0E != 0x2 or key[2] != 0:
            failures.append(f'{name} is not an absolute symbol')
        elif not any(n == name for _, n in new_symbols):
            old_symbols.remove((key, name))
            report.append(f'{key[0]:#x} absolute: {name} removed')
    return report, failures


def renames(old_symbols: list[Symbol], new_symbols: list[Symbol]) -> Findings:
    """Return each address's renamed symbols, or the failure if the symbols differ in more than their names."""
    by_key_old, by_key_new = collections.defaultdict(list), collections.defaultdict(list)
    for key, name in old_symbols:
        by_key_old[key].append(name)
    for key, name in new_symbols:
        by_key_new[key].append(name)
    if collections.Counter(k for k, _ in old_symbols) != collections.Counter(k for k, _ in new_symbols):
        gone = set(by_key_old) - set(by_key_new)
        added = set(by_key_new) - set(by_key_old)
        failures = [f'the symbols differ beyond names: {len(gone)} keys gone, {len(added)} new']
        failures += [f'  gone {k[0]:#x} type {k[1]:#x}: {by_key_old[k]}' for k in sorted(gone)[:20]]
        failures += [f'  new  {k[0]:#x} type {k[1]:#x}: {by_key_new[k]}' for k in sorted(added)[:20]]
        return [], failures
    report = []
    for key in sorted(by_key_old):
        old_names, new_names = sorted(by_key_old[key]), sorted(by_key_new[key])
        if old_names != new_names:
            gone = sorted(set(old_names) - set(new_names))
            came = sorted(set(new_names) - set(old_names))
            report.append(f'{key[0]:#x} type {key[1]:#x}: {", ".join(gone)} -> {", ".join(came)}')
    return report, []


def compare(old: bytes, new: bytes, removed: Collection[str] = ()) -> Findings:
    """Return the report lines and the list of failures for two kernels."""
    a, b = parse(old), parse(new)
    if [(n, o) for n, o, _ in a['segments']] != [(n, o) for n, o, _ in b['segments']]:
        return [], ['the segments are not laid out alike']
    report = []
    if a['uuid'] != b['uuid']:
        report.append(f'LC_UUID {uuid.UUID(bytes=bytes(a["uuid"]))} -> {uuid.UUID(bytes=bytes(b["uuid"]))}')
    segment_report, failures = segment_differences(old, new, a, b)
    old_symbols, new_symbols = symbols(old, a['symtab']), symbols(new, b['symtab'])
    removed_report, removed_failures = drop_removed(old_symbols, new_symbols, removed)
    rename_report, rename_failures = renames(old_symbols, new_symbols)
    return report + segment_report + removed_report + rename_report, failures + removed_failures + rename_failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('old', type=Path)
    parser.add_argument('new', type=Path)
    parser.add_argument('--removed', action='append', default=[], help='an absolute symbol that may be gone')
    args = parser.parse_args()
    report, failures = compare(args.old.read_bytes(), args.new.read_bytes(), set(args.removed))
    print('\n'.join(report))
    if failures:
        sys.exit('\n'.join(['NOT LINKEDIT-only:', *failures]))
    print('LINKEDIT-only: every other byte is the same and the symbols differ only in name')


if __name__ == '__main__':
    main()
