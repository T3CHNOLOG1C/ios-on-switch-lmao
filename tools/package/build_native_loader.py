#!/usr/bin/env python3
"""Build a fixed-layout XNU entry adapter. Does not install or boot."""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_native_handoff import BUILD_FLAGS, ROOT, read_regular, safe_output, tool, validate_elf
from native_xnu_layout import parse_adt, validate_data_seed, validate_trustcache
from native_oled_adt import ROOT_MATCHING

SOURCE = 0xA2000000
LIMIT = 128 << 20
# The prelinked AMFI closure starts at VA ...0427c000; preserve its 2 MiB
# offset at PA b207c000. Both profiles use the same reviewed rounded envelope.
PHYSICAL_BASES = (0xB2004000, 0xB207C000)

NATIVE_EXCLUSIONS = [
    ('original-fdt', 0x80000000, 0x20000),
    ('working-fdt', 0x8D000000, 0x40000),
    ('boot-script', 0x8FE00000, 0x20000),
    ('adapter-stack-vectors', 0xA0000000, 0x20000),
    ('wrapped-adapter', 0xA0100000, 0x20040),
    ('staged-kernel-capacity', 0xA2000000, 0x6000000),
    # hekate loads all of bl33.bin here: U-Boot in the first MiB, then the staged kernel window (native/loader/stage.h).
    ('bl33-and-kernel-window', 0xAA000000, 0x6000000),
    ('ramoops', 0xB0000000, 0x200000),
    ('retained-firmware-display-carveouts', 0xF0000000, 0x10000000),
]

NATIVE_FULL_RAM_EXCLUSIONS = [r for r in NATIVE_EXCLUSIONS if r[0] != 'retained-firmware-display-carveouts'] + [
    ('framebuffer',0xF5A00000,0x400000), ('secure-low-tail',0xFFD00000,0x300000),
]

# The cacheable-copy loader ships the window packed: only its nonzero extents (4 KiB granular; zero gaps shorter than
# 64 KiB stay in an extent), back to back. Over half the window is zeros (the VA holes between prelinked segments), and
# U-Boot reads the file from FAT at ~20 MB/s (48 MHz 4-bit SD); the loader writes the zeros itself during its cached
# copy. Each extent is also LZ4-compressed (lz4 -12, ~2.4:1 on this kernel: ~36 -> ~15 MB, about a second less of
# U-Boot reading); the loader decodes it under the same cached regime (native/loader/copy.c lz4_block).
PACK_PAGE = 0x1000
PACK_MIN_GAP = 0x10000
LZ4_MAGIC = b'\x04\x22\x4d\x18'
# /chosen/ioswitch-boot-stamps: native/loader/stamps.h IOSW_STAMP_COUNT 64-bit words, zero until the loader fills it.
STAMPS_PROPERTY = 'ioswitch-boot-stamps'
STAMPS_BYTES = 26 * 8
STAMPS_SLOT = 0xA001EF00
# /chosen/random-seed (the pinned 64 bytes) and /chosen/ioswitch-seed-source (8 zero bytes): the cacheable-copy loader
# mixes SE2's DRBG output into the first and reports in the second (native/loader/seed.h).
SEED_PROPERTY = 'random-seed'
SEED_BYTES = 64
SEED_SOURCE_PROPERTY = 'ioswitch-seed-source'
SEED_SOURCE_BYTES = 8
# The cacheable-copy loader's window rides in bl33.bin (native/loader/stage.h): hekate reads it with U-Boot at SDR104.
BL33_BASE = 0xAA000000
STAGE_HEADER = 0xAA100000
STAGE_SOURCE = 0xAA101000
STAGE_END = 0xB0000000
STAGE_WORDS = {'magic': 0x57534F49, 'head': 0x4E49574B, 'tail': 0x444E454B, 'version': 1}


