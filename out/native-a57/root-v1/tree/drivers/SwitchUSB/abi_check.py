#!/usr/bin/env python3
"""The ABI of our subclasses of original 21H461 classes (run by cmake/Drivers.cmake on every build).

Usage: abi_check.py --class xudc|xhci --kernelcache KC --object X.o --late X.late.o --bindings B.S --output X.checked.o
                    [--write]

xudc: SwitchXUDC against IOUSBDeviceController (com.apple.iokit.IOUSBDeviceFamily);
xhci: SwitchXHCI against AppleUSBXHCI (com.apple.driver.usb.AppleUSBXHCI, on IOUSBHostFamily's AppleUSBHostController).
Checks, against the kernelcache the prelinker uses:
- the original vtable (slot count), getMetaClass returning the pinned gMetaClass, the instruction bytes at every address
  bound by name;
- the compiled vtable has exactly the original slot count, no pure virtual left, our functions only in OWN, and every
  other slot names the function the original vtable holds there (xhci: inherited AppleUSBXHCI slots are bound to the
  original entry of that very slot);
- the only original imports are the audited ones, all inside the original kexts, and no abstract-base D0/D1;
- the bindings file binds exactly those imports at original address + prelink slide (-0x1000000); --write regenerates it
  instead of comparing;
- the late object (static initializer staged, Drivers.cmake ioswitch_late_object) has the same instructions as the
  object and no __mod_init_func.
Then copies the late object to --output, which the archive takes.
"""
import argparse
from pathlib import Path
import struct
import sys

TREE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TREE / 'kits/NativePrelink'))
from native_prelink import macho, backed_offset, exports  # noqa: E402

SLIDE = -0x1000000
# --- xudc: SwitchXUDC against IOUSBDeviceController ------------------------------------------------------------
VTABLE = 0xfffffff006e81d90        # __ZTV21IOUSBDeviceController (entries start at +16)
METACLASS = 0xfffffff007b1a4c8     # IOUSBDeviceController::gMetaClass
SLOTS = 234
OWN = {0, 1, 7, 14, 86, 87, 111, 175, 177, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189,
       192, 193, 199, 202, 215, 227, 233}
