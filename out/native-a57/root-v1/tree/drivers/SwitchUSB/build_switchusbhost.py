#!/usr/bin/env python3
"""Build libSwitchUSBHost.a: XusbHost + UsbDmaMapper + SwitchXHCI (AppleUSBXHCI subclass) + SwitchPD
(BM92T36/BQ24193) + SwitchDPOut (DPAUX1), next to libSwitchUSB.a (build_switchusb.py), which it needs
(xudc::usb2PadInit/DmaBuffer/cacheFlush, switchxudc_set_host_owns_port).

Usage: build_switchusbhost.py [OUTDIR] [--firmware tegra210b01_xusb_firmware]
       (default OUTDIR: <tree>/build/switch-usb-host)

Checks before anything is archived:
- the original AppleUSBXHCI (21H461 kernelcache): vtable header and its 299 slots, getMetaClass returning
  the pinned gMetaClass, the class size (0xe48) in its OSMetaClass constructor call, the instruction
  bytes of every address bound by name (constructor, destructor, the slots SwitchXHCI calls);
- the compiled SwitchXHCI vtable has exactly 299 slots, no pure virtual, our functions only in OWN, an
  IOService/OSObject entry wherever the original has the kernel's function, and every other slot names
  an AppleUSBXHCI symbol that is then bound to the original entry of that very slot;
- the only original imports are those bindings plus gMetaClass; SwitchXHCI's static initializer is
  staged (it runs from switchxhci_start after AppleUSBXHCI is loaded).
Bound values are original addresses plus the prelink slide (-0x1000000).
"""
import json
from pathlib import Path
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_switchusb as base  # noqa: E402  (macho helpers, kernelcache paths, run/require/symbols_of/compiled_vtable)
from build_switchusb import TREE, KERNELCACHE, SLIDE, LLVM, macho, backed_offset, exports, require, run, symbols_of, compiled_vtable  # noqa: E402

VTABLE = 0xfffffff006dd4d38      # __ZTV12AppleUSBXHCI (entries start at +16)
METACLASS = 0xfffffff007af5468   # AppleUSBXHCI::gMetaClass
METACLASS_INIT_SIZE = 0xfffffff005ebdb20   # mov w3, #0xe48 before OSMetaClass::OSMetaClass(name, super, size)
SLOTS = 299
CLASS_SIZE = 0xe48
# Mach-O headers of the two original kexts the bound code lives in (prelink-info _PrelinkExecutableLoadAddr).
KEXT_HEADERS = {'com.apple.driver.usb.AppleUSBXHCI': 0xfffffff0053a5cc0, 'com.apple.iokit.IOUSBHostFamily': 0xfffffff005383600}
# Our overrides: destructors, getMetaClass, free, start, stop, copyMapper, regRead32, regWrite32, regWrite64.
OWN = {0, 1, 7, 14, 86, 87, 253, 290, 292, 293}
# Bound by name and called by our code: (symbol, slot or None, address, first 16 instruction bytes).
CODE = [
    ('__ZN12AppleUSBXHCIC2EPK11OSMetaClass', None, 0xfffffff005ebdb40),   # AppleUSBHostController C2 + vtable store
    ('__ZN12AppleUSBXHCID2Ev', None, 0xfffffff005ebdb80),                 # b AppleUSBHostController::D2
    ('__ZN12AppleUSBXHCI5startEP9IOService', 86, 0xfffffff005ebdc60),
    ('__ZN12AppleUSBXHCI4stopEP9IOService', 87, None),                    # AppleUSBHostController::stop (from the vtable)
    ('__ZN12AppleUSBXHCI4freeEv', 14, 0xfffffff005ec0808),
    ('__ZN12AppleUSBXHCI9regRead32Eyj', 290, 0xfffffff005ecf41c),         # add x0, x1, w2, uxtw; b ml_io_read32
    ('__ZN12AppleUSBXHCI10regWrite32Eyjj', 292, 0xfffffff005ecf42c),      # ...; b ml_io_write32
    ('__ZN12AppleUSBXHCI10regWrite64Eyjy', 293, 0xfffffff005ecf438),      # ...; b ml_io_write64
    ('__ZN12AppleUSBXHCI10copyMapperEv', 253, 0xfffffff005e204f0),         # AppleUSBHostController::copyMapper
]
# First instruction words pinned for the bound code (little-endian hex of 8 bytes).
PINNED = {
    0xfffffff005ebdb40: 'fd7bbfa9fd030091',   # stp x29, x30, [sp, #-16]!; mov x29, sp
    0xfffffff005ecf41c: '2040228b',           # add x0, x1, w2, uxtw
    0xfffffff005ecf42c: '2040228be10303aa',   # add x0, x1, w2, uxtw; mov x1, x3
    0xfffffff005ecf438: '2040228be10303aa',
    0xfffffff005e204f0: '004440f9',           # ldr x0, [x0, #136] (then b IOMapper::copyMapperForDevice)
}
SOURCES = ['XusbHost', 'UsbDmaMapper', 'SwitchXHCI', 'SwitchPD', 'SwitchDPOut']
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier']


