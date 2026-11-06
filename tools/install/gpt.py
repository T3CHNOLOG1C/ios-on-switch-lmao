"""GPT and (hybrid) MBR reading and the one edit the installer makes: one entry's type GUID and name.

The Switch SD card has a hybrid MBR at LBA 0 (FAT32 0x0C, emuMMC 0xE0, protective 0xEE) that hekate and Horizon
read, so nothing here ever rewrites LBA 0 or uses macOS partitioning tools, which may replace a hybrid MBR with a
protective one. An edit rewrites exactly: the target entry in the primary and backup entry arrays, and the two
headers (their entry-array CRC and header CRC). Everything else on the disk keeps its bytes.
"""

import os
import struct
import uuid
import zlib
from dataclasses import dataclass, field

SECTOR = 512
APFS = uuid.UUID('7C3457EF-0000-11AA-AA11-00306543ECAC')
LINUX_DATA = uuid.UUID('0FC63DAF-8483-4772-8E79-3D69D8477DE4')
BASIC_DATA = uuid.UUID('EBD0A0A2-B9E5-4433-87C0-68B6B72699C7')
EMUMMC = uuid.UUID('11CA7E00-0000-0000-0000-656D754D4D43')
UNUSED = uuid.UUID(int=0)
NAMES = {APFS: 'APFS', LINUX_DATA: 'Linux data', BASIC_DATA: 'Microsoft basic data', EMUMMC: 'emuMMC'}

_HEADER = struct.Struct('<8sIIIIQQQQ16sQIII')


class GptError(Exception):
    pass


@dataclass
class Entry:
    index: int
    type: uuid.UUID
    guid: uuid.UUID
    first: int
    last: int
    attributes: int
    name: str

    @property
    def sectors(self) -> int:
        return self.last - self.first + 1

    @property
    def bytes(self) -> int:
        return self.sectors * SECTOR

    def describe(self) -> str:
        return (
            f'#{self.index} {self.name!r} {NAMES.get(self.type, str(self.type))} {str(self.guid).upper()} '
            f'LBA {self.first}-{self.last} ({self.bytes} bytes)'
        )


@dataclass
class MbrPartition:
    index: int
    status: int
    kind: int
    first: int
    sectors: int


@dataclass
class Table:
    disk_sectors: int
    mbr: bytes
    primary_header: bytes
    backup_header: bytes
    entries_lba: int
    backup_entries_lba: int
    entry_count: int
    entry_size: int
    raw_entries: bytes
    entries: list = field(default_factory=list)
    mbr_partitions: list = field(default_factory=list)
    disk_guid: uuid.UUID = None

    @property
    def used(self) -> list[Entry]:
        return [e for e in self.entries if e.type != UNUSED]

    @property
    def entry_sectors(self) -> int:
        return (self.entry_count * self.entry_size + SECTOR - 1) // SECTOR

    def hybrid(self) -> bool:
        """Return whether LBA 0 has real partitions besides the protective 0xEE one."""
        return any(p.kind not in (0, 0xEE) for p in self.mbr_partitions)


def _pread(fd: int, lba: int, sectors: int) -> bytes:
    data = os.pread(fd, sectors * SECTOR, lba * SECTOR)
    if len(data) != sectors * SECTOR:
        raise GptError(f'short read at LBA {lba}')
    return data


def _parse_header(raw: bytes, lba: int) -> dict[str, int | uuid.UUID]:
    (
        signature,
        revision,
        size,
        crc,
        _reserved,
        current,
        backup,
        first_usable,
        last_usable,
        disk_guid,
        entries_lba,
        count,
        entry_size,
        entries_crc,
    ) = _HEADER.unpack_from(raw)
    if signature != b'EFI PART':
        raise GptError(f'no GPT header at LBA {lba}')
    if size < 92 or size > SECTOR:
        raise GptError(f'bad GPT header size {size} at LBA {lba}')
    check = bytearray(raw[:size])
    check[16:20] = b'\0\0\0\0'
    if zlib.crc32(check) != crc:
        raise GptError(f'GPT header CRC mismatch at LBA {lba}')
    if current != lba:
        raise GptError(f'GPT header at LBA {lba} says it is at {current}')
    if entry_size < 128 or entry_size % 8 or count * entry_size > 64 * 1024 or not count:
        raise GptError(f'bad GPT entry geometry at LBA {lba}')
    return dict(
        size=size,
        backup=backup,
        first_usable=first_usable,
        last_usable=last_usable,
        disk_guid=uuid.UUID(bytes_le=disk_guid),
        entries_lba=entries_lba,
        count=count,
        entry_size=entry_size,
        entries_crc=entries_crc,
    )


