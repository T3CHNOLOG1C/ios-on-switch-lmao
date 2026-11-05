#!/usr/bin/env python3
"""TegraVIC's bindings into the IOSurface kext (kernel_driver's abi_checks run it on every build).

Usage: abi_check.py --class iosurface --kernelcache KC --object X.o --late X.o --bindings B.S --output X.checked.o
                    [--write]

TegraVICSurface.cpp calls com.apple.iokit.IOSurface functions by address (the kext exports no symbols in the
kernelcache). Checks, against the kernelcache the prelinker uses:
- each function's original address holds the instructions read when it was identified (first 16 bytes; for the
  plane accessors the shared prologue plus the bounds checks of the plane array at +0x180 and the count at +0x98);
- the bindings file binds exactly these functions at original address + prelink slide (-0x1000000); --write
  regenerates it instead of comparing;
- the object's undefined _tegra_vic_ios_* symbols are all bound, and nothing else of the kext is imported;
- Apple's AppleM2ScalerCSCDriver and IOSurfaceAcceleratorClient override, in the slots TegraVICUserClient.cpp
  replaces in their vtable copies, the methods read for the contract (CONTRACT.md).
Then copies the object to --output, which the archive takes.
"""
import argparse
from pathlib import Path
import shutil
import struct
import sys

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / 'tools/prelink'))  # the prelinker's Mach-O reader
from native_prelink import macho, backed_offset  # noqa: E402

SLIDE = -0x1000000
# (symbol, original address, first 16 instruction bytes, what it is)
FUNCTIONS = [
    ('_tegra_vic_ios_lookup', 0xfffffff005baa72c, 'f657bda9f44f01a9fd7b02a9fd830091',
     'IOSurfaceRoot::lookupSurface(id, task): find_surface, then a try-retain of +0xc'),
    ('_tegra_vic_ios_memory', 0xfffffff005b9ead0, '001840f9c0035fd6fd7bbfa9fd030091', 'ldr x0, [x0, #0x30]: the descriptor'),
    ('_tegra_vic_ios_format', 0xfffffff005b9ebac, '008040b9c0035fd6001040b9c0035fd6', 'ldr w0, [x0, #0x80]: pixel format'),
    ('_tegra_vic_ios_width', 0xfffffff005b9ebbc, '002c40f9c0035fd6003040f9c0035fd6', 'ldr x0, [x0, #0x58]: width'),
    ('_tegra_vic_ios_height', 0xfffffff005b9ebc4, '003040f9c0035fd6003c40f9c0035fd6', 'ldr x0, [x0, #0x60]: height'),
    ('_tegra_vic_ios_bytes_per_row', 0xfffffff005b9ebd4, '003440f9c0035fd600e04079c0035fd6',
     'ldr x0, [x0, #0x68]: bytes per row'),
    ('_tegra_vic_ios_plane_count', 0xfffffff005b9ebf4, '009840b9c0035fd6004840f9c0035fd6',
     'ldr w0, [x0, #0x98]: plane count'),
    ('_tegra_vic_ios_alloc_size', 0xfffffff005b9ebfc, '004840f9c0035fd6004440f9c0035fd6',
     'ldr x0, [x0, #0x90]: allocation size'),
    ('_tegra_vic_ios_protection', 0xfffffff005b9d2fc, '00c441f9c0035fd668fa00b000b945f9',
     'ldr x0, [x0, #0x388]: protection options'),
    ('_tegra_vic_ios_plane_width', 0xfffffff005b9eeb8, 'fd7bbfa9fd03009108c040f9080400b4',
     'getPlaneWidth(plane): plane entry +0x00 (stride 0x38)'),
    ('_tegra_vic_ios_plane_height', 0xfffffff005b9ef84, 'fd7bbfa9fd03009108c040f9e80300b4',
     'getPlaneHeight(plane): +0x08'),
    ('_tegra_vic_ios_plane_offset', 0xfffffff005b9ed10, 'fd7bbfa9fd03009108c040f9e80300b4',
     'getPlaneOffset(plane): +0x18'),
    ('_tegra_vic_ios_plane_bytes_per_row', 0xfffffff005b9f1fc, 'fd7bbfa9fd03009108c040f9e80300b4',
     'getPlaneBytesPerRow(plane): +0x20'),
    ('_tegra_vic_ios_prepare', 0xfffffff005b9fda0, 'ffc301d1f85f03a9f65704a9f44f05a9',
     'IOSurface::prepare(): wire count +0x20c, descriptor prepare'),
    ('_tegra_vic_ios_complete', 0xfffffff005b9ff64, 'ff8301d1f44f04a9fd7b05a9fd430191', 'IOSurface::complete()'),
]
# Each plane accessor's field: the immediate of its `add x8, x8, #field` (offset in the function, instruction).
PLANE_FIELDS = {
    '_tegra_vic_ios_plane_height': (0x80, '08210091'),        # add x8, x8, #0x8
    '_tegra_vic_ios_plane_offset': (0x80, '08610091'),        # add x8, x8, #0x18
    '_tegra_vic_ios_plane_bytes_per_row': (0x80, '08810091'), # add x8, x8, #0x20
}


