"""The install image's manifest: how install-ios.bin rebuilds the iOS partition from part files on the FAT.

boot/installer/manifest.c decodes the same layout (all integers little-endian):

  header (128 bytes)
    0   magic "SWIMAGE1"            8   u32 version (2)       12  u32 part count (1..MAX_PARTS)
    16  u64 partition bytes (the target must be exactly this size)
    24  u32 block size (4096..65536, a power of two)            28  u32 extent count (1..MAX_EXTENTS)
    32  u64 source bytes (the parts' sizes added up)           40  u64 bytes written to the partition
    48  16 bytes: the APFS container UUID (the NXSB at partition offset 0)
    64  u32 CRC-32 of the whole manifest with this field zero  68  u32 hash chunk bytes (HASH_CHUNK)
    72  48 bytes: a NUL-padded ASCII label for the installer's screen
    120 u32 boot file count (1..MAX_BOOT)                      124 u32 reserved (0)
  part, then boot file (256 bytes each)
    0   200 bytes: the NUL-padded ASCII path on the FAT       200 u64 bytes
    208 48 bytes reserved (0)
  extent (24 bytes each)
    0   u64 partition offset   8   u64 source offset, or ZERO for a range written as zeros   16  u64 bytes
  hash (32 bytes each)
    SHA-256 of each HASH_CHUNK of the parts concatenated (the last one shorter), so the installer checks every chunk
    before it writes it; then each boot file's, chunked the same way.

The parts, concatenated, are exactly the copy extents' bytes in order. Extents are block aligned, sorted and disjoint;
the first copies the container superblock from source offset 0. The partition's other ranges are left as they are.
A part is below 4 GiB and a block multiple. A boot file is one of the kind's hekate boot entry (switchroot/ios/*),
which install-ios.bin copies there when it installs the image. Every part and boot file is in one folder, the
image's own, and no boot file is named manifest.bin or the user's bootargs.txt.
"""

import struct
import uuid
import zlib
from dataclasses import dataclass, field

MAGIC = b'SWIMAGE1'
VERSION = 2
HEADER = struct.Struct('<8sIIQIIQQ16sII48sII')
RECORD = struct.Struct('<200sQ48s')
EXTENT = struct.Struct('<QQQ')
HASH_BYTES = 32
HASH_CHUNK = 8 << 20
ZERO = (1 << 64) - 1
MAX_PARTS = 64
MAX_BOOT = 8
MAX_EXTENTS = 1 << 16
PART_LIMIT = (4 << 30) - HASH_CHUNK
BOOT_LIMIT = 64 << 20
BOOT_NAMES_REFUSED = ('manifest.bin', 'bootargs.txt')
CRC_OFFSET = 64


class ManifestError(Exception):
    pass


@dataclass
class Part:
    path: str
    bytes: int


@dataclass
class Boot:
    path: str
    bytes: int
    hashes: list = field(default_factory=list)


@dataclass
class Manifest:
    partition_bytes: int
    block_size: int
    container_uuid: uuid.UUID
    label: str
    parts: list = field(default_factory=list)
    extents: list = field(default_factory=list)
    hashes: list = field(default_factory=list)
    boot: list = field(default_factory=list)

    @property
    def source_bytes(self) -> int:
        return sum(p.bytes for p in self.parts)

    @property
    def write_bytes(self) -> int:
        return sum(n for _, _, n in self.extents)

    @property
    def copy_bytes(self) -> int:
        return sum(n for _, s, n in self.extents if s != ZERO)


def _ascii(text: str, size: int, what: str) -> bytes:
    raw = text.encode('ascii')
    if len(raw) >= size or b'\0' in raw:
        raise ManifestError(f'{what} {text!r} does not fit in {size - 1} bytes')
    return raw.ljust(size, b'\0')


