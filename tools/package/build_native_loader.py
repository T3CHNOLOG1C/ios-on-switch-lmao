#!/usr/bin/env python3
"""The kernel window the native loader copies into place, and the loader's generated headers.

    build_native_loader.py --kernel K --adt ADT --trustcache BUNDLE --data-seed SEED --boot-args F --lz4 LZ4 \
        --layout-h OUT --runs-h OUT --window OUT --constants OUT

One step of //boot:package (boot/defs.bzl): lays the kernel, the ADT, the trust cache bundle and the Data seed out
at their physical addresses (native_xnu_layout.build_layout, the full-RAM root-boot profile), checks the result
against every contract the loader relies on, packs it (nonzero extents only), LZ4-compresses each 4 MiB block
(lz4 -12, blocks in parallel, each checked by decoding it again) and writes:

  layout.h        the loader's LOAD_* constants (native/loader/loader.c, copy.c, entry.S, copy.S include it)
  runs.h          copy.c's table of the staged window's blocks
  window          the staged window: the blocks back to back, padded to 4 KiB (bl33.py appends it to U-Boot)
  constants.json  the same constants for the steps after this one (bl33.py, boot_script.py, loader_link.py)

The loader itself is compiled by Bazel (boot/defs.bzl), one action per source.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from loader_abi import (
    BL33_BASE,
    LZ4_BLOCK,
    LZ4_MAGIC,
    NATIVE_FULL_RAM_EXCLUSIONS,
    PACK_MIN_GAP,
    PACK_PAGE,
    PHYSICAL_BASE,
    SD_REFERENCE,
    SD_REFERENCE_BYTES,
    SEED_BYTES,
    SEED_PROPERTY,
    SEED_SOURCE_BYTES,
    SEED_SOURCE_PROPERTY,
    STAGE_END,
    STAGE_HEADER,
    STAGE_SOURCE,
    STAGE_WORDS,
    STAMPS_BYTES,
    STAMPS_PROPERTY,
    VIDEO,
    WINDOW_SIZE,
)
from native_oled_adt import ROOT_MATCHING
from native_xnu_layout import build_layout, parse_adt, validate_data_seed, validate_trustcache

LIMIT = 128 << 20


def pack_window(payload: bytes) -> tuple[list[tuple[int, int]], bytes]:
    """The payload's nonzero extents as (offset, bytes) and their bytes back to back; checked by expanding them again."""
    zero, runs, offset = bytes(PACK_PAGE), [], 0
    while offset < len(payload):
        if payload[offset : offset + PACK_PAGE] == zero[: len(payload) - offset]:
            offset += PACK_PAGE
            continue
        start = offset
        while offset < len(payload) and payload[offset : offset + PACK_PAGE] != zero[: len(payload) - offset]:
            offset += PACK_PAGE
        end = min(offset, len(payload))
        if runs and start - (runs[-1][0] + runs[-1][1]) < PACK_MIN_GAP:
            runs[-1] = (runs[-1][0], end - runs[-1][0])
        else:
            runs.append((start, end - start))
    packed = b''.join(payload[start : start + size] for start, size in runs)
    expanded, at = bytearray(len(payload)), 0
    for start, size in runs:
        expanded[start : start + size] = packed[at : at + size]
        at += size
    if expanded != payload:
        raise ValueError('packed kernel window does not expand to the layout image')
    return runs, packed


def lz4_frame_blocks(frame: bytes, length: int) -> list[tuple[int, bool, bytes]]:
    """An LZ4 frame (lz4.org frame format) of `length` bytes as its blocks (decoded length, stored, block bytes). Only
    what the loader decodes: independent blocks without checksums or dictionary."""
    if frame[:4] != LZ4_MAGIC or len(frame) < 7:
        raise ValueError('lz4: not an LZ4 frame')
    flags, descriptor = frame[4], frame[5]
    if flags >> 6 != 1 or not flags >> 5 & 1 or flags >> 4 & 1 or flags & 1 or not 4 <= descriptor >> 4 & 7:
        raise ValueError(f'lz4: unsupported frame flags {flags:#x}/{descriptor:#x}')
    block_max = 1 << (8 + 2 * (descriptor >> 4 & 7))
    at, left, blocks = 6 + (8 if flags >> 3 & 1 else 0) + 1, length, []
    while True:
        size = struct.unpack_from('<I', frame, at)[0]
        at += 4
        if not size:
            break
        stored, size = bool(size >> 31), size & 0x7FFFFFFF
        if not size or not left or at + size > len(frame):
            raise ValueError('lz4: truncated frame')
        blocks.append((min(block_max, left), stored, frame[at : at + size]))
        left -= blocks[-1][0]
        at += size
    if left or at + (4 if flags >> 2 & 1 else 0) != len(frame):
        raise ValueError('lz4: frame does not hold exactly the extent')
    return blocks