def parse_entries(raw: bytes, count: int, size: int) -> list[Entry]:
    entries = []
    for i in range(count):
        e = raw[i * size : (i + 1) * size]
        first, last, attributes = struct.unpack_from('<QQQ', e, 32)
        entries.append(
            Entry(
                i,
                uuid.UUID(bytes_le=e[:16]),
                uuid.UUID(bytes_le=e[16:32]),
                first,
                last,
                attributes,
                e[56:128].decode('utf-16-le').split('\0', 1)[0],
            )
        )
    return entries


def read(fd: int, disk_bytes: int) -> Table:
    """Return the disk's partition table; both GPT copies must be present, valid and identical."""
    if disk_bytes % SECTOR:
        raise GptError(f'disk size {disk_bytes} is not a multiple of 512')
    sectors = disk_bytes // SECTOR
    mbr = _pread(fd, 0, 1)
    if mbr[510:512] != b'\x55\xaa':
        raise GptError('no MBR signature at LBA 0')
    mbr_parts = []
    for i in range(4):
        status, kind = mbr[446 + 16 * i], mbr[446 + 16 * i + 4]
        first, count = struct.unpack_from('<II', mbr, 446 + 16 * i + 8)
        mbr_parts.append(MbrPartition(i, status, kind, first, count))
    primary_raw = _pread(fd, 1, 1)
    primary = _parse_header(primary_raw, 1)
    if primary['backup'] != sectors - 1:
        raise GptError('backup GPT header expected at LBA {}, primary says {}'.format(sectors - 1, primary['backup']))
    backup_raw = _pread(fd, sectors - 1, 1)
    backup = _parse_header(backup_raw, sectors - 1)
    if (backup['count'], backup['entry_size'], backup['disk_guid']) != (
        primary['count'],
        primary['entry_size'],
        primary['disk_guid'],
    ):
        raise GptError('primary and backup GPT headers disagree')
    span = (primary['count'] * primary['entry_size'] + SECTOR - 1) // SECTOR
    raw = _pread(fd, primary['entries_lba'], span)[: primary['count'] * primary['entry_size']]
    backup_entries = _pread(fd, backup['entries_lba'], span)[: primary['count'] * primary['entry_size']]
    if zlib.crc32(raw) != primary['entries_crc']:
        raise GptError('primary GPT entry array CRC mismatch')
    if zlib.crc32(backup_entries) != backup['entries_crc'] or backup_entries != raw:
        raise GptError('backup GPT entry array differs from the primary')
    table = Table(
        sectors,
        mbr,
        primary_raw,
        backup_raw,
        primary['entries_lba'],
        backup['entries_lba'],
        primary['count'],
        primary['entry_size'],
        raw,
        parse_entries(raw, primary['count'], primary['entry_size']),
        mbr_parts,
        primary['disk_guid'],
    )
    for e in table.used:
        if not primary['first_usable'] <= e.first <= e.last <= primary['last_usable']:
            raise GptError(f'entry {e.describe()} lies outside the usable LBAs')
    return table


def _encode_name(name: str) -> bytes:
    encoded = name.encode('utf-16-le')
    if len(encoded) > 72:
        raise GptError(f'partition name {name!r} is longer than 36 UTF-16 units')
    return encoded.ljust(72, b'\0')


def _rewrite_header(raw: bytes, entries_crc: int) -> bytes:
    size = struct.unpack_from('<I', raw, 12)[0]
    out = bytearray(raw)
    struct.pack_into('<I', out, 88, entries_crc)
    out[16:20] = b'\0\0\0\0'
    struct.pack_into('<I', out, 16, zlib.crc32(bytes(out[:size])))
    return bytes(out)


