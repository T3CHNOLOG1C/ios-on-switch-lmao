#!/usr/bin/env python3
"""Read the _MTLDevice ABI the frontend depends on out of one IPSW kind's Metal.framework into kinds/<kind>.json.

The frontend's device is a dynamic subclass of the OS's own _MTLDevice that adds no instance variables, so it checks
the base class's instance size (the class's class_ro_t), and it writes its answers into the base class's limits table
(-[_MTLDevice limits]), at the offset each accessor loads after asking for that table.

usage: extract.py <Metal Mach-O> <kind> <out.json>
       (ipsw dyld extract <dyld_shared_cache> Metal --slide -o <dir>: the image with its symbols and its slide applied)
"""

import itertools
import json
import re
import struct
import subprocess
import sys

CLASS_RO = '__OBJC_CLASS_RO_$__MTLDevice'
ACCESSOR = re.compile(r'^[0-9a-f]+ <-\[_MTLDevice (\w+)\]>:$')
# An accessor asks for the table (a call: [self limits]) and loads its 32-bit field from it.
LOAD = re.compile(r'ldr\s+w0, \[x0(?:, #(0x[0-9a-f]+))?\]$')


def segments(image: bytes) -> list[tuple[int, int, int]]:
    ncmds = struct.unpack_from('<I', image, 16)[0]
    at, found = 32, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', image, at)
        if cmd == 0x19:  # LC_SEGMENT_64
            vm, _vsize, fileoff, filesize = struct.unpack_from('<QQQQ', image, at + 24)
            found.append((vm, fileoff, filesize))
        at += size
    return found


def read(image: bytes, address: int, size: int) -> bytes:
    for vm, fileoff, filesize in segments(image):
        if vm <= address < vm + filesize:
            return image[fileoff + address - vm : fileoff + address - vm + size]
    raise KeyError(hex(address))


def symbol(path: str, name: str) -> int:
    out = subprocess.run(['xcrun', 'nm', path], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    sys.exit(f'extract.py: no {name} in {path}')


def limits(path: str) -> dict[str, int]:
    """Give each _MTLDevice accessor's field offset in the limits table: a call, then that load."""
    disassembly = subprocess.run(
        ['xcrun', 'llvm-objdump', '-d', '--no-show-raw-insn', path], capture_output=True, text=True, check=True
    ).stdout
    fields: dict[str, int] = {}
    selector, body = None, []
    for line in [*disassembly.splitlines(), '0 <end>:']:
        if line.endswith('>:'):
            if selector:
                fields.update(_field(selector, body))
            match = ACCESSOR.match(line)
            selector, body = (match.group(1), []) if match else (None, [])
        elif selector and ':' in line:
            body.append(' '.join(line.split(':', 1)[1].split()))
    return dict(sorted(fields.items(), key=lambda item: item[1]))


def _field(selector: str, body: list[str]) -> dict[str, int]:
    for previous, instruction in itertools.pairwise(body[:7]):
        load = LOAD.match(instruction)
        if load and previous.startswith('bl '):
            return {selector: int(load.group(1) or '0', 16)}
    return {}


def main() -> None:
    path, kind, out = sys.argv[1:4]
    with open(path, 'rb') as f:
        image = f.read()
    _flags, _start, size = struct.unpack('<III', read(image, symbol(path, CLASS_RO), 12))
    table = {
        'kind': kind,
        'MTLDevice': {'instance_size': size, 'limits': limits(path)},
        'found': {
            'instance_size': f'{CLASS_RO} instanceSize',
            'limits': 'each -[_MTLDevice <field>]: a call to -limits, then ldr w0, [x0, #offset]',
        },
    }
    with open(out, 'w') as f:
        json.dump(table, f, indent=2)  # dprint's JSON layout (//:format_test)
        f.write('\n')


if __name__ == '__main__':
    main()