# Apple's scaler classes, which TegraVICUserClient.cpp instantiates and whose vtable copies it points their slots of
# from: the slot of each IOService/IOUserClient method it replaces must hold Apple's override (inside the kext's text,
# 0xfffffff0060ed000+0x765ec), the address the contract was read at. Address points of the original vtables.
KEXT_TEXT = (0xfffffff0060ed000, 0x765ec)
VTABLES = [
    ('AppleM2ScalerCSCDriver', 0xfffffff006e705f8, {
        140: 0xfffffff0061283e4,  # newUserClient(task, securityID, type, properties, handler)
        86: 0xfffffff006126644,   # start
        87: 0xfffffff006127068,   # stop
        85: 0xfffffff006126524,   # probe
        155: 0xfffffff006124a28,  # setPowerState
        44: 0xfffffff0061251bc,   # setProperties
    }),
    ('IOSurfaceAcceleratorClient', 0xfffffff006e65a38, {
        14: 0xfffffff006114078,   # free
        172: 0xfffffff0061142e8,  # clientClose
        175: 0xfffffff00611434c,  # registerNotificationPort(port, type, UInt32 refCon)
        178: 0xfffffff00611435c,  # clientMemoryForType
        182: 0xfffffff0061142c0,  # getTargetAndMethodForIndex
    }),
]


def require(ok, why):
    if not ok:
        sys.exit(f'abi_check (TegraVIC): {why}')


def undefined(blob):
    image = macho(blob)
    table = next(at for cmd, at, _ in image['commands'] if cmd == 2)
    sym, count, strings, _ = struct.unpack_from('<4I', blob, table + 8)
    out = set()
    for i in range(count):
        ix, kind, _, _, _ = struct.unpack_from('<IBBHQ', blob, sym + i * 16)
        if (kind & 0x0e) == 0 and (kind & 1):
            out.add(blob[strings + ix:blob.index(b'\0', strings + ix)].decode())
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--class', dest='kind', required=True)
    p.add_argument('--kernelcache', type=Path, required=True)
    p.add_argument('--object', type=Path, required=True)
    p.add_argument('--late', type=Path, required=True)
    p.add_argument('--bindings', type=Path, required=True)
    p.add_argument('--output', type=Path)
    p.add_argument('--write', action='store_true')
    args = p.parse_args()
    require(args.kind == 'iosurface', f'unknown class {args.kind}')
    kc = args.kernelcache.read_bytes()
    top = macho(kc)
    for name, address, want, what in FUNCTIONS:
        at = backed_offset(top['segments'], address, 16, len(kc))
        require(kc[at:at + 16].hex() == want, f'{name} ({what}) at {address:#x} is {kc[at:at + 16].hex()}, want {want}')
        if name in PLANE_FIELDS:
            off, insn = PLANE_FIELDS[name]
            at = backed_offset(top['segments'], address + off, 4, len(kc))
            require(kc[at:at + 4].hex() == insn, f'{name}: field instruction at +{off:#x} is {kc[at:at + 4].hex()}')
    for cls, vtable, slots in VTABLES:
        for slot, want in slots.items():
            at = backed_offset(top['segments'], vtable + 8 * slot, 8, len(kc))
            got = struct.unpack_from('<Q', kc, at)[0]
            require(got == want and KEXT_TEXT[0] <= got < KEXT_TEXT[0] + KEXT_TEXT[1],
                    f'{cls} vtable slot {slot} holds {got:#x}, want {want:#x}')
    text = ('// Generated by abi_check.py --write from the 21H461 kernelcache: IOSurface kext (com.apple.iokit.IOSurface) '
            'functions at\n// their original addresses plus the prelink slide (-0x1000000). Do not edit.\n')
    for name, address, _, _ in sorted(FUNCTIONS):
        text += f'.globl {name}\n.set {name}, {address + SLIDE:#x}\n'
    if args.write:
        args.bindings.write_text(text)
    else:
        require(args.bindings.read_text() == text, f'{args.bindings} is stale: rerun abi_check.py --write')
    bound = {name for name, _, _, _ in FUNCTIONS}
    imports = {s for s in undefined(args.object.read_bytes()) if s.startswith('_tegra_vic_ios_')}
    require(imports <= bound, f'unbound IOSurface imports: {sorted(imports - bound)}')
    if args.output:
        shutil.copyfile(args.late, args.output)


if __name__ == '__main__':
    main()