# (symbol, original vtable slot or None, original address, first 16 instruction bytes)
CODE = [
    ('__ZN21IOUSBDeviceController4freeEv', 14, 0xfffffff00617d7ec, 'f44fbea9fd7b01a9fd430091f30300aa'),
    ('__ZN21IOUSBDeviceController13setPropertiesEP8OSObject', 44, 0xfffffff006185610, 'ff4301d1f65702a9f44f03a9fd7b04a9'),
    ('__ZN21IOUSBDeviceController5startEP9IOService', 86, 0xfffffff00617ca68, 'ff0302d1f85f04a9f65705a9f44f06a9'),
    ('__ZN21IOUSBDeviceController4stopEP9IOService', 87, 0xfffffff00617d644, 'f44fbea9fd7b01a9fd430091f30301aa'),
    ('__ZN21IOUSBDeviceController20callPlatformFunctionEPK8OSSymbolbPvS3_S3_S3_', 117, 0xfffffff00617d860, 'fa67bba9f85f01a9f65702a9f44f03a9'),
    ('__ZN21IOUSBDeviceController9opaque168Ev', 168, 0xfffffff00617d928, 'ff8300d1fd7b01a9fd430091e80300aa'),
    ('__ZN21IOUSBDeviceController9opaque169Ev', 169, 0xfffffff00617d9bc, 'e90300aa405880520000bc72c10100b4'),
    ('__ZN21IOUSBDeviceController9opaque170Ev', 170, 0xfffffff00617da8c, 'e90300aa405880520000bc72c10100b4'),
    ('__ZN21IOUSBDeviceController9opaque171Ev', 171, 0xfffffff00617db10, '00f84279c0035fd6f44fbea9fd7b01a9'),
    ('__ZN21IOUSBDeviceController9opaque172Ev', 172, 0xfffffff00617d978, 'f44fbea9fd7b01a9fd430091f30302aa'),
    ('__ZN21IOUSBDeviceController9opaque173Ev', 173, 0xfffffff00617da04, 'f85fbca9f65701a9f44f02a9fd7b03a9'),
    ('__ZN21IOUSBDeviceController9opaque174Ev', 174, 0xfffffff00617dad4, 'f44fbea9fd7b01a9fd430091f30300aa'),
    ('__ZN21IOUSBDeviceController9opaque176Ev', 176, 0xfffffff00617ed40, 'c0035fd6fd7bbfa9fd030091e0cc0090'),
    ('__ZN21IOUSBDeviceController9opaque178Ev', 178, 0xfffffff00617ba9c, 'ffc301d1f85f03a9f65704a9f44f05a9'),
    ('__ZN21IOUSBDeviceController9opaque190Ev', 190, 0xfffffff0061804c4, 'ff0302d1fa6703a9f85f04a9f65705a9'),
    ('__ZN21IOUSBDeviceController9opaque191Ev', 191, 0xfffffff006180674, 'ffc301d1f85f03a9f65704a9f44f05a9'),
    ('__ZN21IOUSBDeviceController9opaque194Ev', 194, 0xfffffff00617de50, '280400511f090071e80000541f8000f9'),
    ('__ZN21IOUSBDeviceController16handleUSBSuspendEv', 195, 0xfffffff006181b08, 'ffc301d1f85f03a9f65704a9f44f05a9'),
    ('__ZN21IOUSBDeviceController14handleUSBResetEv', 196, 0xfffffff006181f90, 'f44fbea9fd7b01a9fd430091f30300aa'),
    ('__ZN21IOUSBDeviceController23handleUSBConnectionDoneEv', 197, 0xfffffff006182518, 'f657bda9f44f01a9fd7b02a9fd830091'),
    ('__ZN21IOUSBDeviceController15handleUSBResumeEv', 198, 0xfffffff006182734, 'f85fbca9f65701a9f44f02a9fd7b03a9'),
    ('__ZN21IOUSBDeviceController9opaque200Ev', 200, 0xfffffff00617e294, 'f657bda9f44f01a9fd7b02a9fd830091'),
    ('__ZN21IOUSBDeviceController9opaque201Ev', 201, 0xfffffff006187fdc, 'c0035fd6f44fbea9fd7b01a9fd430091'),
    ('__ZN21IOUSBDeviceController9opaque203Ev', 203, 0xfffffff006188818, 'e05880520000bc72c0035fd6fd7bbfa9'),
    ('__ZN21IOUSBDeviceController21handleUSBCableConnectEv', 204, 0xfffffff00618285c, '004440f9080040f9067540f901000090'),
    ('__ZN21IOUSBDeviceController9opaque205Ev', 205, 0xfffffff006181f14, '004440f9400100b4080040f9067540f9'),
    ('__ZN21IOUSBDeviceController9opaque206Ev', 206, 0xfffffff006181e4c, 'ff0301d1fd7b03a9fdc30091e80300aa'),
    ('__ZN21IOUSBDeviceController9opaque207Ev', 207, 0xfffffff00617de78, '40008052c0035fd600008052c0035fd6'),
    ('__ZN21IOUSBDeviceController9opaque208Ev', 208, 0xfffffff00617de80, '00008052c0035fd6ff4305d1f85f11a9'),
    ('__ZN21IOUSBDeviceController9opaque209Ev', 209, 0xfffffff00617e434, '00f04539c0035fd6fd7bbfa9fd030091'),
    ('__ZN21IOUSBDeviceController9opaque210Ev', 210, 0xfffffff00617e43c, 'fd7bbfa9fd030091e80300aa40588052'),
    ('__ZN21IOUSBDeviceController9opaque211Ev', 211, 0xfffffff00617e47c, 'f44fbea9fd7b01a9fd430091f30300aa'),
    ('__ZN21IOUSBDeviceController9opaque212Ev', 212, 0xfffffff00617e4b0, 'f657bda9f44f01a9fd7b02a9fd830091'),
    ('__ZN21IOUSBDeviceController9opaque213Ev', 213, 0xfffffff00617de88, 'ff4305d1f85f11a9f65712a9f44f13a9'),
    ('__ZN21IOUSBDeviceController9opaque214Ev', 214, 0xfffffff00617e3f4, 'ffc300d1fd7b02a9fd830091e897ff90'),
    ('__ZN21IOUSBDeviceController31handleControlRequestStateChangeEii', 215, 0xfffffff00617e430, 'c0035fd600f04539c0035fd6fd7bbfa9'),
    ('__ZN21IOUSBDeviceController9opaque216Ev', 216, 0xfffffff006180860, 'f44fbea9fd7b01a9fd430091f30300aa'),
    ('__ZN21IOUSBDeviceController9opaque217Ev', 217, 0xfffffff0061808b0, 'f44fbea9fd7b01a9fd430091f30300aa'),
    ('__ZN21IOUSBDeviceController9opaque218Ev', 218, 0xfffffff006180b24, 'f85fbca9f65701a9f44f02a9fd7b03a9'),
    ('__ZN21IOUSBDeviceController9opaque219Ev', 219, 0xfffffff006180a24, 'f44fbea9fd7b01a9fd430091f30300aa'),
    ('__ZN21IOUSBDeviceController9opaque220Ev', 220, 0xfffffff006180cb0, 'ffc301d1f85f03a9f65704a9f44f05a9'),
    ('__ZN21IOUSBDeviceController9opaque221Ev', 221, 0xfffffff006181428, 'fc6fbaa9fa6701a9f85f02a9f65703a9'),
    ('__ZN21IOUSBDeviceController9opaque222Ev', 222, 0xfffffff006185c9c, 'ffc302d1f85f07a9f65708a9f44f09a9'),
    ('__ZN21IOUSBDeviceController9opaque223Ev', 223, 0xfffffff00617e534, '011080520200805203008052b0dbff17'),
    ('__ZN21IOUSBDeviceController9opaque224Ev', 224, 0xfffffff00617bc84, '084a80520800bc725f00086b08628052'),
    ('__ZN21IOUSBDeviceController9opaque225Ev', 225, 0xfffffff00617e544, 'ffc301d1fa6702a9f85f03a9f65704a9'),
    ('__ZN21IOUSBDeviceController9opaque226Ev', 226, 0xfffffff00617e8b8, 'ffc301d1fa6702a9f85f03a9f65704a9'),
    ('__ZN21IOUSBDeviceController9opaque228Ev', 228, 0xfffffff00617eb10, 'fd7bbfa9fd030091206800b000201d91'),
    ('__ZN21IOUSBDeviceController9opaque229Ev', 229, 0xfffffff00617eb40, '206800b000201e91ba2a0014ff4301d1'),
    ('__ZN21IOUSBDeviceController9opaque230Ev', 230, 0xfffffff00617eb4c, 'ff4301d1f65702a9f44f03a9fd7b04a9'),
    ('__ZN21IOUSBDeviceController9opaque231Ev', 231, 0xfffffff006187cdc, 'f44fbea9fd7b01a9fd430091f30300aa'),
    ('__ZN21IOUSBDeviceController9opaque232Ev', 232, 0xfffffff00618880c, 'e05880520000bc72c0035fd6e0588052'),
    ('__ZN21IOUSBDeviceController16getBufferOptionsERj', 233, 0xfffffff00617ed2c, '3f0000b9e05880520000bc72c0035fd6'),
    # Non-virtual members (AppleUSBXDCI's GOT; names from the symbolized macOS family).
    ('__ZN21IOUSBDeviceControllerC2EPK11OSMetaClass', None, 0xfffffff00617ba04, 'fd7bbfa9fd030091bd370094286800d0'),
    ('__ZN21IOUSBDeviceControllerD2Ev', None, 0xfffffff00617ba24, 'ba370014200020d4200020d4e0cc00f0'),
    ('__ZN21IOUSBDeviceController12readyForHostEv', None, 0xfffffff00617db18, 'f44fbea9fd7b01a9fd430091f30300aa'),
    ('__ZN21IOUSBDeviceController18completeEndpointIOEP24_IOUSBDeviceIORequestTag', None, 0xfffffff00617dbdc, 'f44fbea9fd7b01a9fd430091210300b4'),
    ('__ZN25IOUSBDeviceControlRequest16inputSetupPacketEPh', None, 0xfffffff006175cc0, 'ff0302d1f85f04a9f65705a9f44f06a9'),
    ('__ZN25IOUSBDeviceControlRequest18processSetupPacketEv', None, 0xfffffff00617641c, 'ff8301d1f65703a9f44f04a9fd7b05a9'),
    ('__ZN25IOUSBDeviceControlRequest15inputDataPacketEPhy', None, 0xfffffff006176948, 'ffc301d1f85f03a9f65704a9f44f05a9'),
    ('__ZN25IOUSBDeviceControlRequest17stallSetupRequestEv', None, 0xfffffff0061774bc, 'ffc301d1f65704a9f44f05a9fd7b06a9'),
    ('__ZN25IOUSBDeviceControlRequest5resetEv', None, 0xfffffff006175840, 'ff8301d1f65703a9f44f04a9fd7b05a9'),
    ('__ZNK20IOUSBDeviceInterface24getAlternateSettingCountEv', None, 0xfffffff00616fa0c, 'ff8300d1fd7b01a9fd430091bfc31fb8'),
    ('__ZN20IOUSBDeviceInterface31getPipeCountForAlternateSettingEh', None, 0xfffffff00616fa58, 'ff8300d1fd7b01a9fd430091bfc31fb8'),
    ('__ZN20IOUSBDeviceInterface26getPipeForAlternateSettingEih', None, 0xfffffff00616faa8, 'ff8300d1fd7b01a9fd430091ff0700f9'),
    ('__ZN15IOUSBDevicePipe12getDirectionEv', None, 0xfffffff006173e34, '001c40b9c0035fd6003440b9c0035fd6'),
]