def edit(table: Table, index: int, type_guid: uuid.UUID, name: str) -> list[tuple[int, bytes]]:
    """Return the sector writes that give entry `index` a new type GUID and name: [(lba, bytes)], primary and backup.

    Only that entry's 16 type bytes and 72 name bytes change, plus the two headers' CRCs.
    """
    entry = table.entries[index]
    if entry.type == UNUSED:
        raise GptError(f'entry {index} is unused')
    raw = bytearray(table.raw_entries)
    at = index * table.entry_size
    raw[at : at + 16] = type_guid.bytes_le
    raw[at + 56 : at + 128] = _encode_name(name)
    raw = bytes(raw)
    crc = zlib.crc32(raw)
    span = table.entry_sectors * SECTOR
    # Rewrite only the sectors of the entry array that hold the entry (the rest keep their bytes).
    first_sector, last_sector = at // SECTOR, (at + table.entry_size - 1) // SECTOR
    padded = raw.ljust(span, b'\0')
    writes = []
    for base in (table.entries_lba, table.backup_entries_lba):
        for s in range(first_sector, last_sector + 1):
            writes.append((base + s, padded[s * SECTOR : (s + 1) * SECTOR]))
    writes.append((1, _rewrite_header(table.primary_header, crc)))
    writes.append((table.disk_sectors - 1, _rewrite_header(table.backup_header, crc)))
    return writes


def build(
    disk_sectors: int,
    partitions: list[tuple[uuid.UUID, uuid.UUID, int, int, str]],
    disk_guid: uuid.UUID | None = None,
) -> list[tuple[int, bytes]]:
    """Build a fresh GPT with a protective MBR for a scratch image: partitions [(type, guid, first, last, name)].

    Returns [(lba, bytes)] covering LBA 0-33 and the backup's last 33 sectors.
    """
    count, size = 128, 128
    raw = bytearray(count * size)
    for i, (kind, guid, first, last, name) in enumerate(partitions):
        at = i * size
        raw[at : at + 16] = kind.bytes_le
        raw[at + 16 : at + 32] = guid.bytes_le
        struct.pack_into('<QQQ', raw, at + 32, first, last, 0)
        raw[at + 56 : at + 128] = _encode_name(name)
    raw = bytes(raw)
    crc = zlib.crc32(raw)
    disk_guid = disk_guid or uuid.uuid4()

    def header(current: int, backup: int, entries_lba: int) -> bytes:
        out = bytearray(SECTOR)
        _HEADER.pack_into(
            out,
            0,
            b'EFI PART',
            0x10000,
            92,
            0,
            0,
            current,
            backup,
            34,
            disk_sectors - 34,
            disk_guid.bytes_le,
            entries_lba,
            count,
            size,
            crc,
        )
        struct.pack_into('<I', out, 16, zlib.crc32(bytes(out[:92])))
        return bytes(out)

    mbr = bytearray(SECTOR)
    mbr[446 + 4] = 0xEE
    struct.pack_into('<II', mbr, 446 + 8, 1, min(disk_sectors - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xaa'
    writes = [(0, bytes(mbr)), (1, header(1, disk_sectors - 1, 2))]
    writes += [(2 + i, raw[i * SECTOR : (i + 1) * SECTOR]) for i in range(32)]
    writes += [(disk_sectors - 33 + i, raw[i * SECTOR : (i + 1) * SECTOR]) for i in range(32)]
    writes.append((disk_sectors - 1, header(disk_sectors - 1, 1, disk_sectors - 33)))
    return writes


def apply(fd: int, writes: list[tuple[int, bytes]]) -> None:
    for lba, data in writes:
        if len(data) != SECTOR or lba == 0:
            raise GptError(f'refusing a write to LBA {lba} ({len(data)} bytes)')
        if os.pwrite(fd, data, lba * SECTOR) != SECTOR:
            raise GptError(f'short write at LBA {lba}')
    os.fsync(fd)
