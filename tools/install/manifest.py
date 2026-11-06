"""The install image (NAME.SII): one stream, split into NAME.SII.001, .002, ... only because FAT32 caps a file at 4 GiB.

    [0, BOOT_OFFSET)            the manifest, zero-padded
    [BOOT_OFFSET, BODY_OFFSET)  the version's boot files (switchroot/ios/*), zero-padded
    [BODY_OFFSET, ...)          the container's bytes: the copy extents' sources, concatenated

The container is composed for one size and fitted to the partition by install-ios.bin (boot/installer/apfs.c), so an
image fits any partition of at least its minimum. boot/installer/manifest.c decodes the same manifest (all integers
little-endian):

  header (128 bytes)
    0   magic "SWIMAGE1"            8   u32 version (3)       12  u32 part count (1..MAX_PARTS)
    16  u64 container bytes (the container as composed, what its superblock says)
    24  u32 block size (4096..65536, a power of two)            28  u32 extent count (1..MAX_EXTENTS)
    32  u64 source bytes (the body)                            40  u64 bytes written to the partition
    48  16 bytes: the APFS container UUID (the NXSB at partition offset 0)
    64  u32 CRC-32 of the manifest with this field zero        68  u32 hash chunk bytes (HASH_CHUNK)
    72  40 bytes: a NUL-padded ASCII label for the installer's screen
    112 u64 minimum bytes: the smallest partition the image is installed to
    120 u32 boot file count (1..MAX_BOOT)                      124 u32 reserved (0)
  part (256 bytes each): 200 bytes NUL-padded FAT path, 200 u64 bytes, 48 reserved (0)
  boot file (256 bytes each): 200 bytes NUL-padded name, 200 u64 bytes, 208 u64 stream offset, 40 reserved (0)
  extent (24 bytes each): u64 partition offset, u64 source offset (or ZERO: written as zeros), u64 bytes
  hash (32 bytes each): SHA-256 of each HASH_CHUNK of the body (the last one shorter), then of each boot file's

Parts are in one folder; every part but the last is a whole number of hash chunks, so no chunk spans two files, the
first holds the whole head, and together they are the stream. Extents are block aligned, sorted, disjoint and below
the minimum; the first copies the container superblock from the body's start. Boot files lie in their region in
order, named neither manifest.bin nor the user's bootargs.txt.
"""

import struct
import uuid
import zlib
from dataclasses import dataclass, field

MAGIC = b'SWIMAGE1'
VERSION = 3
HEADER = struct.Struct('<8sIIQIIQQ16sII40sQII')
PART = struct.Struct('<200sQ48s')
BOOT = struct.Struct('<200sQQ40s')
EXTENT = struct.Struct('<QQQ')
HASH_BYTES = 32
HASH_CHUNK = 8 << 20
BOOT_OFFSET = HASH_CHUNK
BODY_OFFSET = BOOT_OFFSET + (64 << 20)
ZERO = (1 << 64) - 1
MAX_PARTS = 64
MAX_BOOT = 8
MAX_EXTENTS = 1 << 16
PART_LIMIT = (4 << 30) - HASH_CHUNK
BOOT_NAMES_REFUSED = ('', '.', '..', 'manifest.bin', 'bootargs.txt')
CRC_OFFSET = 64


class ManifestError(Exception):
    pass


@dataclass
class Part:
    path: str
    bytes: int


@dataclass
class Boot:
    name: str
    offset: int
    bytes: int
    hashes: list = field(default_factory=list)


@dataclass
class Manifest:
    container_bytes: int
    minimum_bytes: int
    block_size: int
    container_uuid: uuid.UUID
    label: str
    parts: list = field(default_factory=list)
    extents: list = field(default_factory=list)
    hashes: list = field(default_factory=list)
    boot: list = field(default_factory=list)

    @property
    def source_bytes(self) -> int:
        return sum(n for _, s, n in self.extents if s != ZERO)

    @property
    def write_bytes(self) -> int:
        return sum(n for _, _, n in self.extents)


