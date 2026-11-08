"""The emulated microSD card: a sparse 2 TiB raw image, hybrid MBR, GPT, the iOS APFS partition and hekate's FAT.

QEMU's SD card must be a power of two, so the card is 2 TiB (SDXC's largest) rather than the user's 1 TB. The APFS
container sits at LBA 2048, where the image builder's composed or populated image (tools/install) already has it, so
the base is an APFS clone of that image and costs no copy. hekate's FAT is GPT entry 1 (U-Boot's partition 1), placed
after the container. The base is made once per container (cache.py); each run boots a clone of it.
"""

import json
import os
import struct
import subprocess
import uuid
import zlib
from pathlib import Path

from tools.emu import cache

SECTOR = 512
CARD_BYTES = 2 << 40
APFS_FIRST = 2048
FAT_BYTES = 256 << 20
ENTRIES = 128
ENTRY_BYTES = 128
ENTRY_SECTORS = ENTRIES * ENTRY_BYTES // SECTOR
APFS_TYPE = uuid.UUID('7C3457EF-0000-11AA-AA11-00306543ECAC')
BASIC_DATA = uuid.UUID('EBD0A0A2-B9E5-4433-87C0-68B6B72699C7')
# Fixed GUIDs: the card is the same card on every run, as the real one is.
DISK_GUID = uuid.UUID('5357494B-0000-4000-8000-000000000001')
FAT_GUID = uuid.UUID('5357494B-0000-4000-8000-000000000002')
IOS_GUID = uuid.UUID('5357494B-0000-4000-8000-000000000003')
LAYOUT_REVISION = 1


def _entry(kind: uuid.UUID, unique: uuid.UUID, first: int, last: int, name: str) -> bytes:
    return (
        kind.bytes_le + unique.bytes_le + struct.pack('<QQQ', first, last, 0) + name.encode('utf-16le').ljust(72, b'\0')
    )


def _header(current: int, backup: int, entries_lba: int, array_crc: int, last_usable: int) -> bytes:
    header = bytearray(
        struct.pack(
            '<8sIIII4Q16sQIII',
            b'EFI PART',
            0x10000,
            92,
            0,
            0,
            current,
            backup,
            34,
            last_usable,
            DISK_GUID.bytes_le,
            entries_lba,
            ENTRIES,
            ENTRY_BYTES,
            array_crc,
        )
    )
    struct.pack_into('<I', header, 16, zlib.crc32(header))
    return bytes(header).ljust(SECTOR, b'\0')


def _mbr(fat_first: int, fat_sectors: int) -> bytes:
    """Return hekate's hybrid MBR: its FAT32 as entry 1, the rest of the card behind a protective 0xEE entry."""
    mbr = bytearray(SECTOR)
    struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\xfe\xff\xff', 0x0C, b'\xfe\xff\xff', fat_first, fat_sectors)
    struct.pack_into('<B3sB3sII', mbr, 462, 0, b'\0\2\0', 0xEE, b'\xfe\xff\xff', 1, fat_first - 1)
    mbr[510:512] = b'\x55\xaa'
    return bytes(mbr)