def compress_window(
    payload: bytes, runs: list[tuple[int, int]], lz4: str | None = None
) -> tuple[list[tuple[int, int, int, int]], bytes]:
    """The window's nonzero extents, each LZ4-compressed (lz4 -12, independent blocks of up to 4 MiB): the loader's block
    table (window offset, length, offset in the stream, stream bytes | 1 << 63 if stored) and the blocks back to back.

    Independent blocks make each 4 MiB block of an extent its own compression, so every block is compressed by its
    own lz4 (in parallel) to the bytes one lz4 over the whole extent writes for it, and checked by decoding it with lz4
    again. The table is checked by laying the blocks back into an empty window."""
    lz4 = lz4 or shutil.which('lz4')
    if not lz4:
        raise ValueError('missing build tool: lz4')
    blocks = [(at, min(LZ4_BLOCK, start + size - at)) for start, size in runs for at in range(start, start + size, LZ4_BLOCK)]

    def compress(block):
        at, length = block
        data = payload[at : at + length]
        frame = subprocess.run(
            [lz4, '-12', '-B7', '--no-frame-crc', '-c', '-q'], input=data, capture_output=True, check=True
        ).stdout
        (decoded_length, stored, body), *more = lz4_frame_blocks(frame, length)
        if more or decoded_length != length:
            raise ValueError('lz4: a 4 MiB block did not compress to one block')
        if subprocess.run([lz4, '-d', '-c', '-q'], input=frame, capture_output=True, check=True).stdout != data:
            raise ValueError('lz4: a block does not decode to the window')
        return stored, body

    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        compressed = list(pool.map(compress, blocks))
    table, stream = [], bytearray()
    expanded = bytearray(len(payload))
    for (at, length), (stored, body) in zip(blocks, compressed, strict=True):
        if (at | length) % PACK_PAGE:
            raise ValueError('lz4: block not page-granular')
        table.append((at, length, len(stream), len(body) | (1 << 63 if stored else 0)))
        expanded[at : at + length] = payload[at : at + length]
        stream += body
    if expanded != payload:
        raise ValueError('compressed kernel window does not decode to the layout image')
    return table, bytes(stream)


def emc_khz(command_line: str) -> int:
    """The EMC rate the kernel's SwitchPMIC sets for this command line (switch-emc=1: 1600 MHz, switch-emc=<kHz>), which
    the loader then sets before its copy (native/loader/emc.h); 0 when the kernel leaves EMC alone."""
    values = [v.split('=', 1)[1] for v in command_line.split() if v.startswith('switch-emc=')]
    if not values or values[-1] in ('0', ''):
        return 0
    khz = 1600000 if values[-1] == '1' else int(values[-1], 0)
    if not 204000 < khz <= 3200000:
        raise ValueError(f'switch-emc={values[-1]}: not an EMC rate in kHz')
    return khz


def stage_id(staged: bytes) -> int:
    """The staged window's id (native/loader/stage.h): its SHA-256's first 8 bytes, little-endian."""
    return int.from_bytes(hashlib.sha256(staged).digest()[:8], 'little')


def stage_pages(staged: bytes, stream_bytes: int) -> tuple[bytes, bytes]:
    """The header and trailer pages around the staged window (native/loader/stage.h)."""
    ident = stage_id(staged)
    words = STAGE_WORDS
    header = struct.pack(
        '<8I',
        words['magic'],
        words['head'],
        words['version'],
        STAGE_SOURCE - STAGE_HEADER,
        stream_bytes,
        len(staged),
        ident & 0xFFFFFFFF,
        ident >> 32,
    )
    trailer = struct.pack(
        '<6I', words['magic'], words['tail'], stream_bytes, len(staged), ident & 0xFFFFFFFF, ident >> 32
    )
    return header.ljust(PACK_PAGE, b'\0'), trailer.ljust(PACK_PAGE, b'\0')