def pack_window(payload: bytes) -> tuple[list[tuple[int, int]], bytes]:
    """The payload's nonzero extents as (offset, bytes) and their bytes back to back; checked by expanding them again."""
    zero, runs, offset = bytes(PACK_PAGE), [], 0
    while offset < len(payload):
        if payload[offset:offset + PACK_PAGE] == zero[:len(payload) - offset]:
            offset += PACK_PAGE
            continue
        start = offset
        while offset < len(payload) and payload[offset:offset + PACK_PAGE] != zero[:len(payload) - offset]:
            offset += PACK_PAGE
        end = min(offset, len(payload))
        if runs and start - (runs[-1][0] + runs[-1][1]) < PACK_MIN_GAP:
            runs[-1] = (runs[-1][0], end - runs[-1][0])
        else:
            runs.append((start, end - start))
    packed = b''.join(payload[start:start + size] for start, size in runs)
    expanded, at = bytearray(len(payload)), 0
    for start, size in runs:
        expanded[start:start + size] = packed[at:at + size]
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
        stored, size = bool(size >> 31), size & 0x7fffffff
        if not size or not left or at + size > len(frame):
            raise ValueError('lz4: truncated frame')
        blocks.append((min(block_max, left), stored, frame[at:at + size]))
        left -= blocks[-1][0]
        at += size
    if left or at + (4 if flags >> 2 & 1 else 0) != len(frame):
        raise ValueError('lz4: frame does not hold exactly the extent')
    return blocks


def lz4_decode(block: bytes, length: int) -> bytes:
    """One LZ4 block (lz4.org block format), as the loader decodes it; the builder's own check of the table."""
    out, at, end = bytearray(), 0, len(block)

    def extended(value: int) -> int:
        nonlocal at
        while True:
            more = block[at]
            at += 1
            value += more
            if more != 255:
                return value
    while True:
        token = block[at]
        at += 1
        literal = token >> 4
        if literal == 15:
            literal = extended(literal)
        out += block[at:at + literal]
        at += literal
        if at >= end:
            break
        offset = block[at] | block[at + 1] << 8
        at += 2
        if not offset or offset > len(out):
            raise ValueError('lz4: bad match offset')
        match = token & 15
        match = (extended(match) if match == 15 else match) + 4
        begin = len(out) - offset
        if match <= offset:
            out += out[begin:begin + match]
        else:
            repeats, rest = divmod(match, offset)
            pattern = bytes(out[begin:])
            out += pattern * repeats + pattern[:rest]
    if at != end or len(out) != length:
        raise ValueError('lz4: block does not decode to its length')
    return bytes(out)


def compress_window(payload: bytes, runs: list[tuple[int, int]]) -> tuple[list[tuple[int, int, int, int]], bytes]:
    """The window's nonzero extents, each LZ4-compressed (lz4 -12, independent blocks of up to 4 MiB): the loader's block
    table (window offset, length, offset in the stream, stream bytes | 1 << 63 if stored) and the blocks back to back.
    Checked by decoding the table into the window again."""
    lz4 = tool('lz4')

    def frame(run):
        start, size = run
        return subprocess.run([lz4, '-12', '-B7', '--no-frame-crc', '-c', '-q'], input=payload[start:start + size],
                              capture_output=True, check=True).stdout
    with ThreadPoolExecutor() as pool:
        frames = list(pool.map(frame, runs))
    table, stream = [], bytearray()
    expanded = bytearray(len(payload))
    for (start, size), data in zip(runs, frames):
        at = start
        for length, stored, block in lz4_frame_blocks(data, size):
            if (at | length) % PACK_PAGE:
                raise ValueError('lz4: block not page-granular')
            table.append((at, length, len(stream), len(block) | (1 << 63 if stored else 0)))
            expanded[at:at + length] = block if stored else lz4_decode(block, length)
            stream += block
            at += length
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
    header = struct.pack('<8I', words['magic'], words['head'], words['version'], STAGE_SOURCE - STAGE_HEADER,
                         stream_bytes, len(staged), ident & 0xFFFFFFFF, ident >> 32)
    trailer = struct.pack('<6I', words['magic'], words['tail'], stream_bytes, len(staged), ident & 0xFFFFFFFF,
                          ident >> 32)
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
    if image[:len(bl33)] != bl33 or any(image[len(bl33):slot]):
        raise ValueError('bl33: U-Boot changed or the padding is not zero')
    if not 0 < end_ofs <= len(bl33) <= slot or not 0 < bss_end_ofs <= slot:
        raise ValueError(f'bl33: U-Boot (_end {end_ofs:#x}, BSS end {bss_end_ofs:#x}) reaches the staged window')
    header, trailer = stage_pages(staged, stream_bytes)
    source = STAGE_SOURCE - BL33_BASE
    if (image[slot:source] != header or image[source:source + len(staged)] != staged or
            image[source + len(staged):] != trailer):
        raise ValueError('bl33: staged window not where the loader reads it')
    if BL33_BASE + len(image) > STAGE_END:
        raise ValueError('bl33: staged window reaches ramoops at 0xB0000000')


