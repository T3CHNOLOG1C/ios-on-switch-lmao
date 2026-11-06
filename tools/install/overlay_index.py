"""Data:/private/var/Switch/pretranslated.index: the System paths of iOS 27's translated System volume files, which the
kernel shows from Switch/pretranslated/<path> by a lookup redirect it builds from this index once, before launchd.

Little-endian: "SWOV", u32 version (1), u32 count, u32 string bytes; u32 offsets[count] into the string table; the
string table, count NUL-terminated absolute paths, sorted by bytes and unique, every byte belonging to one path.
"""

import struct

MAGIC = b'SWOV'
VERSION = 1
HEADER = struct.Struct('<4sIII')
MAX_PATH = 1024


class OverlayIndexError(ValueError):
    pass


def check_path(path: str) -> None:
    if (
        not path.startswith('/')
        or path.endswith('/')
        or '//' in path
        or any(part in ('.', '..') for part in path.split('/'))
        or '\0' in path
        or len(path.encode()) >= MAX_PATH
    ):
        raise OverlayIndexError(f'{path!r} is not a path the overlay index takes')


def encode(paths: list[str]) -> bytes:
    encoded = sorted({p.encode() for p in paths})
    if len(encoded) != len(paths):
        raise OverlayIndexError('the overlay index lists a path twice')
    for p in encoded:
        check_path(p.decode())
    offsets, strings = [], b''
    for p in encoded:
        offsets.append(len(strings))
        strings += p + b'\0'
    return (
        HEADER.pack(MAGIC, VERSION, len(encoded), len(strings)) + struct.pack(f'<{len(offsets)}I', *offsets) + strings
    )


def decode(raw: bytes) -> list[str]:
    if len(raw) < HEADER.size:
        raise OverlayIndexError('short overlay index')
    magic, version, count, size = HEADER.unpack_from(raw)
    if magic != MAGIC or version != VERSION or len(raw) != HEADER.size + 4 * count + size:
        raise OverlayIndexError(f'not a version {VERSION} overlay index')
    offsets = struct.unpack_from(f'<{count}I', raw, HEADER.size)
    strings = raw[HEADER.size + 4 * count :]
    paths, at = [], 0
    for offset in offsets:
        end = strings.find(b'\0', offset)
        if offset != at or end < 0:
            raise OverlayIndexError('the overlay index strings are not packed in order')
        paths.append(strings[offset:end].decode())
        at = end + 1
    if at != size or paths != sorted(set(paths), key=str.encode):
        raise OverlayIndexError('the overlay index is not sorted, unique and packed')
    for p in paths:
        check_path(p)
    return paths