def layout(container_sectors: int) -> dict:
    total = CARD_BYTES // SECTOR
    ios_last = APFS_FIRST + container_sectors - 1
    fat_first = (ios_last + 1 + 2047) // 2048 * 2048
    return {
        'total': total,
        'ios': (APFS_FIRST, ios_last),
        'fat': (fat_first, fat_first + FAT_BYTES // SECTOR - 1),
    }


def write_tables(f: 'os.FileIO', plan: dict) -> None:
    total = plan['total']
    array = _entry(BASIC_DATA, FAT_GUID, *plan['fat'], 'hos_data') + _entry(APFS_TYPE, IOS_GUID, *plan['ios'], 'iOS')
    array = array.ljust(ENTRIES * ENTRY_BYTES, b'\0')
    crc = zlib.crc32(array)
    last_usable = total - 1 - ENTRY_SECTORS - 1
    writes = {
        0: _mbr(plan['fat'][0], plan['fat'][1] - plan['fat'][0] + 1),
        1: _header(1, total - 1, 2, crc, last_usable),
        2: array,
        total - 1 - ENTRY_SECTORS: array,
        total - 1: _header(total - 1, 1, total - 1 - ENTRY_SECTORS, crc, last_usable),
    }
    for lba, data in writes.items():
        f.seek(lba * SECTOR)
        f.write(data)


def _mtools(image: Path, offset: int, *args: str) -> None:
    subprocess.run([args[0], '-i', f'{image}@@{offset}', *args[1:]], check=True, capture_output=True)


def format_fat(image: Path, plan: dict) -> None:
    first, last = plan['fat']
    _mtools(
        image,
        first * SECTOR,
        'mformat',
        '-F',
        '-T',
        str(last - first + 1),
        '-h',
        '255',
        '-s',
        '63',
        '-H',
        str(first),
        '-v',
        'SWITCH SD',
        '::',
    )


def copy_files(image: Path, plan: dict, files: dict[str, Path]) -> None:
    """Copy files onto the FAT (card path -> host file), making their directories."""
    offset = plan['fat'][0] * SECTOR
    made = set()
    for card in sorted(files):
        parts = card.split('/')[:-1]
        for i in range(1, len(parts) + 1):
            directory = '/'.join(parts[:i])
            if directory not in made:
                _mtools(image, offset, 'mmd', '-D', 's', '::/' + directory)
                made.add(directory)
        _mtools(image, offset, 'mcopy', '-o', str(files[card]), '::/' + card)


def container_sectors(source: Path | None) -> int:
    if source is None:
        return (1 << 30) // SECTOR
    with source.open('rb') as f:
        f.seek(SECTOR + 0x48)
        (entries_lba,) = struct.unpack('<Q', f.read(8))
        f.seek(entries_lba * SECTOR + 32)
        first, last = struct.unpack('<QQ', f.read(16))
    if first != APFS_FIRST:
        raise SystemExit(f'{source}: its APFS partition starts at LBA {first}, not {APFS_FIRST}')
    return last - first + 1


def source_identity(source: Path | None) -> dict:
    """Identify a container image without hashing a terabyte: the image builder's stage keys, size and mtime."""
    if source is None:
        return {}
    state = source.with_name('state.json')
    stat = source.stat()
    return {
        'path': str(source),
        'state': json.loads(state.read_text()) if state.exists() else None,
        'size': stat.st_size,
        'mtime': stat.st_mtime_ns,
    }


def base(source: Path | None) -> Path:
    """Return the card with the container and an empty FAT, built once per container (cache.py)."""
    key = cache.digest({'source': source_identity(source), 'revision': LAYOUT_REVISION, 'fat': FAT_BYTES})

    def make(tmp: Path, _: Path) -> None:
        image = tmp / 'card.img'
        if source is not None:
            subprocess.run(['/bin/cp', '-c', str(source), str(image)], check=True)
        plan = layout(container_sectors(source))
        with image.open('r+b' if source else 'wb') as f:
            f.truncate(CARD_BYTES)
            write_tables(f, plan)
        format_fat(image, plan)
        (tmp / 'layout.json').write_text(json.dumps(plan))

    return cache.entry('sd-base', key, make)


def card_for_run(source: Path | None, run: Path, files: dict[str, Path]) -> Path:
    """Return a clone of the base for one run (writes never reach the base), with this run's boot files on its FAT."""
    entry = base(source)
    run.mkdir(parents=True, exist_ok=True)
    image = run / 'sd.img'
    image.unlink(missing_ok=True)
    subprocess.run(['/bin/cp', '-c', str(entry / 'card.img'), str(image)], check=True)
    copy_files(image, json.loads((entry / 'layout.json').read_text()), files)
    return image
