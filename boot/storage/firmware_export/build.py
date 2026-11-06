#!/usr/bin/env python3
"""Build the pinned-BL33 RAM metadata exporter locally; no install/device I/O."""

import argparse
import hashlib
import json
import re
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[2]
PIN = 'b492a4017580ca11c9cd49075a899a65f62069d3750615217fc10308954c9faa'
LOAD = 0xA7000000


def tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    for p in ('/opt/homebrew/opt/llvm/bin', '/opt/homebrew/opt/lld/bin'):
        if (Path(p) / name).is_file():
            return str(Path(p) / name)
    raise ValueError('missing build tool ' + name)


def _check_output(out: Path) -> None:
    if out.exists() or any(p.is_symlink() for p in (out, *out.parents)):
        raise ValueError('output must be a new nonsymlink directory')
    if not out.is_relative_to(ROOT / 'out'):
        raise ValueError('output must be under workspace out')


def _pinned_firmware() -> bytes:
    firmware = ROOT / 'bazel/pinned/firmware/bl33.bin'
    if firmware.is_symlink():
        raise ValueError('symlink firmware')
    raw = firmware.read_bytes()
    if hashlib.sha256(raw).hexdigest() != PIN:
        raise ValueError('BL33 pin mismatch')
    return raw


def _signatures(raw: bytes) -> list[tuple[str, str]]:
    words = re.findall(r'\{0x([0-9a-f]{8})u,\s*0x([0-9a-f]{8})u\}', (SOURCE / 'signatures.h').read_text())
    if len(words) < 100:
        raise ValueError('missing instruction signatures')
    for off, word in words:
        if struct.unpack_from('<I', raw, int(off, 16))[0] != int(word, 16):
            raise ValueError('instruction signature mismatch')
    return words


def _compile(out: Path) -> list[list[str]]:
    flags = [
        '--target=aarch64-none-elf',
        '-march=armv8-a',
        '-mstrict-align',
        '-mgeneral-regs-only',
        '-ffreestanding',
        '-fno-builtin',
        '-fno-stack-protector',
        '-ffixed-x18',
        '-fno-pic',
        '-O2',
        '-Wall',
        '-Wextra',
        '-Werror',
        '-fstack-usage',
    ]
    commands = []
    for name in ('entry.S', 'export.c', 'native.c'):
        cmd = [
            tool('clang'),
            *flags,
            '-I',
            str(SOURCE),
            '-c',
            str(SOURCE / name),
            '-o',
            str(out / (Path(name).stem + '.o')),
        ]
        commands.append(cmd)
        subprocess.run(cmd, check=True)
    cmd = [
        tool('ld.lld'),
        '-T',
        str(SOURCE / 'link.ld'),
        '-o',
        str(out / 'exporter.elf'),
        str(out / 'entry.o'),
        str(out / 'export.o'),
        str(out / 'native.o'),
    ]
    commands.append(cmd)
    subprocess.run(cmd, check=True)
    cmd = [tool('llvm-objcopy'), '-O', 'binary', str(out / 'exporter.elf'), str(out / 'exporter.bin')]
    commands.append(cmd)
    subprocess.run(cmd, check=True)
    return commands


def _segments(out: Path, data: bytes) -> list[dict]:
    if not 0 < len(data) <= 65536:
        raise ValueError('invalid footprint')
    elf = (out / 'exporter.elf').read_bytes()
    e = struct.unpack_from('<HHIQQQIHHHHHH', elf, 16)
    if e[:4] != (2, 183, 1, LOAD):
        raise ValueError('ELF entry mismatch')
    segments = []
    for i in range(e[9]):
        typ, flags_, _off, va, pa, filesz, memsz, _align = struct.unpack_from('<IIQQQQQQ', elf, e[4] + e[8] * i)
        if typ != 1:
            continue
        if va != pa or not LOAD <= va < LOAD + 65536 or filesz != memsz or memsz > LOAD + 65536 - va:
            raise ValueError('out-of-range or uninitialized ELF allocation')
        segments.append({'address': va, 'size': memsz, 'flags': flags_})
    return segments


def _check_code(out: Path) -> None:
    undefined = subprocess.check_output([tool('llvm-nm'), '--undefined-only', str(out / 'exporter.elf')], text=True)
    if undefined.strip():
        raise ValueError('exporter has undefined imports')
    disasm = subprocess.check_output([tool('llvm-objdump'), '-d', str(out / 'exporter.elf')], text=True)
    (out / 'exporter-disassembly.txt').write_text(disasm)
    instruction_lines = '\n'.join(
        line for line in disasm.splitlines() if re.match(r'\s*[0-9a-f]+:\s+[0-9a-f]{8}\s', line)
    )
    if re.search(r'\b(?:[vqds][0-9]+)\b', instruction_lines):
        raise ValueError('SIMD/FP register emitted')


def _stack(out: Path) -> dict[str, int]:
    stack = {}
    for p in out.glob('*.su'):
        for line in p.read_text().splitlines():
            cols = line.split('\t')
            stack[cols[0].rsplit(':', 1)[-1]] = int(cols[1])
    if sum(stack.values()) >= 8192:
        raise ValueError('stack upper bound too large')
    return stack


def _uimage(data: bytes) -> bytes:
    # Optional standalone uImage wrapper: the root packager chooses how to
    # publish the code through its cache-maintenance sequence.
    header = struct.pack(
        '>7I4B32s',
        0x27051956,
        0,
        0,
        len(data),
        LOAD,
        LOAD,
        zlib.crc32(data),
        17,
        22,
        1,
        0,
        b'Switch MMC RAM Export v1'.ljust(32, b'\0'),
    )
    header = header[:4] + struct.pack('>I', zlib.crc32(header)) + header[8:]
    return header + data


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.absolute()
    _check_output(out)
    raw = _pinned_firmware()
    source_names = ('export.c', 'export.h', 'native.c', 'entry.S', 'link.ld', 'signatures.h', 'build.py')
    source_hashes = {n: hashlib.sha256((SOURCE / n).read_bytes()).hexdigest() for n in source_names}
    words = _signatures(raw)
    out.mkdir(parents=True)
    commands = _compile(out)
    data = (out / 'exporter.bin').read_bytes()
    segments = _segments(out, data)
    _check_code(out)
    stack = _stack(out)
    (out / 'exporter.uimg').write_bytes(_uimage(data))
    result = {
        'profile': 'pinned-bl33-ram-card-metadata-export-v1',
        'bl33_sha256': PIN,
        'entry': LOAD,
        'footprint_bytes': len(data),
        'record_pa': 0xA001F400,
        'record_bytes': 256,
        'segments': segments,
        'stack_frames_bytes': stack,
        'stack_capacity': 16384,
        'instruction_signature_count': len(words),
        'source_sha256': source_hashes,
        'files': {
            n: {'size_bytes': (out / n).stat().st_size, 'sha256': hashlib.sha256((out / n).read_bytes()).hexdigest()}
            for n in ('exporter.elf', 'exporter.bin', 'exporter.uimg')
        },
        'commands': commands,
        'boundary': 'Host build only. No firmware, SD card, or native execution performed.',
    }
    (out / 'manifest.json').write_text(json.dumps(result, indent=2) + '\n')
    print(
        json.dumps(
            {k: result[k] for k in ('profile', 'footprint_bytes', 'instruction_signature_count', 'stack_frames_bytes')},
            indent=2,
        )
    )


if __name__ == '__main__':
    main()