def symbols_of(blob):
    image = macho(blob)
    table = next(at for cmd, at, _ in image['commands'] if cmd == 2)
    sym, count, strings, _ = struct.unpack_from('<4I', blob, table + 8)
    out = []
    for i in range(count):
        ix, kind, _, _, value = struct.unpack_from('<IBBHQ', blob, sym + i * 16)
        out.append((blob[strings + ix:blob.index(b'\0', strings + ix)].decode(), value, kind))
    return image, out


def compiled_vtable(blob, image, symbols, name):
    """Symbols named by the relocations of a vtable in an unlinked object."""
    vtable = next(value for sym, value, kind in symbols if sym == name and kind & 0x0e)
    relocations = {}
    for segment in image['segments']:
        for section in segment['sections']:
            if section['va'] <= vtable < section['va'] + section['size']:
                ro, rc = struct.unpack_from('<II', blob, section['command_offset'] + 56)
                for i in range(rc):
                    offset, word = struct.unpack_from('<II', blob, ro + i * 8)
                    if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
                        relocations[section['va'] + offset] = symbols[word & 0xffffff][0]
    entries = []
    while vtable + 16 + len(entries) * 8 in relocations:
        entries.append(relocations[vtable + 16 + len(entries) * 8])
    return entries




def require(ok, why):
    if not ok:
        raise SystemExit('abi_check: ' + why)


