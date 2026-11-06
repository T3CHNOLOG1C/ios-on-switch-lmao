#!/usr/bin/env python3
"""Build a host-only fixed XNU layout from the pinned DarwinBoot contract.

DarwinBoot was UniXNU's EDK2 XNU loader (github.com/UniXNU/DarwinBoot); our copy, whose ParseKernel this mirrors, is
in git history: `git show 27f2155:drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c`.

This writes a new local directory, never a boot entry or storage device. An
explicit window and reservation list are inputs, not proof of native ownership.
Packed root ramdisks, relocations and virtual slides are deliberately unsupported.
NativeDataSeed is a separate bounded sparse input for a writable RAM provider;
it is never installed as the md0 root device by this tool.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import shutil
import stat
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

U64_MAX = (1 << 64) - 1
PAGE = 0x1000
BLOCK = 0x200000
MIN_WINDOW = 0x10000000
MAX_WINDOW = 0x200000000
BOOT_ARGS_SIZE = 0x480


class LayoutError(ValueError):
    """An input does not satisfy the bounded host layout contract."""


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & -boundary


def uint64(value: int, label: str) -> int:
    if type(value) is not int or not 0 <= value <= U64_MAX:
        raise LayoutError(f'{label} must be a uint64')
    return value


def bounded_end(start: int, size: int, label: str) -> int:
    uint64(start, label + ' start')
    uint64(size, label + ' size')
    if size == 0 or start > U64_MAX - size:
        raise LayoutError(f'{label} is empty or overflows uint64')
    return start + size


def valid_window(size: int) -> None:
    uint64(size, 'window size')
    if not MIN_WINDOW <= size <= MAX_WINDOW or size % BLOCK:
        raise LayoutError('window must be 256 MiB..8 GiB in 2 MiB steps')


@dataclass(frozen=True)
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    protection: int


@dataclass(frozen=True)
class Kernel:
    segments: tuple[Segment, ...]
    base: int
    end: int
    entry: int


def parse_kernel(data: bytes) -> Kernel:
    """Mirror DarwinBoot ParseKernel, including its 256 MiB parser bound."""
    if not 32 <= len(data) <= 128 << 20:
        raise LayoutError('kernel must be 32 bytes..128 MiB')
    magic, cpu, subtype, kind, count, size, _, _ = struct.unpack_from('<8I', data)
    if (magic, cpu, subtype, kind) != (0xFEEDFACF, 0x100000C, 0, 2):
        raise LayoutError('expected little-endian ARM64 subtype-0 MH_EXECUTE')
    if size > len(data) - 32:
        raise LayoutError('load-command table exceeds kernel file')
    cursor, end, entry = 32, 32 + size, None
    segments = []
    for _ in range(count):
        if end - cursor < 8:
            raise LayoutError('truncated load command')
        command, length = struct.unpack_from('<II', data, cursor)
        if length < 8 or length % 8 or length > end - cursor:
            raise LayoutError('invalid load-command length')
        if command == 0x19:
            if length < 72 or len(segments) == 64:
                raise LayoutError('invalid or excessive segment commands')
            sections = struct.unpack_from('<I', data, cursor + 64)[0]
            if sections > (length - 72) // 80:
                raise LayoutError('segment sections exceed command')
            address, extent, offset, backed = struct.unpack_from('<4Q', data, cursor + 24)
            protection = struct.unpack_from('<I', data, cursor + 60)[0]
            if extent:
                bounded_end(address, extent, 'virtual segment')
                if extent > MIN_WINDOW or backed > extent or offset > len(data) or backed > len(data) - offset:
                    raise LayoutError('invalid segment memory/file extent')
                raw_name = data[cursor + 8 : cursor + 24].split(b'\0', 1)[0]
                segments.append(
                    Segment(
                        raw_name.decode('ascii', errors='backslashreplace'), address, extent, offset, backed, protection
                    )
                )
        elif command == 5:
            if entry is not None or length != 288 or struct.unpack_from('<II', data, cursor + 8) != (6, 68):
                raise LayoutError('expected exactly one ARM64 LC_UNIXTHREAD state')
            entry = struct.unpack_from('<Q', data, cursor + 272)[0]
        cursor += length
    if cursor != end or entry is None or not segments:
        raise LayoutError('incomplete load-command table, segments or entry')
    ordered = sorted(segments, key=lambda item: item.vmaddr)
    if any(a.vmaddr + a.vmsize > b.vmaddr for a, b in itertools.pairwise(ordered)):
        raise LayoutError('overlapping virtual segments')
    base, last = ordered[0].vmaddr, ordered[-1].vmaddr + ordered[-1].vmsize
    executable = any(s.vmaddr <= entry < s.vmaddr + s.filesize and s.protection & 4 for s in segments)
    if not executable or entry % 4 or base % PAGE or base > U64_MAX - MIN_WINDOW or last - base > MIN_WINDOW - BLOCK:
        raise LayoutError('invalid kernel base, span or executable file-backed entry')
    return Kernel(tuple(segments), base, last, entry)


@dataclass(frozen=True)
class Adt:
    properties: dict[tuple[str, str], tuple[int, int]]
    window_size: int


def parse_adt(data: bytes) -> Adt:
    """Preserve byte offsets, with stricter names than the exporter ADT helper.

    A local reader is needed for in-place sidecar fixups. It also rejects
    unterminated names and duplicate sibling nodes instead of silently picking
    one. All accepted bytes remain unchanged apart from the zero placeholders.
    """
    if not 8 <= len(data) <= 16 << 20:
        raise LayoutError('ADT must be 8 bytes..16 MiB')
    cursor = 0
    offsets = {}

    def read(size: int) -> bytes:
        nonlocal cursor
        if size > len(data) - cursor:
            raise LayoutError('truncated ADT')
        value = data[cursor : cursor + size]
        cursor += size
        return value

    def node(depth: int, parent: str) -> str:
        nonlocal cursor
        if depth > 64:
            raise LayoutError('ADT nesting exceeds 64 levels')
        count, children = struct.unpack('<II', read(8))
        if count > (len(data) - cursor) // 36:
            raise LayoutError('ADT property count exceeds input')
        props = {}
        for _ in range(count):
            raw, length = struct.unpack('<32sI', read(36))
            if b'\0' not in raw:
                raise LayoutError('unterminated ADT property name')
            try:
                key = raw.split(b'\0', 1)[0].decode('ascii')
            except UnicodeError as exc:
                raise LayoutError('non-ASCII ADT property name') from exc
            if not key or key in props:
                raise LayoutError('empty or duplicate ADT property')
            start = cursor
            value = read(length)
            read(-length % 4)
            props[key] = (start, length, value)
        if 'name' not in props:
            raise LayoutError('ADT node has no name')
        raw_name = props['name'][2]
        if not raw_name.endswith(b'\0') or b'\0' in raw_name[:-1]:
            raise LayoutError('unterminated or embedded-NUL ADT node name')
        try:
            name = raw_name[:-1].decode('ascii')
        except UnicodeError as exc:
            raise LayoutError('non-ASCII ADT node name') from exc
        if depth and (not name or '/' in name):
            raise LayoutError('invalid ADT node name')
        path = '/' if depth == 0 else parent.rstrip('/') + '/' + name
        for key, (start, length, _) in props.items():
            offsets[path, key] = (start, length)
        if children > (len(data) - cursor) // 8:
            raise LayoutError('ADT child count exceeds input')
        names = set()
        for _ in range(children):
            child_name = node(depth + 1, path)
            if child_name in names:
                raise LayoutError('duplicate ADT sibling node')
            names.add(child_name)
        return name

    node(0, '')
    if cursor != len(data):
        raise LayoutError('trailing bytes after ADT')
    if ('/chosen', 'name') not in offsets:
        raise LayoutError('ADT requires /chosen')
    window = MIN_WINDOW
    for (path, key), (offset, length) in offsets.items():
        if key == 'darwinboot-memory-size':
            if path != '/chosen' or length != 8:
                raise LayoutError('invalid ADT window property')
            window = struct.unpack_from('<Q', data, offset)[0]
            valid_window(window)
        if key in ('RAMDisk', 'PackedRAMDisk', 'TrustCache', 'NativeDataSeed'):
            if path != '/chosen/memory-map' or length != 16 or data[offset : offset + 16] != bytes(16):
                raise LayoutError('sidecars require zero 16-byte /chosen/memory-map placeholders')
            if key == 'PackedRAMDisk':
                raise LayoutError('PackedRAMDisk is unsupported; supply an uncompressed RAMDisk')
    return Adt(offsets, window)


def validate_trustcache(data: bytes) -> None:
    if len(data) < 8:
        raise LayoutError('truncated trustcache table')
    count = struct.unpack_from('<I', data)[0]
    if not 1 <= count <= 128 or count > (len(data) - 4) // 4:
        raise LayoutError('invalid trustcache count')
    starts = struct.unpack_from(f'<{count}I', data, 4)
    for start, end in zip(starts, (*starts[1:], len(data)), strict=True):
        if start < 4 + count * 4 or start % 4 or end > len(data) or end - start < 24:
            raise LayoutError('invalid trustcache range')
        if struct.unpack_from('<I', data, start)[0] != 2:
            raise LayoutError('trustcache must use version 2')
        entries = struct.unpack_from('<I', data, start + 20)[0]
        if end - start != 24 + entries * 24:
            raise LayoutError('invalid trustcache entries')
        previous = None
        for at in range(start + 24, end, 24):
            digest = data[at : at + 20]
            if not 1 <= data[at + 20] <= 4 or (previous is not None and previous >= digest):
                raise LayoutError('invalid hash type or unsorted trustcache')
            previous = digest


def validate_data_seed(data: bytes) -> None:
    """Check DBRDISK1 geometry without allocating its logical disk size.

    This is format validation, not APFS or seed authentication. The native
    consumer must independently pin the input hash, logical size and UUID.
    """
    if not 32 <= len(data) <= 16 << 20:
        raise LayoutError('NativeDataSeed must be 32 bytes..16 MiB')
    magic, version, block, logical, count = struct.unpack_from('<8sIIQQ', data)
    if (magic, version, block) != (b'DBRDISK1', 1, PAGE):
        raise LayoutError('NativeDataSeed requires DBRDISK1 version 1 with 4 KiB pages')
    if not 0 < logical <= 1 << 30 or logical % PAGE or count > 65536 or count > (len(data) - 32) // 16:
        raise LayoutError('NativeDataSeed logical size or extent count is invalid')
    cursor, previous_end = 32 + count * 16, 0
    for index in range(count):
        at, size = struct.unpack_from('<QQ', data, 32 + index * 16)
        if (
            not size
            or (at | size) % PAGE
            or at < previous_end
            or at > logical
            or size > logical - at
            or size > len(data) - cursor
        ):
            raise LayoutError('NativeDataSeed extent is unaligned, overlapping or out of bounds')
        previous_end = at + size
        cursor += size
    if cursor != len(data):
        raise LayoutError('NativeDataSeed has trailing bytes')


def read_input(path: Path, limit: int) -> bytes:
    try:
        info = path.lstat()
        if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
            raise LayoutError(f'input must be a nonempty regular file <= {limit} bytes: {path}')
        with path.open('rb') as stream:
            data = stream.read(limit + 1)
        if len(data) != info.st_size:
            raise LayoutError(f'input size changed while reading: {path}')
        return data
    except OSError as exc:
        raise LayoutError(f'cannot read input {path}: {exc}') from exc


def source_record(path: Path, data: bytes) -> dict:
    return {'path': str(path.resolve()), 'size_bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def build_layout(
    kernel_path: Path,
    adt_path: Path,
    output_dir: Path,
    *,
    physical_base: int,
    window_size: int,
    reserved_ranges: list[dict],
    command_line: str = '',
    ramdisk_path: Path | None = None,
    trustcache_path: Path | None = None,
    data_seed_path: Path | None = None,
    video: list[int] | None = None,
    full_ram: bool = False,
) -> dict:
    """Validate everything, then create only kernel-window.bin and manifest.json."""
    kernel_path, adt_path, output_dir = map(Path, (kernel_path, adt_path, output_dir))
    if output_dir.exists() or output_dir.is_symlink():
        raise LayoutError('output already exists; choose a new directory')
    if type(full_ram) is not bool:
        raise LayoutError('full_ram must be boolean')
    valid_window(window_size)
    window_end = bounded_end(physical_base, window_size, 'physical window')
    mapped_start, mapped_end = physical_base & -BLOCK, align(window_end, BLOCK)
    if mapped_end > U64_MAX:
        raise LayoutError('block-rounded window overflows uint64')
    if not isinstance(reserved_ranges, list):
        raise LayoutError('reserved_ranges must be an explicit list')
    reservations = []
    seen = set()
    for item in reserved_ranges:
        if not isinstance(item, dict) or set(item) != {'name', 'start', 'size'}:
            raise LayoutError('reservation requires exactly name, start and size')
        name = item['name']
        if not isinstance(name, str) or not name or name in seen:
            raise LayoutError('reservation names must be nonempty and unique')
        seen.add(name)
        end = bounded_end(item['start'], item['size'], 'reservation ' + name)
        if item['start'] < mapped_end and mapped_start < end:
            raise LayoutError(f'block-rounded window overlaps reservation {name}')
        reservations.append(dict(item, end=end))
    image = read_input(kernel_path, 128 << 20)
    tree_bytes = read_input(adt_path, 16 << 20)
    kernel, adt = parse_kernel(image), parse_adt(tree_bytes)
    if physical_base % BLOCK != kernel.base % BLOCK:
        raise LayoutError('physical and virtual bases must be congruent modulo 2 MiB')
    if kernel.base > U64_MAX - window_size:
        raise LayoutError('virtual window overflows uint64')
    if adt.window_size != ((4 << 30) if full_ram else window_size):
        raise LayoutError('explicit window differs from ADT darwinboot-memory-size (default 256 MiB)')
    try:
        command = command_line.encode('ascii')
    except (AttributeError, UnicodeError) as exc:
        raise LayoutError('command line must be ASCII text') from exc
    if len(command) >= 1024 or b'\0' in command:
        raise LayoutError('command line must be at most 1023 bytes with no NUL')
    video = [0] * 6 if video is None else list(video)
    if len(video) != 6:
        raise LayoutError('video must contain six uint64 fields')
    for field in video:
        uint64(field, 'video field')
    video_range = None
    if any(video):
        base, display, row, width, height, depth = video
        native = video[:1] + video[2:] == [0xF5A00000, 2880, 720, 1280, 0x320] and display in (0, 1)
        if (
            not base
            or base % 4
            or (not native and (display != 1 or depth != 32))
            or not width
            or not height
            or row < width * 4
            or row % 4
        ):
            raise LayoutError(
                'video requires 32bpp graphics or the native rotated profile, with valid width/height/stride'
            )
        end = bounded_end(base, row * height, 'framebuffer')
        if base < mapped_end and mapped_start < end:
            raise LayoutError('block-rounded window overlaps framebuffer')
        video_range = {'name': 'framebuffer', 'start': base, 'size': row * height, 'end': end}
    sources = {'kernel': source_record(kernel_path, image), 'adt': source_record(adt_path, tree_bytes)}
    sidecars = {}
    for key, given, limit in (
        ('RAMDisk', ramdisk_path, 256 << 20),
        ('TrustCache', trustcache_path, 16 << 20),
        ('NativeDataSeed', data_seed_path, 16 << 20),
    ):
        placeholder = ('/chosen/memory-map', key) in adt.properties
        if (given is not None) != placeholder:
            raise LayoutError(f'{key} input and ADT placeholder must both be present or absent')
        if given is not None:
            path = Path(given)
            data = read_input(path, limit)
            if key == 'RAMDisk' and data[:8] == b'DBRDISK1':
                raise LayoutError('packed ramdisk input is unsupported')
            if key == 'TrustCache':
                validate_trustcache(data)
            if key == 'NativeDataSeed':
                validate_data_seed(data)
            sidecars[key] = data
            sources[key] = source_record(path, data)
    args_pa = align(physical_base + kernel.end - kernel.base, 0x4000)
    tree_pa = args_pa + PAGE
    ramdisk_pa = align(tree_pa + len(tree_bytes), PAGE)
    ramdisk_size = align(len(sidecars.get('RAMDisk', b'')), PAGE)
    trustcache_pa = ramdisk_pa + ramdisk_size
    trustcache_end = trustcache_pa + len(sidecars.get('TrustCache', b''))
    data_seed_pa = align(trustcache_end, PAGE)
    data_seed_size = len(sidecars.get('NativeDataSeed', b''))
    data_seed_span = align(data_seed_size, PAGE)
    used_end = align(data_seed_pa + data_seed_span if data_seed_size else trustcache_end, 0x4000)
    used = used_end - physical_base
    if used > window_size - BLOCK:
        raise LayoutError('layout exceeds window or leaves less than 2 MiB slack')
    tree = bytearray(tree_bytes)
    for key, address, size in (
        ('RAMDisk', ramdisk_pa, ramdisk_size),
        ('TrustCache', trustcache_pa, len(sidecars.get('TrustCache', b''))),
        ('NativeDataSeed', data_seed_pa, data_seed_size),
    ):
        if key in sidecars:
            struct.pack_into('<QQ', tree, adt.properties['/chosen/memory-map', key][0], address, size)
    boot_phys = 0x80000000 if full_ram else physical_base
    boot_virt = kernel.base - physical_base + boot_phys
    boot_size = (4 << 30) if full_ram else window_size
    boot_actual = ((4 << 30) - (3 << 20)) if full_ram else window_size
    if full_ram and (
        physical_base != 0xB207C000 or adt.properties.get(('/chosen', 'switch-ram-ledger'), (0, 0))[1] != 1584
    ):
        raise LayoutError('full RAM requires the measured native profile and runtime ledger')
    boot_args = bytearray(BOOT_ARGS_SIZE)
    struct.pack_into('<HHI4Q6Q', boot_args, 0, 2, 2, 0, boot_virt, boot_phys, boot_size, used_end, *video)
    struct.pack_into('<QI', boot_args, 0x60, kernel.base + tree_pa - physical_base, len(tree))
    boot_args[0x6C : 0x6C + len(command)] = command
    struct.pack_into('<Q', boot_args, 0x478, boot_actual)
    payload = bytearray(used)
    segments = []
    ranges = []

    def place(name: str, address: int, data: bytes, memory_size: int) -> None:
        offset = address - physical_base
        if offset < 0 or len(data) > memory_size or offset + memory_size > used:
            raise LayoutError('internal layout bounds failure')
        for other in ranges:
            if address < other['end'] and other['start'] < address + memory_size:
                raise LayoutError('internal layout overlap')
        payload[offset : offset + len(data)] = data
        ranges.append(
            {
                'name': name,
                'start': address,
                'size': memory_size,
                'end': address + memory_size,
                'file_bytes': len(data),
                'image_offset': offset,
            }
        )

    for index, segment in enumerate(kernel.segments):
        pa = physical_base + segment.vmaddr - kernel.base
        place(
            f'segment:{index}:{segment.name}',
            pa,
            image[segment.fileoff : segment.fileoff + segment.filesize],
            segment.vmsize,
        )
        segments.append(
            dict(
                asdict(segment),
                physical_address=pa,
                image_offset=pa - physical_base,
                zero_fill_bytes=segment.vmsize - segment.filesize,
            )
        )
    place('boot_args', args_pa, boot_args, PAGE)
    place('adt', tree_pa, tree, len(tree))
    if 'RAMDisk' in sidecars:
        place('ramdisk', ramdisk_pa, sidecars['RAMDisk'], ramdisk_size)
    if 'TrustCache' in sidecars:
        place('trustcache', trustcache_pa, sidecars['TrustCache'], len(sidecars['TrustCache']))
    if 'NativeDataSeed' in sidecars:
        place('native_data_seed', data_seed_pa, sidecars['NativeDataSeed'], data_seed_span)
    manifest = {
        'schema_version': 1,
        'profile': 'native-xnu-host-layout',
        'native_window_ownership_verified': False,
        'full_ram': full_ram,
        'window_role': 'contiguous early image/bootstrap safety envelope' if full_ram else 'managed RAM',
        'boot_phys_base': boot_phys,
        'boot_virt_base': boot_virt,
        'boot_memory_size': boot_size,
        'boot_memory_actual': boot_actual,
        'proof_boundary': (
            'Host layout only; addresses and exclusions do not establish native RAM ownership, launchability or a '
            'kernel boot.'
        ),
        'sources': sources,
        'phys_base': physical_base,
        'virt_base': kernel.base,
        'owned_window_size': window_size,
        'window_end': window_end,
        'entry_pa': physical_base + kernel.entry - kernel.base,
        'entry_va': kernel.entry,
        'boot_args_pa': args_pa,
        'boot_args_size': BOOT_ARGS_SIZE,
        'adt_pa': tree_pa,
        'adt_va': kernel.base + tree_pa - physical_base,
        'adt_size': len(tree),
        'adt_patched_sha256': hashlib.sha256(tree).hexdigest(),
        'used_extent': used,
        'used_end': used_end,
        'top_of_kernel_data': used_end,
        'unused_window_bytes': window_size - used,
        'block_rounded_extent': {'start': mapped_start, 'end': mapped_end, 'size': mapped_end - mapped_start},
        'segments': segments,
        'ranges': ranges,
        'reserved_ranges': reservations,
        'video': video,
        'framebuffer_range': video_range,
        'command_line': command_line,
        'image': {
            'file': 'kernel-window.bin',
            'size_bytes': len(payload),
            'sha256': hashlib.sha256(payload).hexdigest(),
        },
    }
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    created = False
    try:
        output_dir.mkdir()  # Atomic exclusive creation: never replace an existing result.
        created = True
        (output_dir / 'kernel-window.bin').write_bytes(payload)
        (output_dir / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    except OSError as exc:
        if created:
            shutil.rmtree(output_dir)
        raise LayoutError(f'could not publish layout: {exc}') from exc
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', required=True, type=Path)
    parser.add_argument('--adt', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--physical-base', required=True, type=lambda value: int(value, 0))
    parser.add_argument('--window-size', required=True, type=lambda value: int(value, 0))
    parser.add_argument(
        '--reserved-ranges',
        required=True,
        type=Path,
        help='JSON array of {"name": str, "start": int, "size": int}; [] is an explicit model assumption',
    )
    parser.add_argument('--ramdisk', type=Path)
    parser.add_argument('--trustcache', type=Path)
    parser.add_argument(
        '--data-seed', type=Path, help='Sparse DBRDISK1 NativeDataSeed for a separate RAM-backed Data device'
    )
    parser.add_argument('--command-line', default='')
    parser.add_argument(
        '--video',
        nargs=6,
        type=lambda value: int(value, 0),
        metavar=('BASE', 'DISPLAY', 'ROW_BYTES', 'WIDTH', 'HEIGHT', 'DEPTH'),
    )
    args = parser.parse_args()
    try:
        reservations = json.loads(args.reserved_ranges.read_text())
        manifest = build_layout(
            args.kernel,
            args.adt,
            args.output,
            physical_base=args.physical_base,
            window_size=args.window_size,
            reserved_ranges=reservations,
            ramdisk_path=args.ramdisk,
            trustcache_path=args.trustcache,
            data_seed_path=args.data_seed,
            command_line=args.command_line,
            video=args.video,
        )
    except (LayoutError, OSError, ValueError) as exc:
        print(f'native layout refused: {exc}', file=sys.stderr)
        return 1
    print(
        json.dumps(
            {
                'output': str(args.output.resolve()),
                'entry_pa': manifest['entry_pa'],
                'boot_args_pa': manifest['boot_args_pa'],
                'used_extent': manifest['used_extent'],
                'native_window_ownership_verified': False,
            },
            indent=2,
        )
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