def stage_into_bl33(bl33: bytes, staged: bytes, stream_bytes: int) -> bytes:
    """bl33.bin with the staged kernel window after U-Boot (native/loader/stage.h), checked by check_stage."""
    if len(staged) % PACK_PAGE or not stream_bytes <= len(staged) < stream_bytes + PACK_PAGE:
        raise ValueError('staged window must be its stream padded to 4 KiB')
    header, trailer = stage_pages(staged, stream_bytes)
    image = bl33.ljust(STAGE_HEADER - BL33_BASE, b'\0') + header + staged + trailer
    check_stage(image, bl33, staged, stream_bytes)
    return image


def check_stage(image: bytes, bl33: bytes, staged: bytes, stream_bytes: int) -> None:
    """The composed bl33.bin as hekate puts it at 0xAA000000: U-Boot untouched and wholly below the header, including
    its pre-relocation BSS (U-Boot's arm64 start.S header: _end_ofs at 0x10, _bss_end_ofs at 0x20); header, window
    and trailer where boot.cmd and the loader look; everything below hekate's ramoops (0xB0000000)."""
    slot = STAGE_HEADER - BL33_BASE
    end_ofs, bss_end_ofs = struct.unpack_from('<Q', bl33, 0x10)[0], struct.unpack_from('<Q', bl33, 0x20)[0]
    if image[: len(bl33)] != bl33 or any(image[len(bl33) : slot]):
        raise ValueError('bl33: U-Boot changed or the padding is not zero')
    if not 0 < end_ofs <= len(bl33) <= slot or not 0 < bss_end_ofs <= slot:
        raise ValueError(f'bl33: U-Boot (_end {end_ofs:#x}, BSS end {bss_end_ofs:#x}) reaches the staged window')
    header, trailer = stage_pages(staged, stream_bytes)
    source = STAGE_SOURCE - BL33_BASE
    if (
        image[slot:source] != header
        or image[source : source + len(staged)] != staged
        or image[source + len(staged) :] != trailer
    ):
        raise ValueError('bl33: staged window not where the loader reads it')
    if BL33_BASE + len(image) > STAGE_END:
        raise ValueError('bl33: staged window reaches ramoops at 0xB0000000')


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def window(m: dict, payload: bytes, adt: bytes, lz4: str | None = None) -> tuple[dict, list, bytes]:
    """The checks the cacheable-copy root-boot loader needs of a layout (`m`, its manifest; `payload`, its image; `adt`,
    the ADT it was laid out with), then the staged window: (constants, block table, staged bytes)."""
    _require(m['schema_version'] == 1 and m['profile'] == 'native-xnu-host-layout', 'unsupported host layout')
    _require(
        m.get('native_window_ownership_verified') is False, 'this builder requires an explicitly unverified ownership status'
    )
    _require(m['image']['file'] == 'kernel-window.bin', 'unexpected image filename')
    _require(
        len(payload) == m['used_extent'] == m['image']['size_bytes']
        and hashlib.sha256(payload).hexdigest() == m['image']['sha256'],
        'layout image identity mismatch',
    )
    _require(0 < len(payload) <= LIMIT and not len(payload) % 8, 'image exceeds aligned bounded staging region')
    _require(m['phys_base'] == PHYSICAL_BASE and m['owned_window_size'] == WINDOW_SIZE, 'not the 768 MiB root-boot layout')
    for key in (
        'phys_base',
        'virt_base',
        'owned_window_size',
        'entry_pa',
        'entry_va',
        'boot_args_pa',
        'boot_args_size',
        'adt_pa',
        'adt_va',
        'adt_size',
        'top_of_kernel_data',
        'used_end',
        'window_end',
    ):
        _require(type(m[key]) is int and 0 <= m[key] < 1 << 64, 'invalid unsigned layout field: ' + key)
    _require(
        m['top_of_kernel_data'] == m['phys_base'] + len(payload)
        and m['used_end'] == m['top_of_kernel_data']
        and m['window_end'] == m['phys_base'] + m['owned_window_size']
        and m['adt_va'] == m['virt_base'] + m['adt_pa'] - m['phys_base']
        and m['entry_va'] == m['virt_base'] + m['entry_pa'] - m['phys_base']
        and m['virt_base'] + m['owned_window_size'] < 1 << 64
        and m['phys_base'] % 0x200000 == m['virt_base'] % 0x200000
        and not m['boot_args_pa'] % 0x4000
        and m['boot_args_size'] == 0x480
        and m['adt_pa'] == m['boot_args_pa'] + 0x1000
        and 0 < m['adt_size']
        and m['adt_pa'] + m['adt_size'] <= m['used_end']
        and m['boot_args_pa'] + 0x480 <= m['used_end']
        and not m['entry_pa'] % 4
        and len(payload) <= m['owned_window_size'] - 0x200000,
        'inconsistent fixed-layout relationships',
    )
    _require(STAGE_SOURCE + len(payload) <= m['phys_base'], 'staging intersects destination')
    for key in ('entry_pa', 'boot_args_pa', 'adt_pa'):
        _require(m['phys_base'] <= m[key] < m['phys_base'] + len(payload), 'layout address outside used extent: ' + key)
    _require(m.get('full_ram') is True, 'the root-boot loader takes the full-RAM layout')

    # The same-boot SD reference, the RAM ledger, the boot timing record and the per-boot seed: zeroed ADT slots the
    # loader fills, at the offsets layout.h gives it.
    sidecars = ('NativeDataSeed', 'TrustCache')
    _require(not set(sidecars) - m['sources'].keys(), 'the root boot needs the Data seed and trust cache sidecars')
    _require(not any(k in m['sources'] for k in ('RAMDisk', 'PackedRAMDisk')), 'root_boot excludes a diagnostic RAM root')
    source = m['sources']['adt']
    _require(
        len(adt) == source['size_bytes'] and hashlib.sha256(adt).hexdigest() == source['sha256'],
        'SD reference ADT source changed',
    )
    properties = parse_adt(adt).properties
    ram_offset, ram_bytes = properties.get(('/chosen', 'ioswitch-ram-ledger'), (-1, 0))
    _require(
        ram_bytes == 1584 and adt[ram_offset : ram_offset + ram_bytes] == bytes(ram_bytes),
        'full RAM requires zeroed runtime ledger slot',
    )
    _require(
        (m.get('boot_phys_base'), m.get('boot_virt_base'), m.get('boot_memory_size'), m.get('boot_memory_actual'))
        == (0x80000000, 0xFFFFFFEFD2200000, 0x100000000, 0xFFD00000),
        'full RAM boot bases/span differ from measured bank profile',
    )
    start = m['adt_pa'] - m['phys_base']

    def in_window(offset, size):
        return payload[start + offset : start + offset + size]

    sd_reference_offset, size = properties.get(('/chosen', 'ioswitch-sd-reference'), (-1, 0))
    expected = struct.pack('<QQ', SD_REFERENCE, SD_REFERENCE_BYTES)
    _require(
        size == 16
        and adt[sd_reference_offset : sd_reference_offset + size] == expected
        and in_window(sd_reference_offset, size) == expected,
        'missing or changed fixed SD reference property',
    )
    stamps_offset, stamps_bytes = properties.get(('/chosen', STAMPS_PROPERTY), (0, 0))
    _require(
        not stamps_offset
        or (
            stamps_bytes == STAMPS_BYTES
            and not stamps_offset % 4
            and adt[stamps_offset : stamps_offset + stamps_bytes] == bytes(stamps_bytes)
            and in_window(stamps_offset, stamps_bytes) == bytes(stamps_bytes)
        ),
        'boot timing placeholder must be a zeroed, aligned, exact-size /chosen property',
    )
    seed_offset = 0
    seed_source_offset, seed_source_bytes = properties.get(('/chosen', SEED_SOURCE_PROPERTY), (0, 0))
    if seed_source_offset:
        seed_offset, seed_bytes = properties.get(('/chosen', SEED_PROPERTY), (0, 0))
        _require(
            seed_source_bytes == SEED_SOURCE_BYTES
            and not seed_source_offset % 4
            and adt[seed_source_offset : seed_source_offset + seed_source_bytes] == bytes(seed_source_bytes)
            and in_window(seed_source_offset, seed_source_bytes) == bytes(seed_source_bytes),
            'seed report placeholder must be a zeroed, aligned, exact-size /chosen property',
        )
        _require(
            seed_offset
            and seed_bytes == SEED_BYTES
            and not seed_offset % 4
            and in_window(seed_offset, seed_bytes) == adt[seed_offset : seed_offset + seed_bytes],
            'random-seed must be a 64-byte, aligned /chosen property carried into the window',
        )

    # The root boot: the OS volume by UUID, persistent Data, no RAM root or runtime manifest.
    required = {
        'osenvironment': b'main\0',
        'root-matching': ROOT_MATCHING,
        'crypto-hash-method': b'sha2-384\0',
        'amfi-only-platform-code': struct.pack('<I', 1),
        'ephemeral-storage': struct.pack('<I', 0),  # persistent SD Data
    }
    for key, value in required.items():
        pos, length = properties.get(('/chosen', key), (-1, 0))
        _require(
            length == len(value) and adt[pos : pos + length] == value and in_window(pos, length) == value,
            'root_boot chosen property changed: ' + key,
        )
    _require(
        ('/chosen/manifest-properties', 'name') in properties
        and not any(
            (path, key) != ('/chosen/manifest-properties', 'name')
            for path, key in properties
            if path.startswith('/chosen/manifest-properties')
        )
        and not any(
            key in ('RAMDisk', 'PackedRAMDisk', 'boot-uuid', 'boot-manifest-hash', 'owner-boot-manifest-domain')
            for _, key in properties
        ),
        'root_boot requires empty runtime manifest inputs and no RAM root',
    )
    spans = []
    for key in sidecars:
        pos, length = properties.get(('/chosen/memory-map', key), (-1, 0))
        _require(length == 16, 'root_boot sidecar placeholder missing: ' + key)
        address, size = struct.unpack_from('<QQ', payload, start + pos)
        expected = m['sources'][key]
        _require(
            not address % 0x1000
            and size == expected['size_bytes']
            and 0 < size <= 16 << 20
            and m['adt_pa'] + m['adt_size'] <= address <= m['used_end']
            and size <= m['used_end'] - address,
            'root_boot sidecar bounds changed: ' + key,
        )
        _require(not any(address < end and begin < address + size for begin, end in spans), 'root_boot sidecars overlap')
        spans.append((address, address + size))
        content = payload[address - m['phys_base'] : address - m['phys_base'] + size]
        _require(hashlib.sha256(content).hexdigest() == expected['sha256'], 'root_boot sidecar contents changed: ' + key)
        (validate_data_seed if key == 'NativeDataSeed' else validate_trustcache)(content)

    # The boot-args the loader gates on, and the line in the window's boot_args.
    words = m['command_line'].split()
    _require([v for v in words if v.startswith('ioswitch-sd-test=')] == ['ioswitch-sd-test=1'], 'SD probe requires one explicit boot argument')
    command_at = m['boot_args_pa'] - m['phys_base'] + 0x6C
    encoded = m['command_line'].encode('ascii')
    _require(
        len(encoded) < 1024 and payload[command_at : command_at + 1024] == encoded + bytes(1024 - len(encoded)),
        'SD boot argument manifest does not match the staged boot arguments',
    )
    _require(
        [v for v in words if v.startswith('ioswitch-sd-files=')] == ['ioswitch-sd-files=1'],
        'SD file boot argument requires the card-enabled loader',
    )
    _require(len(payload) <= 0x5000000, 'kernel staging overlaps the firmware exporter')
    _require(
        [v for v in words if v.startswith('ioswitch-root-boot=')] == ['ioswitch-root-boot=1'],
        'root_boot requires its exact explicit boot argument',
    )
    _require(
        not any(v.startswith(('rd=', 'boot-uuid=', 'ioswitch-user-test=', 'ioswitch-block-test=')) for v in words),
        'root_boot cannot use diagnostic PID1 or alternate root selection',
    )

    # The native profile's reviewed envelope.
    _require(m['virt_base'] == 0xFFFFFFF00427C000, 'prelinked native profile requires its exact reviewed virtual base')
    _require(
        m['phys_base'] & ~0x1FFFFF == 0xB2000000 and (m['window_end'] + 0x1FFFFF) & ~0x1FFFFF == 0xE2200000,
        'native ownership envelope changed',
    )
    expected = set(NATIVE_FULL_RAM_EXCLUSIONS)
    actual = {(r['name'], r['start'], r['size']) for r in m['reserved_ranges']}
    _require(actual == expected and len(m['reserved_ranges']) == len(expected), 'native experiment requires the exact reviewed exclusion ledger')
    _require(len(payload) <= 0x6000000 and m['video'] == VIDEO, 'native staging or display contract mismatch')
    _require(not len(payload) % 0x1000, 'native cacheable copy requires a page-aligned payload')

    runs, _ = pack_window(payload)
    blocks, stream = compress_window(payload, runs, lz4)
    # Staged in bl33.bin (stage_into_bl33); the loader maps it in 4 KiB pages.
    staged = stream + bytes(-len(stream) % PACK_PAGE)
    _require(STAGE_SOURCE + len(staged) + PACK_PAGE <= STAGE_END, 'compressed kernel window does not fit before ramoops')
    constants = {
        'SOURCE': STAGE_SOURCE,
        'DEST': m['phys_base'],
        'BYTES': len(payload),
        'PACKED_BYTES': len(staged),
        'STREAM_BYTES': len(stream),
        'STAMPS_ADT_OFFSET': stamps_offset,
        'SEED_ADT_OFFSET': seed_offset,
        'SEED_SOURCE_ADT_OFFSET': seed_source_offset,
        'STAGE_ID': stage_id(staged),
        'EMC_KHZ': emc_khz(m['command_line']),
        'ARGS': m['boot_args_pa'],
        'ENTRY': m['entry_pa'],
        'CRC': 0,
        'VERIFY_KERNEL': 0,
        'VIRT': m['virt_base'],
        'ADT_PA': m['adt_pa'],
        'BOOT_PHYS': m.get('boot_phys_base', m['phys_base']),
        'BOOT_VIRT': m.get('boot_virt_base', m['virt_base']),
        'BOOT_SIZE': m.get('boot_memory_size', m['owned_window_size']),
        'BOOT_ACTUAL': m.get('boot_memory_actual', m['owned_window_size']),
        'FULL_RAM': 1,
        'RAM_ADT_OFFSET': ram_offset,
        'WINDOW': m['owned_window_size'],
        'TOP': m['top_of_kernel_data'],
        'ADT_VA': m['adt_va'],
        'ADT_BYTES': m['adt_size'],
        'NATIVE_GATES': 1,
        'SD_REFERENCE': 1,
        'SD_CARD': 1,
        'SD_REFERENCE_ADT_OFFSET': sd_reference_offset,
        'FAST_COPY': 1,
    }
    return constants, blocks, staged