def text_of(data):
    parsed = macho(data)
    section = next(s for g in parsed['segments'] for s in g['sections'] if s['name'] == '__text')
    return data[section['fileoff']:section['fileoff'] + section['size']], parsed



# --- xhci: SwitchXHCI against AppleUSBXHCI ----------------------------------------------------------------------------
XHCI_VTABLE = 0xfffffff006dd4d38      # __ZTV12AppleUSBXHCI (entries start at +16)
XHCI_METACLASS = 0xfffffff007af5468   # AppleUSBXHCI::gMetaClass
XHCI_METACLASS_SIZE = 0xfffffff005ebdb20   # mov w3, #0xe48 before OSMetaClass::OSMetaClass(name, super, size)
XHCI_SLOTS = 299
XHCI_SIZE = 0xe48
# Mach-O headers of the original kexts the bound code lives in (prelink info _PrelinkExecutableLoadAddr).
XHCI_KEXTS = {'com.apple.driver.usb.AppleUSBXHCI': 0xfffffff0053a5cc0, 'com.apple.iokit.IOUSBHostFamily': 0xfffffff005383600}
# Ours: destructors, getMetaClass, free, start, stop, copyMapper, regRead32, regWrite32, regWrite64.
XHCI_OWN = {0, 1, 7, 14, 86, 87, 253, 290, 292, 293}
# Bound by name and called by our code: (symbol, original slot or None, address or None = the slot's entry).
XHCI_CODE = [
    ('__ZN12AppleUSBXHCIC2EPK11OSMetaClass', None, 0xfffffff005ebdb40),   # AppleUSBHostController C2 + vtable store
    ('__ZN12AppleUSBXHCID2Ev', None, 0xfffffff005ebdb80),                 # b AppleUSBHostController::D2
    ('__ZN12AppleUSBXHCI5startEP9IOService', 86, 0xfffffff005ebdc60),
    ('__ZN12AppleUSBXHCI4stopEP9IOService', 87, None),                    # AppleUSBHostController::stop
    ('__ZN12AppleUSBXHCI4freeEv', 14, 0xfffffff005ec0808),
    ('__ZN12AppleUSBXHCI9regRead32Eyj', 290, 0xfffffff005ecf41c),         # add x0, x1, w2, uxtw; b ml_io_read32
    ('__ZN12AppleUSBXHCI10regWrite32Eyjj', 292, 0xfffffff005ecf42c),      # ...; b ml_io_write32
    ('__ZN12AppleUSBXHCI10regWrite64Eyjy', 293, 0xfffffff005ecf438),      # ...; b ml_io_write64
    ('__ZN12AppleUSBXHCI10copyMapperEv', 253, 0xfffffff005e204f0),         # AppleUSBHostController::copyMapper
]
XHCI_PINNED = {  # first instruction words of bound code (little-endian hex)
    0xfffffff005ebdb40: 'fd7bbfa9fd030091',   # stp x29, x30, [sp, #-16]!; mov x29, sp
    0xfffffff005ecf41c: '2040228b',           # add x0, x1, w2, uxtw
    0xfffffff005ecf42c: '2040228be10303aa',   # add x0, x1, w2, uxtw; mov x1, x3
    0xfffffff005ecf438: '2040228be10303aa',
    0xfffffff005e204f0: '004440f9',           # ldr x0, [x0, #136] (then b IOMapper::copyMapperForDevice)
}


