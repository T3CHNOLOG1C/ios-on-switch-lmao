"""The install image's manifest: how install-ios.bin rebuilds the iOS partition from part files on the FAT.

boot/installer/manifest.c decodes the same layout (all integers little-endian):

  header (128 bytes)
    0   magic "SWIMAGE1"            8   u32 version (1)       12  u32 part count (1..MAX_PARTS)
    16  u64 partition bytes (the target must be exactly this size)
    24  u32 block size (4096..65536, a power of two)            28  u32 extent count (1..MAX_EXTENTS)
    32  u64 source bytes (the parts' sizes added up)           40  u64 bytes written to the partition
    48  16 bytes: the APFS container UUID (the NXSB at partition offset 0)
    64  u32 CRC-32 of the whole manifest with this field zero  68  u32 hash chunk bytes (HASH_CHUNK)
    72  56 bytes: a NUL-padded ASCII label for the installer's screen
  part (256 bytes each)
    0   200 bytes: the NUL-padded ASCII path on the FAT       200 u64 bytes (below 4 GiB, a block multiple)
    208 48 bytes reserved (0)
  extent (24 bytes each)
    0   u64 partition offset   8   u64 source offset, or ZERO for a range written as zeros   16  u64 bytes
  hash (32 bytes each)
    SHA-256 of each HASH_CHUNK of the parts concatenated (the last one shorter), so the installer checks every chunk
    before it writes it.

The parts, concatenated, are exactly the copy extents' bytes in order. Extents are block aligned, sorted and disjoint;
the first copies the container superblock from source offset 0. The partition's other ranges are left as they are.
"""

import struct
import uuid
import zlib
from dataclasses import dataclass, field

MAGIC = b'SWIMAGE1'
VERSION = 1
HEADER = struct.Struct('<8sIIQIIQQ16sII56s')
PART = struct.Struct('<200sQ48s')
EXTENT = struct.Struct('<QQQ')
HASH_BYTES = 32
HASH_CHUNK = 8 << 20
ZERO = (1 << 64) - 1
MAX_PARTS = 64
MAX_EXTENTS = 1 << 16
PART_LIMIT = (4 << 30) - HASH_CHUNK
CRC_OFFSET = 64


class ManifestError(Exception):
    pass


@dataclass
class Part:
    path: str
    bytes: int


@dataclass
class Manifest:
    partition_bytes: int
    block_size: int
    container_uuid: uuid.UUID
    label: str
    parts: list = field(default_factory=list)
    extents: list = field(default_factory=list)
    hashes: list = field(default_factory=list)

    @property
    def source_bytes(self):
        return sum(p.bytes for p in self.parts)

    @property
    def write_bytes(self):
        return sum(n for _, _, n in self.extents)

    @property
    def copy_bytes(self):
        return sum(n for _, s, n in self.extents if s != ZERO)


def _ascii(text, size, what):
    raw = text.encode('ascii')
    if len(raw) >= size or b'\0' in raw:
        raise ManifestError(f'{what} {text!r} does not fit in {size - 1} bytes')
    return raw.ljust(size, b'\0')


def check(m):
    """The decoder's rules, so the Mac never writes a manifest the installer refuses."""
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
    if len(m.hashes) != -(-m.source_bytes // HASH_CHUNK) or any(len(h) != HASH_BYTES for h in m.hashes):
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


def encode(m):
    check(m)
    body = b''.join(PART.pack(_ascii(p.path, 200, 'path'), p.bytes, bytes(48)) for p in m.parts)
    body += b''.join(EXTENT.pack(*e) for e in m.extents)
    body += b''.join(m.hashes)
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
            _ascii(m.label, 56, 'label'),
        )
    )
    struct.pack_into('<I', header, CRC_OFFSET, zlib.crc32(bytes(header) + body))
    return bytes(header) + body


def decode(raw):
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
    ) = HEADER.unpack_from(raw)
    if magic != MAGIC or version != VERSION or chunk != HASH_CHUNK:
        raise ManifestError(f'not a version {VERSION} {MAGIC.decode()} manifest')
    hashes = -(-source_bytes // HASH_CHUNK)
    if len(raw) != HEADER.size + parts * PART.size + extents * EXTENT.size + hashes * HASH_BYTES:
        raise ManifestError(f'manifest is {len(raw)} bytes')
    zeroed = bytearray(raw)
    zeroed[CRC_OFFSET : CRC_OFFSET + 4] = bytes(4)
    if zlib.crc32(bytes(zeroed)) != crc:
        raise ManifestError('manifest CRC mismatch')
    m = Manifest(partition_bytes, block, uuid.UUID(bytes=container), label.rstrip(b'\0').decode('ascii'))
    at = HEADER.size
    for _ in range(parts):
        path, n, _reserved = PART.unpack_from(raw, at)
        m.parts.append(Part(path.rstrip(b'\0').decode('ascii'), n))
        at += PART.size
    m.extents = [EXTENT.unpack_from(raw, at + i * EXTENT.size) for i in range(extents)]
    at += extents * EXTENT.size
    m.hashes = [raw[at + i * HASH_BYTES : at + (i + 1) * HASH_BYTES] for i in range(hashes)]
    check(m)
    if m.source_bytes != source_bytes or m.write_bytes != write_bytes:
        raise ManifestError('header totals disagree with the records')
    return m


def split(extents, limit=PART_LIMIT):
    """Part sizes for the copy extents' bytes: as few files as FAT32 allows."""
    total = sum(n for _, s, n in extents if s != ZERO)
    sizes = [limit] * (total // limit)
    if total % limit:
        sizes.append(total % limit)
    return sizes
