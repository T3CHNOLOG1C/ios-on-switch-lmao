"""Version-2 trust cache modules, as the kernel's trust cache loader reads them.

One copy for every tool that builds one. Entries are sorted by CDHash with no CDHash twice: the kernel
binary-searches the module and refuses one that is unsorted or repeats an entry.
"""

import hashlib
import struct
from collections.abc import Iterable

VERSION = 2
HASH_TYPE_SHA256 = 2
CDHASH_BYTES = 20
UUID_BYTES = 16

type Entry = bytes | tuple[bytes, int, int]


def trust_module(entries: Iterable[Entry], uuid: bytes | None = None) -> bytes:
    """Build the module for `entries`.

    An entry is a CDHash (20 bytes; flags and launch-constraint category 0) or a (CDHash, flags, category) tuple.
    The same CDHash may come more than once with the same flags and category; with different ones it is an error,
    since only one entry can stand. `uuid` (16 bytes) defaults to the first 16 bytes of the entries' SHA-256.
    """
    unique = {}
    for entry in entries:
        cdhash, flags, category = (entry, 0, 0) if isinstance(entry, bytes | bytearray) else entry
        cdhash = bytes(cdhash)
        if len(cdhash) != CDHASH_BYTES:
            raise ValueError(f'trust cache: CDHash {cdhash.hex()} is {len(cdhash)} bytes, not {CDHASH_BYTES}')
        if unique.setdefault(cdhash, (flags, category)) != (flags, category):
            raise ValueError(
                f'trust cache: {cdhash.hex()} given with flags/category {unique[cdhash]} and {(flags, category)}'
            )
    body = b''.join(
        cdhash + bytes([HASH_TYPE_SHA256, flags, category, 0]) for cdhash, (flags, category) in sorted(unique.items())
    )
    if uuid is None:
        uuid = hashlib.sha256(body).digest()[:UUID_BYTES]
    if len(uuid) != UUID_BYTES:
        raise ValueError(f'trust cache: UUID of {len(uuid)} bytes')
    return struct.pack('<I16sI', VERSION, bytes(uuid), len(unique)) + body