def chunks(size: int) -> int:
    return -(-size // HASH_CHUNK)


def _check_files(m: Manifest) -> None:
    if not 0 < len(m.boot) <= MAX_BOOT:
        raise ManifestError(f'{len(m.boot)} boot files')
    for b in m.boot:
        if not 0 < b.bytes <= BOOT_LIMIT or len(b.hashes) != chunks(b.bytes):
            raise ManifestError(f'boot file {b.path} ({b.bytes} bytes, {len(b.hashes)} chunk hashes)')
    paths = [p.path for p in m.parts] + [b.path for b in m.boot]
    folder = paths[0].rpartition('/')[0]
    if not folder or any(p.rpartition('/')[0] != folder for p in paths):
        raise ManifestError(f'the parts and boot files are not all in one folder: {paths}')
    if len(set(paths)) != len(paths):
        raise ManifestError(f'a path is named twice: {paths}')
    for path in paths:
        if any(c in path for c in ':\\') or any(c in ('', '.', '..') for c in path.split('/')):
            raise ManifestError(f'{path!r} is not a FAT path the installer opens')
    for b in m.boot:
        if b.path.rpartition('/')[2] in BOOT_NAMES_REFUSED:
            raise ManifestError(f'boot file {b.path} would overwrite a file that is not the boot entry')


def check(m: Manifest) -> None:
    """Apply the decoder's rules, so the Mac never writes a manifest the installer refuses."""
    block = m.block_size
    if block < 4096 or block > 65536 or block & (block - 1):
        raise ManifestError(f'block size {block}')
    if not m.partition_bytes > 0 or m.partition_bytes % block:
        raise ManifestError(f'partition bytes {m.partition_bytes}')
    if not 0 < len(m.parts) <= MAX_PARTS or not 0 < len(m.extents) <= MAX_EXTENTS:
        raise ManifestError(f'{len(m.parts)} parts, {len(m.extents)} extents')
    for i, p in enumerate(m.parts):
        whole = HASH_CHUNK if i + 1 < len(m.parts) else block
        if not 0 < p.bytes <= PART_LIMIT or p.bytes % whole:
            raise ManifestError(f'part {p.path} ({p.bytes} bytes)')
    _check_files(m)
    every_hash = m.hashes + [h for b in m.boot for h in b.hashes]
    if len(m.hashes) != chunks(m.source_bytes) or any(len(h) != HASH_BYTES for h in every_hash):
        raise ManifestError(f'{len(m.hashes)} chunk hashes for {m.source_bytes} source bytes')
    image_end = source_end = 0
    for i, (at, source, n) in enumerate(m.extents):
        if not n or (at | n) % block or at < image_end or at + n > m.partition_bytes:
            raise ManifestError(f'extent {i}: {at:#x}+{n:#x}')
        if source != ZERO:
            if source != source_end:
                raise ManifestError(f'extent {i}: source {source:#x}, expected {source_end:#x}')
            source_end += n
        elif i == 0:
            raise ManifestError('the first extent must copy the container superblock')
        image_end = at + n
    if m.extents[0][0] != 0 or source_end != m.source_bytes:
        raise ManifestError(f'copy extents cover {source_end} source bytes of {m.source_bytes}')


def encode(m: Manifest) -> bytes:
    check(m)
    body = b''.join(RECORD.pack(_ascii(f.path, 200, 'path'), f.bytes, bytes(48)) for f in m.parts + m.boot)
    body += b''.join(EXTENT.pack(*e) for e in m.extents)
    body += b''.join(m.hashes) + b''.join(h for b in m.boot for h in b.hashes)
    header = bytearray(
        HEADER.pack(
            MAGIC,
            VERSION,
            len(m.parts),
            m.partition_bytes,
            m.block_size,
            len(m.extents),
            m.source_bytes,
            m.write_bytes,
            m.container_uuid.bytes,
            0,
            HASH_CHUNK,
            _ascii(m.label, 48, 'label'),
            len(m.boot),
            0,
        )
    )
    struct.pack_into('<I', header, CRC_OFFSET, zlib.crc32(bytes(header) + body))
    return bytes(header) + body


def decode(raw: bytes) -> Manifest:
    if len(raw) < HEADER.size:
        raise ManifestError('short manifest')
    (
        magic,
        version,
        parts,
        partition_bytes,
        block,
        extents,
        source_bytes,
        write_bytes,
        container,
        crc,
        chunk,
        label,
        boots,
        reserved,
    ) = HEADER.unpack_from(raw)
    if magic != MAGIC or version != VERSION or chunk != HASH_CHUNK or reserved:
        raise ManifestError(f'not a version {VERSION} {MAGIC.decode()} manifest')
    if len(raw) < HEADER.size + (parts + boots) * RECORD.size:
        raise ManifestError(f'manifest is {len(raw)} bytes')
    records = []
    for i in range(parts + boots):
        path, n, _reserved = RECORD.unpack_from(raw, HEADER.size + i * RECORD.size)
        records.append((path.rstrip(b'\0').decode('ascii'), n))
    hashes = chunks(source_bytes)
    every = hashes + sum(chunks(n) for _, n in records[parts:])
    at = HEADER.size + len(records) * RECORD.size
    if len(raw) != at + extents * EXTENT.size + every * HASH_BYTES:
        raise ManifestError(f'manifest is {len(raw)} bytes')
    zeroed = bytearray(raw)
    zeroed[CRC_OFFSET : CRC_OFFSET + 4] = bytes(4)
    if zlib.crc32(bytes(zeroed)) != crc:
        raise ManifestError('manifest CRC mismatch')
    m = Manifest(partition_bytes, block, uuid.UUID(bytes=container), label.rstrip(b'\0').decode('ascii'))
    m.parts = [Part(path, n) for path, n in records[:parts]]
    m.extents = [EXTENT.unpack_from(raw, at + i * EXTENT.size) for i in range(extents)]
    at += extents * EXTENT.size

    def take(count: int) -> list[bytes]:
        nonlocal at
        taken = [raw[at + i * HASH_BYTES : at + (i + 1) * HASH_BYTES] for i in range(count)]
        at += count * HASH_BYTES
        return taken

    m.hashes = take(hashes)
    m.boot = [Boot(path, n, take(chunks(n))) for path, n in records[parts:]]
    check(m)
    if m.source_bytes != source_bytes or m.write_bytes != write_bytes:
        raise ManifestError('header totals disagree with the records')
    return m


def split(extents: list[tuple[int, int, int]], limit: int = PART_LIMIT) -> list[int]:
    """Part sizes for the copy extents' bytes: as few files as FAT32 allows."""
    total = sum(n for _, s, n in extents if s != ZERO)
    sizes = [limit] * (total // limit)
    if total % limit:
        sizes.append(total % limit)
    return sizes