def main():
    args = sys.argv[1:]
    firmware = None
    if '--firmware' in args:
        at = args.index('--firmware')
        firmware = Path(args[at + 1]).resolve()
        del args[at:at + 2]
        require(firmware.is_file() and firmware.stat().st_size > 256, f'firmware {firmware} missing or too small')
    out = Path(args[0]).resolve() if args else TREE / 'build/switch-usb-host'
    out.mkdir(parents=True, exist_ok=True)

    kc = KERNELCACHE.read_bytes()
    parent = macho(kc)
    def read(va, n):
        at = backed_offset(parent['segments'], va, n, len(kc))
        return kc[at:at + n]
    def u64(va):
        return struct.unpack('<Q', read(va, 8))[0]
    names = exports(kc, parent)
    require(read(VTABLE, 16) == bytes(16), 'original vtable header')
    original = [u64(VTABLE + 16 + 8 * i) for i in range(SLOTS)]
    require(all(v >> 48 == 0xffff for v in original), 'original vtable has 299 code slots')
    require(u64(VTABLE + 16 + 8 * SLOTS) >> 48 != 0xffff, 'original vtable ends after 299 slots')
    # getMetaClass (slot 7) is adrp/add/ret of gMetaClass; the metaclass constructor passes 0xe48.
    getter = original[7]
    adrp, add, ret = struct.unpack('<3I', read(getter, 12))
    imm = ((adrp >> 29) & 3) | ((adrp >> 5) & 0x7ffff) << 2
    page = (getter & ~0xfff) + ((imm - (1 << 21) if imm & (1 << 20) else imm) << 12)
    require(adrp & 0x9f00001f == 0x90000000 and add & 0xffc003ff == 0x91000000 and ret == 0xd65f03c0 and
            page + ((add >> 10) & 0xfff) == METACLASS, 'original getMetaClass returns the pinned gMetaClass')
    movz = struct.unpack('<I', read(METACLASS_INIT_SIZE, 4))[0]
    require(movz & 0xffe0001f == 0x52800003 and (movz >> 5) & 0xffff == CLASS_SIZE, 'AppleUSBXHCI is 0xe48 bytes')
    bound = {}
    for name, slot, address in CODE:
        if address is None:
            address = original[slot]
        if slot is not None:
            require(original[slot] == address, f'{name} is original slot {slot}')
        if address in PINNED:
            pin = bytes.fromhex(PINNED[address])
            require(read(address, len(pin)) == pin, f'instruction bytes of {name} at {address:#x}')
        bound[name] = address
    # The destructor is a single branch into AppleUSBHostController::~AppleUSBHostController.
    require(struct.unpack('<I', read(bound['__ZN12AppleUSBXHCID2Ev'], 4))[0] >> 26 == 0x05, 'AppleUSBXHCI D2 is a branch')

    # Compile (kernel driver flags + Drivers.cmake warnings), as build_switchusb.py does.
    template = json.loads((TREE / 'build/native-ioswitch-root/compile-command.json').read_text())
    block = str(TREE / 'drivers/NativeIOSwitchBlock/NativeIOSwitchBlock.cpp')
    cwd = json.loads((TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                     .read_text().strip().rstrip(','))['directory']
    objects = {}
    for unit in SOURCES:
        command = list(template)
        command[command.index(block)] = str(HERE / (unit + '.cpp'))
        command[command.index('-o') + 1] = str(out / (unit + '.o'))
        command += WARNINGS + ['-I', str(HERE), '-I', str(TREE / '.cache/xnu-work/osfmk')]
        run(command, cwd=cwd)
        objects[unit] = out / (unit + '.o')
    (out / 'compile-command.json').write_text(json.dumps(command, indent=1) + '\n')

    # SwitchXHCI against the original vtable, slot by slot.
    blob = objects['SwitchXHCI'].read_bytes()
    image, symbols = symbols_of(blob)
    compiled = compiled_vtable(blob, image, symbols, '__ZTV10SwitchXHCI')
    require(len(compiled) == SLOTS, f'SwitchXHCI vtable has {len(compiled)} slots, expected {SLOTS}')
    require('___cxa_pure_virtual' not in compiled, 'pure virtual left in SwitchXHCI')
    for slot, name in enumerate(compiled):
        if slot in OWN:
            require(name.startswith(('__ZN10SwitchXHCI', '__ZNK10SwitchXHCI')), f'slot {slot} should be ours: {name}')
        elif name.startswith(('__ZN12AppleUSBXHCI', '__ZNK12AppleUSBXHCI')):
            require(name not in bound or bound[name] == original[slot], f'slot {slot} binds {name} elsewhere')
            require(not names.get(original[slot]), f'slot {slot}: original is the kernel\'s {names.get(original[slot])}, not an override')
            bound[name] = original[slot]
        else:
            require(name in names.get(original[slot], []), f'slot {slot}: {name} is not the original entry')
    texts = [s for header in KEXT_HEADERS.values()
             for s in macho(kc, backed_offset(parent['segments'], header, 32, len(kc)))['segments'] if s['name'] == '__TEXT_EXEC']
    require(len(texts) == 2, 'AppleUSBXHCI and IOUSBHostFamily text segments')
    for name, address in bound.items():
        require(any(t['va'] <= address < t['va'] + t['size'] for t in texts),
                f'{name} at {address:#x} is outside AppleUSBXHCI/IOUSBHostFamily text')
    data = {'__ZN12AppleUSBXHCI10gMetaClassE': METACLASS, '__ZTV12AppleUSBXHCI': VTABLE}
    allowed = dict(bound) | data
    imports = {name for name, _, kind in symbols if kind == 1 and 'AppleUSBXHCI' in name}
    unknown = imports - set(allowed)
    require(not unknown, 'unaudited original imports: ' + ', '.join(sorted(unknown)))
    require(not any(name.endswith(('D0Ev', 'D1Ev')) for name in imports), 'original D0/D1 must not be called')

    bindings = {name: allowed[name] + SLIDE for name in sorted(imports)}
    assembly = out / 'original-bindings.S'
    assembly.write_text(''.join(f'.globl {n}\n.set {n}, {v:#x}\n' for n, v in bindings.items()))
    run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror', '-Wno-incompatible-sysroot',
         '-c', assembly, '-o', out / 'original-bindings.o'])

    # The XUSB falcon firmware, embedded (or an empty image: switchxhci_start then says so on screen).
    fw = out / 'xusb-firmware.S'
    incbin = f'  .incbin "{firmware}"\n' if firmware else ''
    fw.write_text('  .section __DATA,__const\n  .p2align 8\n  .globl _switch_xusb_firmware\n_switch_xusb_firmware:\n' +
                  incbin + '  .globl _switch_xusb_firmware_end\n_switch_xusb_firmware_end:\n')
    run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror', '-Wno-incompatible-sysroot', '-c', fw, '-o',
         out / 'xusb-firmware.o'])

    # SwitchXHCI registers its metaclass only after AppleUSBXHCI is loaded.
    late = out / 'SwitchXHCI.late.o'
    run([LLVM + 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
         '--globalize-symbol=__GLOBAL__sub_I_SwitchXHCI.cpp', objects['SwitchXHCI'], late])
    late_image = macho(late.read_bytes())
    require(not any(s['name'] == '__mod_init_func' for g in late_image['segments'] for s in g['sections']),
            'SwitchXHCI still has an automatic initializer')

    archive = out / 'libSwitchUSBHost.a'
    archive.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', archive, objects['XusbHost'], objects['UsbDmaMapper'], late, objects['SwitchPD'],
         objects['SwitchDPOut'], out / 'original-bindings.o', out / 'xusb-firmware.o'])
    report = {'archive': str(archive), 'slots_checked': SLOTS, 'own_slots': sorted(OWN), 'firmware': str(firmware),
              'bindings': {n: hex(v) for n, v in bindings.items()}, 'slide': SLIDE}
    (out / 'bindings.json').write_text(json.dumps(report, indent=1) + '\n')
    print(f'libSwitchUSBHost.a: {SLOTS} slots checked, {len(bindings)} original bindings, '
          f'firmware {"embedded" if firmware else "NOT embedded"} -> {archive}')


if __name__ == '__main__':
    main()
