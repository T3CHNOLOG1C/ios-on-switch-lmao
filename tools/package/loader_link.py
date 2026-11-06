#!/usr/bin/env python3
"""The native loader linked (its objects are compiled by Bazel, one action per source: boot/defs.bzl) and wrapped for
U-Boot's bootm.

    loader_link.py --ld-lld P --objcopy P --readelf P --linker-script LD --constants C --out LOADER.UBOOT OBJ...

Checks the ELF (an AArch64 executable entered at 0xA0000000, every load segment inside the loader's 128 KiB, no
relocations), that the loader's code, data and stacks end below the fixed SD reference page and, when the ADT has the
boot timing record, below U-Boot's stamp slot, and that the flat image fits before the SD reference page. A loader
that does not fit says how big the kernel window's block table (runs.h, copy.c's loader_blocks) has grown: it is the
part that grows with the kernel, 32 bytes a block.
"""

import argparse
import json
import struct
import subprocess
import tempfile
from pathlib import Path

from loader_abi import SD_REFERENCE, STAMPS_SLOT
from uimage import LOAD, legacy_image

MAX_RAW = 128 * 1024


def validate_elf(data: bytes) -> dict:
    if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected little-endian ELF64')
    header = struct.unpack_from('<HHIQQQIHHHHHH', data, 16)
    etype, machine, version, entry, phoff = header[:5]
    phsize, phnum = header[8:10]
    if (etype, machine, version, entry) != (2, 183, 1, LOAD):
        raise ValueError('ELF must be an AArch64 executable with entry 0xA0000000')
    if phsize != 56 or not 1 <= phnum <= 32 or phoff + phsize * phnum > len(data):
        raise ValueError('invalid ELF program header table')
    segments = []
    for index in range(phnum):
        kind, flags, offset, virt, phys, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + index * phsize)
        if kind != 1:
            continue
        if (
            virt != phys
            or not LOAD <= virt < LOAD + MAX_RAW
            or filesz > memsz
            or virt + memsz > LOAD + MAX_RAW
            or offset + filesz > len(data)
        ):
            raise ValueError('ELF load segment escapes the 128KiB loader region')
        segments.append((virt, memsz, flags))
    if not segments or not any(virt == LOAD and flags & 1 for virt, _, flags in segments):
        raise ValueError('ELF entry is not covered by an executable load segment')
    return {'footprint_end': max(virt + memsz for virt, memsz, _ in segments)}


def block_table(readelf: Path, objects: list[Path]) -> str:
    """The kernel window's block table (copy.c's loader_blocks, runs.h) in the objects, as words for a failure."""
    for obj in objects:
        symbols = subprocess.run([readelf, '-sW', obj], capture_output=True, text=True).stdout
        for line in symbols.splitlines():
            fields = line.split()
            if len(fields) >= 8 and fields[-1] == 'loader_blocks':
                size = int(fields[2], 0)
                return f'the kernel window block table (runs.h) is {size // 32} blocks, {size} bytes of .rodata'
    return 'no kernel window block table in the objects'


def too_big(what: str, readelf: Path, objects: list[Path]) -> SystemExit:
    return SystemExit(
        f'{what}; {block_table(readelf, objects)}. The loader must end below 0x{STAMPS_SLOT:x} (stamps.h): fewer, larger '
        'LZ4 blocks (loader_abi.LZ4_BLOCK) or a smaller loader'
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('ld-lld', 'objcopy', 'readelf', 'linker-script', 'constants', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('objects', nargs='+', type=Path)
    args = parser.parse_args()
    constants = json.loads(args.constants.read_text())
    with tempfile.TemporaryDirectory() as scratch:
        elf, flat = Path(scratch) / 'loader.elf', Path(scratch) / 'loader.bin'
        linked = subprocess.run(
            [args.ld_lld, '-T', args.linker_script, '-o', elf, *args.objects], capture_output=True, text=True
        )
        if linked.returncode:
            if 'overlap' in linked.stdout + linked.stderr:  # link.ld's ASSERTs on the image's end
                raise too_big((linked.stdout + linked.stderr).strip(), args.readelf, args.objects)
            raise SystemExit(linked.stdout + linked.stderr)
        footprint = validate_elf(elf.read_bytes())['footprint_end']
        if footprint > SD_REFERENCE:
            raise too_big('loader code/data/stacks overlap the fixed SD reference page', args.readelf, args.objects)
        if constants['STAMPS_ADT_OFFSET'] and footprint > STAMPS_SLOT:
            raise too_big("loader code/data/stacks overlap U-Boot's boot timing slot", args.readelf, args.objects)
        subprocess.run([args.objcopy, '-O', 'binary', elf, flat], check=True)
        if flat.stat().st_size > SD_REFERENCE - LOAD:
            raise too_big('loader file overlaps the fixed SD reference page', args.readelf, args.objects)
        report = subprocess.run([args.readelf, '-h', '-l', '-r', '-s', elf], check=True, capture_output=True, text=True)
        if 'There are no relocations in this file.' not in report.stdout:
            raise SystemExit('unresolved loader relocations')
        image = legacy_image(flat.read_bytes(), os_type=5, kind=2, name='Loader', load=LOAD, entry=LOAD)
        args.out.write_bytes(image)


if __name__ == '__main__':
    main()