def original_vtable(read, u64, vtable, slots, metaclass):
    require(read(vtable, 16) == bytes(16), 'original vtable header')
    original = [u64(vtable + 16 + 8 * i) for i in range(slots)]
    require(all(v >> 48 == 0xffff for v in original), f'original vtable has {slots} code slots')
    require(u64(vtable + 16 + 8 * slots) == 0 or u64(vtable + 16 + 8 * slots) >> 48 != 0xffff,
            f'original vtable ends after {slots} slots')
    getter = original[7]  # getMetaClass: adrp x0 / add x0 / ret returning the metaclass
    adrp, add, ret = struct.unpack('<3I', read(getter, 12))
    imm = ((adrp >> 29) & 3) | ((adrp >> 5) & 0x7ffff) << 2
    page = (getter & ~0xfff) + ((imm - (1 << 21) if imm & (1 << 20) else imm) << 12)
    require(adrp & 0x9f00001f == 0x90000000 and add & 0xffc003ff == 0x91000000 and ret == 0xd65f03c0 and
            page + ((add >> 10) & 0xfff) == metaclass, 'original getMetaClass returns the pinned gMetaClass')
    return original


def check_xudc(kc, parent, read, u64, blob, image, symbols):
    original_names = exports(kc, parent)
    original = original_vtable(read, u64, VTABLE, SLOTS, METACLASS)
    for name, slot, address, head in CODE:
        require(read(address, 16) == bytes.fromhex(head), f'instruction bytes of {name} at {address:#x}')
        require(slot is None or original[slot] == address, f'{name} is original slot {slot}')
    compiled = compiled_vtable(blob, image, symbols, '__ZTV10SwitchXUDC')
    require(len(compiled) == SLOTS, f'SwitchXUDC vtable has {len(compiled)} slots, expected {SLOTS}')
    require('___cxa_pure_virtual' not in compiled, 'pure virtual left in SwitchXUDC')
    bound = {name: (slot, address) for name, slot, address, _ in CODE}
    for slot, name in enumerate(compiled):
        if slot in OWN:
            require(name.startswith(('__ZN10SwitchXUDC', '__ZNK10SwitchXUDC')), f'slot {slot} should be ours: {name}')
        elif name in bound:
            require(bound[name][0] == slot, f'slot {slot} binds {name} from slot {bound[name][0]}')
        else:
            require(name in original_names.get(original[slot], []), f'slot {slot}: {name} is not the original entry')
    data = {'__ZN21IOUSBDeviceController10gMetaClassE': METACLASS, '__ZTV21IOUSBDeviceController': VTABLE}
    allowed = {name: address for name, _, address, _ in CODE} | data
    imports = {name for name, _, kind in symbols if kind == 1 and
               any(k in name for k in ('IOUSBDevice', 'USBDeviceInterface', 'USBDevicePipe'))}
    owners = [macho(kc, backed_offset(parent['segments'], 0xfffffff0054789c0, 32, len(kc)))]
    return 'IOUSBDeviceFamily', allowed, imports, owners