def build(layout_dir: Path, output: Path, *, native_experiment: bool = False,
          storage_probe: bool = False, storage_files: bool = False,
          root_boot: bool = False, inherited_adt_source: dict | None = None) -> dict:
    output = safe_output(output)
    manifest_bytes = read_regular(layout_dir / 'manifest.json')
    m = json.loads(manifest_bytes)
    if type(storage_probe) is not bool:
        raise ValueError('storage_probe must be a boolean')
    if type(storage_files) is not bool or (storage_files and not storage_probe):
        raise ValueError('storage_files requires a boolean and storage_probe')
    if type(root_boot) is not bool or (root_boot and not storage_files):
        raise ValueError('root_boot requires a boolean and card-enabled storage')
    if m['schema_version'] != 1 or m['profile'] != 'native-xnu-host-layout':
        raise ValueError('unsupported host layout')
    if m.get('native_window_ownership_verified') is not False:
        raise ValueError('this builder requires an explicitly unverified ownership status')
    if m['image']['file'] != 'kernel-window.bin':
        raise ValueError('unexpected image filename')
    payload = read_regular(layout_dir / 'kernel-window.bin')
    if (len(payload) != m['used_extent'] or len(payload) != m['image']['size_bytes'] or
        hashlib.sha256(payload).hexdigest() != m['image']['sha256']):
        raise ValueError('layout image identity mismatch')
    if not 0 < len(payload) <= LIMIT or len(payload) % 8:
        raise ValueError('image exceeds aligned bounded staging region')
    # This first adapter is deliberately tied to one model layout. Do not
    # generalize this into a native allocation claim without a separate review.
    if m['phys_base'] not in PHYSICAL_BASES or m['owned_window_size'] != 768 << 20:
        raise ValueError('only the two explicit 768MiB development layouts are supported')
    for key in ('phys_base', 'virt_base', 'owned_window_size', 'entry_pa', 'entry_va',
                'boot_args_pa', 'boot_args_size', 'adt_pa', 'adt_va', 'adt_size',
                'top_of_kernel_data', 'used_end', 'window_end'):
        if type(m[key]) is not int or not 0 <= m[key] < 1 << 64:
            raise ValueError('invalid unsigned layout field: ' + key)
    if (m['top_of_kernel_data'] != m['phys_base'] + len(payload) or
        m['used_end'] != m['top_of_kernel_data'] or
        m['window_end'] != m['phys_base'] + m['owned_window_size'] or
        m['adt_va'] != m['virt_base'] + m['adt_pa'] - m['phys_base'] or
        m['entry_va'] != m['virt_base'] + m['entry_pa'] - m['phys_base'] or
        m['virt_base'] + m['owned_window_size'] >= 1 << 64 or
        m['phys_base'] % 0x200000 != m['virt_base'] % 0x200000 or
        m['boot_args_pa'] % 0x4000 or m['boot_args_size'] != 0x480 or
        m['adt_pa'] != m['boot_args_pa'] + 0x1000 or
        m['adt_size'] <= 0 or m['adt_pa'] + m['adt_size'] > m['used_end'] or
        m['boot_args_pa'] + 0x480 > m['used_end'] or
        m['entry_pa'] % 4 or len(payload) > m['owned_window_size'] - 0x200000):
        raise ValueError('inconsistent fixed-layout relationships')
    fast_copy = native_experiment and root_boot
    staging_pa = STAGE_SOURCE if fast_copy else SOURCE
    if staging_pa + len(payload) > m['phys_base']:
        raise ValueError('staging intersects destination')
    for key in ('entry_pa', 'boot_args_pa', 'adt_pa'):
        if not m['phys_base'] <= m[key] < m['phys_base'] + len(payload):
            raise ValueError('layout address outside used extent: ' + key)
    full_ram = m.get('full_ram', False)
    if type(full_ram) is not bool:
        raise ValueError('full_ram must be boolean')
    if full_ram and not (native_experiment and root_boot):
        raise ValueError('full RAM requires native root gates')
    ram_offset = 0
    sd_reference_offset = 0
    stamps_offset = 0
    seed_offset = seed_source_offset = 0
    if storage_probe:
        sidecars = ('NativeDataSeed','TrustCache') if root_boot else ('RAMDisk','TrustCache')
        if m['phys_base'] != 0xB207C000 or set(sidecars) - m['sources'].keys():
            raise ValueError('SD probe requires the prelinked userspace profile and both sidecars')
        if root_boot and any(k in m['sources'] for k in ('RAMDisk','PackedRAMDisk')):
            raise ValueError('root_boot excludes a diagnostic RAM root')
        source = m['sources']['adt']
        original_adt = read_regular(Path(source['path']))
        if inherited_adt_source is not None and any(
                source[k] != inherited_adt_source[k] for k in ('path', 'size_bytes', 'sha256')):
            raise ValueError('inherited ADT identity differs from checked prior layout')
        if (len(original_adt) != source['size_bytes'] or
            (inherited_adt_source is None and
             hashlib.sha256(original_adt).hexdigest() != source['sha256'])):
            raise ValueError('SD reference ADT source changed')
        properties = parse_adt(original_adt).properties
        if full_ram:
            ram_offset,ram_bytes=properties.get(('/chosen','ioswitch-ram-ledger'),(-1,0))
            if ram_bytes != 1584 or original_adt[ram_offset:ram_offset+ram_bytes] != bytes(ram_bytes):
                raise ValueError('full RAM requires zeroed runtime ledger slot')
            if (m.get('boot_phys_base'),m.get('boot_virt_base'),m.get('boot_memory_size'),m.get('boot_memory_actual')) != (0x80000000,0xffffffefd2200000,0x100000000,0xffd00000):
                raise ValueError('full RAM boot bases/span differ from measured bank profile')
        offset, size = properties.get(('/chosen','ioswitch-sd-reference'), (-1,0))
        expected = struct.pack('<QQ',0xA001F000,0x1000)
        start = m['adt_pa'] - m['phys_base']
        if size != 16 or original_adt[offset:offset+size] != expected or payload[start+offset:start+offset+size] != expected:
            raise ValueError('missing or changed fixed SD reference property')
        sd_reference_offset = offset
        # Optional pre-kernel timing record (native/loader/stamps.h), filled by the cacheable-copy loader only.
        stamps_offset, stamps_bytes = properties.get(('/chosen', STAMPS_PROPERTY), (0, 0))
        if stamps_offset and (stamps_bytes != STAMPS_BYTES or stamps_offset % 4 or
                              original_adt[stamps_offset:stamps_offset + stamps_bytes] != bytes(stamps_bytes) or
                              payload[start + stamps_offset:start + stamps_offset + stamps_bytes] != bytes(stamps_bytes)):
            raise ValueError('boot timing placeholder must be a zeroed, aligned, exact-size /chosen property')
        # Optional per-boot seed (native/loader/seed.h): only with its zeroed report slot, both 4-byte aligned.
        seed_source_offset, seed_source_bytes = properties.get(('/chosen', SEED_SOURCE_PROPERTY), (0, 0))
        if seed_source_offset:
            seed_offset, seed_bytes = properties.get(('/chosen', SEED_PROPERTY), (0, 0))
            if (seed_source_bytes != SEED_SOURCE_BYTES or seed_source_offset % 4 or
                    original_adt[seed_source_offset:seed_source_offset + seed_source_bytes] != bytes(seed_source_bytes) or
                    payload[start + seed_source_offset:start + seed_source_offset + seed_source_bytes] !=
                    bytes(seed_source_bytes)):
                raise ValueError('seed report placeholder must be a zeroed, aligned, exact-size /chosen property')
            if (not seed_offset or seed_bytes != SEED_BYTES or seed_offset % 4 or
                    payload[start + seed_offset:start + seed_offset + seed_bytes] !=
                    original_adt[seed_offset:seed_offset + seed_bytes]):
                raise ValueError('random-seed must be a 64-byte, aligned /chosen property carried into the window')
        if root_boot:
            required = {'osenvironment':b'main\0', 'root-matching':ROOT_MATCHING,
                        'crypto-hash-method':b'sha2-384\0',
                        'amfi-only-platform-code':struct.pack('<I',1),
                        'ephemeral-storage':struct.pack('<I',0)}  # persistent SD Data
            for key,value in required.items():
                pos,length = properties.get(('/chosen',key),(-1,0))
                if (length != len(value) or original_adt[pos:pos+length] != value or
                    payload[start+pos:start+pos+length] != value):
                    raise ValueError('root_boot chosen property changed: '+key)
            if (('/chosen/manifest-properties','name') not in properties or
                any((path,key) != ('/chosen/manifest-properties','name') for path,key in properties
                    if path.startswith('/chosen/manifest-properties')) or
                any(key in ('RAMDisk','PackedRAMDisk','boot-uuid','boot-manifest-hash','owner-boot-manifest-domain')
                    for _,key in properties)):
                raise ValueError('root_boot requires empty runtime manifest inputs and no RAM root')
            spans=[]
            for key in sidecars:
                pos,length = properties.get(('/chosen/memory-map',key),(-1,0))
                if length != 16:
                    raise ValueError('root_boot sidecar placeholder missing: '+key)
                address,size = struct.unpack_from('<QQ',payload,start+pos)
                expected=m['sources'][key]
                if (address % 0x1000 or size != expected['size_bytes'] or not 0 < size <= 16 << 20 or
                    address < m['adt_pa'] + m['adt_size'] or address > m['used_end'] or size > m['used_end'] - address):
                    raise ValueError('root_boot sidecar bounds changed: '+key)
                if any(address < end and begin < address + size for begin,end in spans):
                    raise ValueError('root_boot sidecars overlap')
                spans.append((address,address+size))
                content=payload[address-m['phys_base']:address-m['phys_base']+size]
                if hashlib.sha256(content).hexdigest() != expected['sha256']:
                    raise ValueError('root_boot sidecar contents changed: '+key)
                (validate_data_seed if key=='NativeDataSeed' else validate_trustcache)(content)
        flags = [v for v in m['command_line'].split() if v.startswith('ioswitch-sd-test=')]
        if flags != ['ioswitch-sd-test=1']:
            raise ValueError('SD probe requires one explicit boot argument')
        command_at = m['boot_args_pa'] - m['phys_base'] + 0x6c
        encoded = m['command_line'].encode('ascii')
        if len(encoded) >= 1024 or payload[command_at:command_at+1024] != encoded+bytes(1024-len(encoded)):
            raise ValueError('SD boot argument manifest does not match the staged boot arguments')
    elif any(v.startswith('ioswitch-sd-test=') for v in m['command_line'].split()):
        raise ValueError('SD boot argument requires the reference-enabled loader')
    file_flags = [v for v in m['command_line'].split() if v.startswith('ioswitch-sd-files=')]
    if file_flags != (['ioswitch-sd-files=1'] if storage_files else []):
        raise ValueError('SD file boot argument requires the card-enabled loader')
    if storage_files and len(payload) > 0x5000000:
        raise ValueError('kernel staging overlaps the firmware exporter')
    root_flags = [v for v in m['command_line'].split() if v.startswith('ioswitch-root-boot=')]
    if root_flags != (['ioswitch-root-boot=1'] if root_boot else []):
        raise ValueError('root_boot requires its exact explicit boot argument')
    if root_boot and any(v.startswith(('rd=','boot-uuid=','ioswitch-user-test=','ioswitch-block-test=')) for v in m['command_line'].split()):
        raise ValueError('root_boot cannot use diagnostic PID1 or alternate root selection')
    if native_experiment:
        if (m['phys_base'] == 0xB207C000 and m['virt_base'] != 0xFFFFFFF00427C000):
            raise ValueError('prelinked native profile requires its exact reviewed virtual base')
        if (m['phys_base'] & ~0x1FFFFF != 0xB2000000 or
            (m['window_end'] + 0x1FFFFF) & ~0x1FFFFF != 0xE2200000):
            raise ValueError('native ownership envelope changed')
        expected = {(name, start, size) for name, start, size in (NATIVE_FULL_RAM_EXCLUSIONS if full_ram else NATIVE_EXCLUSIONS)}
        actual = {(r['name'], r['start'], r['size']) for r in m['reserved_ranges']}
        if actual != expected or len(m['reserved_ranges']) != len(expected):
            raise ValueError('native experiment requires the exact reviewed exclusion ledger')
        if len(payload) > 0x6000000 or m['video'] not in ([0xF5A00000, 1, 2880, 720, 1280, 32], [0xF5A00000, 0, 2880, 720, 1280, 0x320]):
            raise ValueError('native staging or display contract mismatch')
    if fast_copy and len(payload) % 0x1000:
        raise ValueError('native cacheable copy requires a page-aligned payload')
    if stamps_offset and not fast_copy:
        raise ValueError('the boot timing record needs the cacheable-copy loader')
    if seed_offset and not fast_copy:
        raise ValueError('the per-boot seed needs the cacheable-copy loader')
    runs, packed = pack_window(payload) if fast_copy else ([], payload)
    blocks, stream = compress_window(payload, runs) if fast_copy else ([], packed)
    if fast_copy:
        # Staged in bl33.bin (stage_into_bl33); the loader maps it in 4 KiB pages.
        packed = stream + bytes(-len(stream) % PACK_PAGE)
        if STAGE_SOURCE + len(packed) + PACK_PAGE > STAGE_END:
            raise ValueError('compressed kernel window does not fit before ramoops')
    constants = {'SOURCE': staging_pa, 'DEST': m['phys_base'], 'BYTES': len(payload), 'PACKED_BYTES': len(packed),
                 'STREAM_BYTES': len(stream), 'STAMPS_ADT_OFFSET': stamps_offset,
                 'SEED_ADT_OFFSET': seed_offset, 'SEED_SOURCE_ADT_OFFSET': seed_source_offset,
                 'STAGE_ID': stage_id(packed) if fast_copy else 0, 'EMC_KHZ': emc_khz(m['command_line']) if fast_copy else 0,
                 'ARGS': m['boot_args_pa'], 'ENTRY': m['entry_pa'],
                 'CRC': 0 if root_boot else zlib.crc32(payload), 'VERIFY_KERNEL': int(not root_boot),
                 'VIRT': m['virt_base'], 'ADT_PA': m['adt_pa'],
                 'BOOT_PHYS': m.get('boot_phys_base',m['phys_base']),
                 'BOOT_VIRT': m.get('boot_virt_base',m['virt_base']),
                 'BOOT_SIZE': m.get('boot_memory_size',m['owned_window_size']),
                 'BOOT_ACTUAL': m.get('boot_memory_actual',m['owned_window_size']),
                 'FULL_RAM': int(full_ram), 'RAM_ADT_OFFSET': ram_offset,
                 'WINDOW': m['owned_window_size'], 'TOP': m['top_of_kernel_data'],
                 'ADT_VA': m['adt_va'], 'ADT_BYTES': m['adt_size'], 'NATIVE_GATES': int(native_experiment),
                 'SD_REFERENCE': int(storage_probe), 'SD_CARD': int(storage_files),
                 'SD_REFERENCE_ADT_OFFSET': sd_reference_offset, 'FAST_COPY': int(fast_copy)}
    header = ''.join(f'#define LOAD_{k} UINT64_C(0x{v:x})\n' for k, v in constants.items())
    output.mkdir(parents=True)
    (output / 'layout.h').write_text(header)
    if fast_copy:
        # copy.c's table of the staged file's blocks; package_raw_boot.py appends the file to bl33.bin (stage.h).
        (output / 'runs.h').write_text(
            '/* Generated: the kernel window\'s nonzero extents as LZ4 blocks (window offset, bytes, offset in\n'
            ' * kernel-window.bin, its bytes | LOADER_BLOCK_STORED if not compressed) in order, stored back to back;\n'
            ' * everything else in the window is zero. */\n'
            f'#define LOAD_BLOCK_COUNT {len(blocks)}u\n'
            'static const uint64_t loader_blocks[LOAD_BLOCK_COUNT][4] = {\n' +
            ''.join(f'    {{UINT64_C(0x{a:x}), UINT64_C(0x{b:x}), UINT64_C(0x{c:x}), UINT64_C(0x{d:x})}},\n'
                    for a, b, c, d in blocks) + '};\n')
        (output / 'kernel-window.bin').write_bytes(packed)
    commands = []
    sources = [ROOT/'native/loader/entry.S', ROOT/'native/loader/loader.c', ROOT/'native/loader/memory.c', ROOT/'native/handoff/probe.c']
    linker = ROOT / ('native/loader/link.ld' if fast_copy else 'native/handoff/link.ld')
    if fast_copy:
        sources += [ROOT/'native/loader/copy.c', ROOT/'native/loader/copy.S', ROOT/'native/loader/emc.c',
                    ROOT/'native/loader/seed.c']
    if not root_boot:
        sources.append(ROOT/'native/loader/checksum.S')
    if storage_probe:
        sources.append(ROOT/'native/storage/sd_reference.c')
    source_files = [*sources, ROOT/'native/loader/copy.h', ROOT/'native/loader/memory.h', ROOT/'native/loader/ram.h', ROOT/'native/loader/stamps.h', ROOT/'native/loader/stage.h', ROOT/'native/loader/emc.h', ROOT/'native/loader/seed.h', ROOT/'native/storage/sd_reference.h', *(ROOT/'native/handoff').glob('*.h'), ROOT/'native/handoff/entry.S', linker, ROOT/'tools/package/build_native_loader.py', ROOT/'tools/package/package_native_handoff.py']
    if root_boot:
        source_files += [ROOT/'tools/package/native_xnu_layout.py', ROOT/'tools/package/native_oled_adt.py']
    source_hashes = {str(p.relative_to(ROOT)): hashlib.sha256(read_regular(p)).hexdigest() for p in source_files}
    objects = []
    for i, source in enumerate(sources):
        obj = output / f'part-{i}.o'; objects.append(obj)
        # PROBE_QUIET: the shared handoff probe draws nothing and skips its FDT CRC/walk unless it refuses (the loader
        # checks the FDT itself); everything before the copy runs with the caches off.
        commands.append([tool('clang'), *BUILD_FLAGS, '-DPROBE_QUIET=1', '-I', str(output), '-I', str(ROOT/'native/loader'), '-I', str(ROOT/'native/handoff'), '-I', str(ROOT/'native/storage'),
                         '-c', str(source), '-o', str(obj)])
    elf = output / 'loader.elf'
    commands.append([tool('ld.lld'), '-T', str(linker), '-o', str(elf), *map(str, objects)])
    for command in commands:
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
    elf_layout = validate_elf(elf.read_bytes())
    if storage_probe and int(elf_layout['footprint_end_exclusive'], 0) > 0xA001F000:
        raise ValueError('adapter code/data/stacks overlap the fixed SD reference page')
    if stamps_offset and int(elf_layout['footprint_end_exclusive'], 0) > STAMPS_SLOT:
        raise ValueError("adapter code/data/stacks overlap U-Boot's boot timing slot")
    subprocess.run([tool('llvm-objcopy'), '-O', 'binary', str(elf), str(output/'loader.bin')], check=True)
    if storage_probe and (output/'loader.bin').stat().st_size > 0x1F000:
        raise ValueError('adapter file overlaps the fixed SD reference page')
    report = subprocess.check_output([tool('llvm-readelf'), '-h', '-l', '-r', '-s', str(elf)], text=True)
    if 'There are no relocations in this file.' not in report:
        raise ValueError('unresolved adapter relocations')
    (output/'readelf.txt').write_text(report)
    (output/'disassembly.txt').write_text(subprocess.check_output([tool('llvm-objdump'), '-d', str(elf)], text=True))
    if source_hashes != {str(p.relative_to(ROOT)): hashlib.sha256(read_regular(p)).hexdigest() for p in source_files}:
        raise ValueError('source changed during build')
    result = {'schema_version': 1, 'profile': 'native-xnu-entry-experiment' if native_experiment else 'native-xnu-entry-model', 'model_only': not native_experiment,
              'native_window_ownership_verified': False, 'native_xnu_boot_verified': False,
              'root_boot': root_boot,
              'adt_identity_inherited': inherited_adt_source is not None,
              'proof_boundary': ('Conditional native experiment: runtime bank, reservation, carveout and stack gates precede kernel writes. Native XNU execution and complete memory ownership are not yet observed.' if native_experiment else 'Freestanding adapter built for a synthetic RAM/CPU test. No SD package or installation.'),
              'layout': str(layout_dir.resolve()), 'layout_manifest_sha256': hashlib.sha256(manifest_bytes).hexdigest(),
              'payload': m['image'], 'constants': constants, 'elf_layout': elf_layout,
              'cacheable_copy': {'enabled': fast_copy, 'temporary_identity_map': fast_copy,
                                 'mapping_granule_bytes': 4096 if fast_copy else None,
                                 'kernel_entry_requires_original_cache_off_state': True},
              'sources': source_hashes, 'build_commands': commands,
              'files': {name: {'size_bytes': (output/name).stat().st_size,
                              'sha256': hashlib.sha256((output/name).read_bytes()).hexdigest()}
                        for name in ('loader.bin','loader.elf','layout.h') + (('kernel-window.bin',) if fast_copy else ())},
              'packed_runs': [[start, size] for start, size in runs],
              'lz4_blocks': [list(block) for block in blocks],
              'temporary_ranges': [{'name': 'staging', 'start': staging_pa, 'size': len(packed), 'lifetime': 'before XNU entry'},
                                   {'name': 'adapter_and_stacks', 'start': 0xA0000000, 'size': 0x20000, 'lifetime': 'before XNU entry'}]}
    (output/'manifest.json').write_text(json.dumps(result, indent=2)+'\n')
    return result

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--layout', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--native-experiment', action='store_true', help='Enable photographed native MC/stack gates and require the reviewed exclusion ledger')
    p.add_argument('--storage-probe', action='store_true', help='Require and validate the fixed same-boot SD reference page')
    p.add_argument('--storage-files', action='store_true', help='Require the version2 SD card evidence and explicit file-test argument')
    p.add_argument('--root-boot', action='store_true', help='Require the original OS UUID, native Data seed and explicit early-storage root profile')
    a = p.parse_args()
    print(json.dumps(build(a.layout, a.output, native_experiment=a.native_experiment,
                           storage_probe=a.storage_probe, storage_files=a.storage_files,
                           root_boot=a.root_boot), indent=2))