def layout_header(constants: dict) -> str:
    return ''.join(f'#define LOAD_{k} UINT64_C(0x{v:x})\n' for k, v in constants.items())


def runs_header(blocks: list) -> str:
    return (
        "/* Generated: the kernel window's nonzero extents as LZ4 blocks (window offset, bytes, offset in\n"
        ' * kernel-window.bin, its bytes | LOADER_BLOCK_STORED if not compressed) in order, stored back to back;\n'
        ' * everything else in the window is zero. */\n'
        f'#define LOAD_BLOCK_COUNT {len(blocks)}u\n'
        'static const uint64_t loader_blocks[LOAD_BLOCK_COUNT][4] = {\n'
        + ''.join(
            f'    {{UINT64_C(0x{a:x}), UINT64_C(0x{b:x}), UINT64_C(0x{c:x}), UINT64_C(0x{d:x})}},\n' for a, b, c, d in blocks
        )
        + '};\n'
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('kernel', 'adt', 'trustcache', 'data-seed', 'boot-args', 'lz4', 'layout-h', 'runs-h', 'window', 'constants'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    command_line = args.boot_args.read_text().rstrip('\n')
    with tempfile.TemporaryDirectory() as scratch:
        scratch = Path(scratch)
        # The layout refuses symlinked inputs, and Bazel's sandbox stages every input as one: regular copies.
        inputs = {}
        for name in ('kernel', 'adt', 'trustcache', 'data_seed'):
            inputs[name] = scratch / name
            shutil.copyfile(getattr(args, name), inputs[name])
        m = build_layout(
            inputs['kernel'],
            inputs['adt'],
            scratch / 'layout',
            physical_base=PHYSICAL_BASE,
            window_size=WINDOW_SIZE,
            reserved_ranges=[{'name': n, 'start': s, 'size': z} for n, s, z in NATIVE_FULL_RAM_EXCLUSIONS],
            command_line=command_line,
            trustcache_path=inputs['trustcache'],
            data_seed_path=inputs['data_seed'],
            video=VIDEO,
            full_ram=True,
        )
        payload = (scratch / 'layout/kernel-window.bin').read_bytes()
        constants, blocks, staged = window(m, payload, inputs['adt'].read_bytes(), str(args.lz4.absolute()))
    args.layout_h.write_text(layout_header(constants))
    args.runs_h.write_text(runs_header(blocks))
    args.window.write_bytes(staged)
    args.constants.write_text(json.dumps(constants, indent=1) + '\n')
    print(f'kernel window {len(staged)} bytes in {len(blocks)} blocks, entry {m["entry_pa"]:#x}')


if __name__ == '__main__':
    main()
