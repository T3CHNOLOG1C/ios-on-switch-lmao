#!/usr/bin/env python3
"""Compile the new pinned watchdog adapter; inspect only its new object ABI."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

TREE = Path(__file__).resolve().parents[1]
OUT = TREE / 'build/native-ioswitch-watchdog'
OUT.mkdir(parents=True, exist_ok=True)
COMPONENT = 'NativeIOSwitchWatchdog'
source = TREE / 'drivers/NativeIOSwitchWatchdog/NativeIOSwitchWatchdog.cpp'
header = source.with_name('OriginalIOWatchdog.h')
objpath = OUT / (COMPONENT + '.o')
old_command = json.loads((TREE / 'build/native-ioswitch-root/compile-command.json').read_text())
command = [str(source) if arg.endswith('/drivers/NativeIOSwitchBlock/NativeIOSwitchBlock.cpp')
    else str(objpath) if arg.endswith('/native-ioswitch-root/NativeIOSwitchBlockDevice.o')
    else arg for arg in old_command]
command.extend(['-Xclang', '-fdump-record-layouts', '-Xclang', '-fdump-vtable-layouts'])
recipe = json.loads((TREE / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
(OUT / 'compile.log').write_text(result.stdout + result.stderr)
if result.returncode:
    raise SystemExit(result.stderr[-8000:])

sys.path.insert(0, str(TREE / 'kits/NativePrelink'))
import native_prelink as np
obj = objpath.read_bytes()
image = np.macho(obj)
symcmd = next(at for cmd, at, size in image['commands'] if cmd == 2)
sy, count, st, _ = struct.unpack_from('<4I', obj, symcmd + 8)
symbols = []
for i in range(count):
    ix, kind, _, _, value = struct.unpack_from('<IBBHQ', obj, sy + i * 16)
    symbols.append((obj[st + ix:obj.index(b'\0', st + ix)].decode(), value, kind))
mangled = str(len(COMPONENT)) + COMPONENT
vtable = next(value for name, value, kind in symbols if name == '__ZTV' + mangled)
relocations = {}
for segment in image['segments']:
    for section in segment['sections']:
        if section['va'] <= vtable < section['va'] + section['size']:
            ro, rc = struct.unpack_from('<II', obj, section['command_offset'] + 56)
            for i in range(rc):
                address, word = struct.unpack_from('<II', obj, ro + i * 8)
                if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                    relocations[section['va'] + address] = symbols[word & 0xffffff][0]
compiled = [relocations.get(vtable + 16 + i * 8) for i in range(171)]
if not all(compiled) or vtable + 16 + 171 * 8 in relocations or '___cxa_pure_virtual' in compiled:
    raise SystemExit('Concrete watchdog must have exactly 171 populated non-pure slots')
audit = json.loads((TREE.parents[2] / 'research-native/watchdog-provider-vtable.json').read_text())
expected_own = {0, 1, 7, 14, 86, 87, 168, 169, 170}
rows = []
for index, actual in enumerate(compiled):
    if index in expected_own:
        valid = actual.startswith(('__ZN' + mangled, '__ZNK' + mangled))
    elif index == 140:
        valid = actual == '__ZN10IOWatchdog13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient'
    else:
        valid = actual in audit['entries'][index]['IOService_names']
    if not valid:
        raise SystemExit(f'Watchdog ABI mismatch slot {index}: {actual}')
    rows.append({'slot': index, 'compiled_symbol': actual})
for slot, suffix in ((168, '14enableWatchdogEv'), (169, '15disableWatchdogEv'), (170, '11petWatchdogEv')):
    if compiled[slot] != '__ZN' + mangled + suffix:
        raise SystemExit(f'Hardware hook order mismatch {slot}')

slide = audit['candidate_slide']
assert slide == -0x1000000
possible = {
    '__ZN10IOWatchdog10gMetaClassE': 0xfffffff007b2d268,
    '__ZN10IOWatchdogC2EPK11OSMetaClass': 0xfffffff00663e400,
    '__ZN10IOWatchdogD2Ev': 0xfffffff00663e420,
    '__ZN10IOWatchdog4stopEP9IOService': 0xfffffff00663eb6c,
    '__ZN10IOWatchdog13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient': 0xfffffff00663ee14,
    '__ZTV10IOWatchdog': 0xfffffff006f247d8,
    '_ioswitch_original_watchdog_start': 0xfffffff00663e498,
    '_ioswitch_original_watchdog_check': 0xfffffff00663ebd4,
}
undefined = [name for name, _, kind in symbols if kind == 1 and
    ('10IOWatchdog' in name or name.startswith('_ioswitch_original_watchdog_'))]
if any(name not in possible for name in undefined):
    raise SystemExit('Unexpected original watchdog import: ' + str(undefined))
if any(name.endswith(('D0Ev', 'D1Ev')) for name in undefined):
    raise SystemExit('Abstract base complete/deleting destructors must not be called')
bindings = {name: possible[name] + slide for name in undefined}
assembly = OUT / 'original-bindings.S'
assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n' for name, value in sorted(bindings.items())))
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
    '-Wno-incompatible-sysroot', '-c', assembly, '-o', OUT / 'original-bindings.o'], check=True)
late = OUT / 'NativeIOSwitchWatchdog.late.o'
subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-objcopy',
    '--remove-section=__DATA,__mod_init_func',
    '--globalize-symbol=__GLOBAL__sub_I_NativeIOSwitchWatchdog.cpp', objpath, late], check=True)
late_blob = late.read_bytes()
late_image = np.macho(late_blob)
if any(s['name'] == '__mod_init_func' for seg in late_image['segments'] for s in seg['sections']):
    raise SystemExit('Late watchdog object still has automatic constructors')
def code_bytes(data, parsed):
    section = next(s for seg in parsed['segments'] for s in seg['sections'] if s['name'] == '__text')
    return data[section['fileoff']:section['fileoff'] + section['size']]
if code_bytes(obj, image) != code_bytes(late_blob, late_image):
    raise SystemExit('Late initialization transform changed instructions')
archive = OUT / 'libNativeIOSwitchWatchdog.a' 
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'original-bindings.o'], check=True)
report = {
    'compiled': True, 'class_size_assertion': 'sizeof(IOWatchdog)==0xf8',
    'virtual_methods_checked': 171, 'methods': rows,
    'bindings': {name: hex(value) for name, value in bindings.items()},
    'candidate_slide': slide, 'original_userclient': True,
    'late_initializer': True, 'original_module_load_synchronous': True,
    'original_code_modified': False, 'physical_execution_verified': False,
    'unchanged_input_hashes_checked': False,
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'header_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
    'object_sha256': hashlib.sha256(obj).hexdigest(),
    'late_object_sha256': hashlib.sha256(late_blob).hexdigest(),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
}
(OUT / 'bindings.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({k: report[k] for k in ('compiled', 'virtual_methods_checked', 'bindings', 'archive_sha256')}, indent=2))