def chunks(size: int) -> int:
    return -(-size // HASH_CHUNK)


def _ascii(text: str, size: int, what: str) -> bytes:
    raw = text.encode('ascii')
    if len(raw) >= size or b'\0' in raw or any(c < 32 or c > 126 for c in raw):
        raise ManifestError(f'{what} {text!r} does not fit in {size - 1} printable bytes')
    return raw.ljust(size, b'\0')


def _check_parts(m: Manifest) -> None:
    if not 0 < len(m.parts) <= MAX_PARTS:
        raise ManifestError(f'{len(m.parts)} parts')
    folder = m.parts[0].path.rpartition('/')[0]
    paths = [p.path for p in m.parts]
    if not folder or any(p.rpartition('/')[0] != folder for p in paths) or len(set(paths)) != len(paths):
        raise ManifestError(f'the parts are not distinct files of one folder: {paths}')
    for path in paths:
        if any(c in path for c in ':\\') or any(c in ('', '.', '..') for c in path.split('/')):
            raise ManifestError(f'{path!r} is not a FAT path the installer opens')
    for i, p in enumerate(m.parts):
        whole = HASH_CHUNK if i + 1 < len(m.parts) else m.block_size
        if not 0 < p.bytes <= PART_LIMIT or p.bytes % whole:
            raise ManifestError(f'part {p.path} ({p.bytes} bytes)')
    if sum(p.bytes for p in m.parts) != BODY_OFFSET + m.source_bytes or m.parts[0].bytes < BODY_OFFSET:
        raise ManifestError('the parts are not the stream, or the first does not hold the head')


def _check_boot(m: Manifest) -> None:
    if not 0 < len(m.boot) <= MAX_BOOT or len({b.name for b in m.boot}) != len(m.boot):
        raise ManifestError(f'{len(m.boot)} boot files')
    end = BOOT_OFFSET
    for b in m.boot:
        if b.name in BOOT_NAMES_REFUSED or '/' in b.name:
            raise ManifestError(f'boot file {b.name!r} would land on a file that is not the boot entry')
        if b.bytes <= 0 or b.offset < end or b.offset + b.bytes > BODY_OFFSET or len(b.hashes) != chunks(b.bytes):
            raise ManifestError(f'boot file {b.name} ({b.bytes} bytes at {b.offset})')
        end = b.offset + b.bytes


def check(m: Manifest) -> None:
    """Apply the decoder's rules, so the Mac never writes a manifest the installer refuses."""
    block = m.block_size
    if block < 4096 or block > 65536 or block & (block - 1):
        raise ManifestError(f'block size {block}')
    if not 0 < m.minimum_bytes <= m.container_bytes or (m.minimum_bytes | m.container_bytes) % block:
        raise ManifestError(f'container {m.container_bytes} bytes, minimum {m.minimum_bytes}')
    if not 0 < len(m.extents) <= MAX_EXTENTS:
        raise ManifestError(f'{len(m.extents)} extents')
    _check_parts(m)
    _check_boot(m)
    every = m.hashes + [h for b in m.boot for h in b.hashes]
    if len(m.hashes) != chunks(m.source_bytes) or any(len(h) != HASH_BYTES for h in every):
        raise ManifestError(f'{len(m.hashes)} chunk hashes for {m.source_bytes} source bytes')
    _check_extents(m)


def _check_extents(m: Manifest) -> None:
    block, image_end, source_end = m.block_size, 0, 0
    for i, (at, source, n) in enumerate(m.extents):
        if not n or (at | n) % block or at < image_end or at + n > m.minimum_bytes:
            raise ManifestError(f'extent {i}: {at:#x}+{n:#x}')
        if source != ZERO:
            if source != source_end:
                raise ManifestError(f'extent {i}: source {source:#x}, expected {source_end:#x}')
            source_end += n
        elif i == 0:
            raise ManifestError('the first extent must copy the container superblock')
        image_end = at + n
    if m.extents[0][0] != 0:
        raise ManifestError('the first extent does not start the container')


def encode(m: Manifest) -> bytes:
    check(m)
    body = b''.join(PART.pack(_ascii(p.path, 200, 'path'), p.bytes, bytes(48)) for p in m.parts)
    body += b''.join(BOOT.pack(_ascii(b.name, 200, 'name'), b.bytes, b.offset, bytes(40)) for b in m.boot)
    body += b''.join(EXTENT.pack(*e) for e in m.extents)
    body += b''.join(m.hashes) + b''.join(h for b in m.boot for h in b.hashes)
    fields = (MAGIC, VERSION, len(m.parts), m.container_bytes, m.block_size, len(m.extents), m.source_bytes)
    fields += (m.write_bytes, m.container_uuid.bytes, 0, HASH_CHUNK, _ascii(m.label, 40, 'label'))
    header = bytearray(HEADER.pack(*fields, m.minimum_bytes, len(m.boot), 0))
    struct.pack_into('<I', header, CRC_OFFSET, zlib.crc32(bytes(header) + body))
    raw = bytes(header) + body
    if len(raw) > BOOT_OFFSET:
        raise ManifestError(f'the manifest is {len(raw)} bytes, more than its {BOOT_OFFSET}')
    return raw


def _read_records(raw: bytes, m: Manifest, parts: int, boots: int) -> None:
    for i in range(parts):
        path, n, _ = PART.unpack_from(raw, HEADER.size + i * PART.size)
        m.parts.append(Part(path.rstrip(b'\0').decode('ascii'), n))
    for i in range(boots):
        name, n, offset, _ = BOOT.unpack_from(raw, HEADER.size + (parts + i) * PART.size)
        m.boot.append(Boot(name.rstrip(b'\0').decode('ascii'), offset, n))


def decode(raw: bytes) -> Manifest:
    """Decode the manifest at the start of `raw` (a whole manifest, or the stream's head)."""
    if len(raw) < HEADER.size:
        raise ManifestError('short manifest')
    fields = HEADER.unpack_from(raw)
    magic, version, parts, container_bytes, block, extents, source_bytes, write_bytes, container = fields[:9]
    crc, chunk, label, minimum_bytes, boots, reserved = fields[9:]
    if magic != MAGIC or version != VERSION or chunk != HASH_CHUNK or reserved:
        raise ManifestError(f'not a version {VERSION} {MAGIC.decode()} manifest')
    at = HEADER.size + (parts + boots) * PART.size
    if len(raw) < at:
        raise ManifestError('short manifest')
    m = Manifest(container_bytes, minimum_bytes, block, uuid.UUID(bytes=container), label.rstrip(b'\0').decode())
    _read_records(raw, m, parts, boots)
    length = at + extents * EXTENT.size + (chunks(source_bytes) + sum(chunks(b.bytes) for b in m.boot)) * HASH_BYTES
    if len(raw) < length:
        raise ManifestError(f'manifest is {len(raw)} bytes of {length}')
    zeroed = bytearray(raw[:length])
    zeroed[CRC_OFFSET : CRC_OFFSET + 4] = bytes(4)
    if zlib.crc32(bytes(zeroed)) != crc:
        raise ManifestError('manifest CRC mismatch')
    m.extents = [EXTENT.unpack_from(raw, at + i * EXTENT.size) for i in range(extents)]
    at += extents * EXTENT.size

    def take(count: int) -> list[bytes]:
        nonlocal at
        taken = [raw[at + i * HASH_BYTES : at + (i + 1) * HASH_BYTES] for i in range(count)]
        at += count * HASH_BYTES
        return taken

    m.hashes = take(chunks(source_bytes))
    for b in m.boot:
        b.hashes = take(chunks(b.bytes))
    check(m)
    if m.source_bytes != source_bytes or m.write_bytes != write_bytes:
        raise ManifestError('header totals disagree with the records')
    return m


def split(total: int, limit: int = PART_LIMIT) -> list[int]:
    """Part sizes for a stream of `total` bytes: as few files as FAT32 allows."""
    sizes = [limit] * (total // limit)
    if total % limit:
        sizes.append(total % limit)
    return sizes