def check_xhci(kc, parent, read, u64, blob, image, symbols):
    names = exports(kc, parent)
    original = original_vtable(read, u64, XHCI_VTABLE, XHCI_SLOTS, XHCI_METACLASS)
    movz = struct.unpack('<I', read(XHCI_METACLASS_SIZE, 4))[0]
    require(movz & 0xffe0001f == 0x52800003 and (movz >> 5) & 0xffff == XHCI_SIZE, 'AppleUSBXHCI is 0xe48 bytes')
    bound = {}
    for name, slot, address in XHCI_CODE:
        if address is None:
            address = original[slot]
        require(slot is None or original[slot] == address, f'{name} is original slot {slot}')
        if address in XHCI_PINNED:
            pin = bytes.fromhex(XHCI_PINNED[address])
            require(read(address, len(pin)) == pin, f'instruction bytes of {name} at {address:#x}')
        bound[name] = address
    require(struct.unpack('<I', read(bound['__ZN12AppleUSBXHCID2Ev'], 4))[0] >> 26 == 0x05, 'AppleUSBXHCI D2 is a branch')
    compiled = compiled_vtable(blob, image, symbols, '__ZTV10SwitchXHCI')
    require(len(compiled) == XHCI_SLOTS, f'SwitchXHCI vtable has {len(compiled)} slots, expected {XHCI_SLOTS}')
    require('___cxa_pure_virtual' not in compiled, 'pure virtual left in SwitchXHCI')
    for slot, name in enumerate(compiled):
        if slot in XHCI_OWN:
            require(name.startswith(('__ZN10SwitchXHCI', '__ZNK10SwitchXHCI')), f'slot {slot} should be ours: {name}')
        elif name.startswith(('__ZN12AppleUSBXHCI', '__ZNK12AppleUSBXHCI')):
            require(name not in bound or bound[name] == original[slot], f'slot {slot} binds {name} elsewhere')
            require(not names.get(original[slot]), f'slot {slot}: the original is the kernel\'s {names.get(original[slot])}')
            bound[name] = original[slot]
        else:
            require(name in names.get(original[slot], []), f'slot {slot}: {name} is not the original entry')
    data = {'__ZN12AppleUSBXHCI10gMetaClassE': XHCI_METACLASS, '__ZTV12AppleUSBXHCI': XHCI_VTABLE}
    allowed = dict(bound) | data
    imports = {name for name, _, kind in symbols if kind == 1 and 'AppleUSBXHCI' in name}
    owners = [macho(kc, backed_offset(parent['segments'], header, 32, len(kc))) for header in XHCI_KEXTS.values()]
    return 'AppleUSBXHCI/IOUSBHostFamily', allowed, imports, owners


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--class', dest='kind', required=True, choices=('xudc', 'xhci'))
    for option in ('--kernelcache', '--object', '--late', '--bindings', '--output'):
        parser.add_argument(option, required=True, type=Path)
    parser.add_argument('--write', action='store_true', help='regenerate --bindings instead of comparing')
    args = parser.parse_args()
    kc = args.kernelcache.read_bytes()
    parent = macho(kc)

    def read(va, n):
        at = backed_offset(parent['segments'], va, n, len(kc))
        return kc[at:at + n]

    def u64(va):
        return struct.unpack('<Q', read(va, 8))[0]
    blob = args.object.read_bytes()
    image, symbols = symbols_of(blob)
    check = check_xudc if args.kind == 'xudc' else check_xhci
    family, allowed, imports, owners = check(kc, parent, read, u64, blob, image, symbols)
    unknown = imports - set(allowed)
    require(not unknown, 'unaudited original imports: ' + ', '.join(sorted(unknown)))
    require(not any(name.endswith(('D0Ev', 'D1Ev')) for name in imports), 'abstract base D0/D1 must not be called')
    segments = [s for owner in owners for s in owner['segments'] if s['name'] != '__LINKEDIT']
    for name in imports:
        require(any(s['va'] <= allowed[name] < s['va'] + s['size'] for s in segments), f'{name} outside {family}')

    text = (f'// Generated by abi_check.py --class {args.kind} --write from the 21H461 kernelcache: original {family}\n'
            '// addresses plus the prelink slide (-0x1000000). Do not edit.\n' +
            ''.join(f'.globl {n}\n.set {n}, {allowed[n] + SLIDE:#x}\n' for n in sorted(imports)))
    if args.write:
        args.bindings.write_text(text)
    else:
        require(args.bindings.read_text() == text,
                f'{args.bindings} is stale: rerun abi_check.py --class {args.kind} --write (imports changed)')

    late = args.late.read_bytes()
    late_text, late_image = text_of(late)
    require(late_text == text_of(blob)[0], 'late-init staging changed instructions')
    require(not any(s['name'] == '__mod_init_func' for g in late_image['segments'] for s in g['sections']),
            'the late object still has an automatic initializer')
    args.output.write_bytes(late)


if __name__ == '__main__':
    main()
