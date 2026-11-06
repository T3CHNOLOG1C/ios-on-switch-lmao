"""subclass.py's vtable check against a minimal object holding one subclass vtable."""

import struct
import tempfile
import unittest
from pathlib import Path

from kits.SwitchABI import subclass

INTERFACE = {
    'class': 'Base',
    'methods': {
        'stop': {'decl': 'void stop(IOService *provider) override', 'mangled': '__ZN4Base4stopEP9IOService'},
        'kick': {'decl': 'virtual void kick() = 0', 'mangled': '__ZN4Base4kickEv'},
    },
}
TABLE = {'kind': 'TEST', 'first_slot': 168, 'slots': 171, 'layout': {'87': 'stop', '169': 'kick'}}


def entries(**changes: str) -> list[str]:
    out = [f'__ZN9IOService{i}Ev' for i in range(168)]
    out[0], out[1], out[7] = '__ZN4MineD1Ev', '__ZN4MineD0Ev', '__ZNK4Mine12getMetaClassEv'
    out[87] = '__ZN4Base4stopEP9IOService'
    out += ['__ZN4Base9opaque168Ev', '__ZN4Mine4kickEv', '__ZN4Base9opaque170Ev']
    for slot, symbol in changes.items():
        out[int(slot[1:])] = symbol
    return out


def vtable_object(names: list[str]) -> bytes:
    """Lay out an MH_OBJECT whose __DATA,__const holds __ZTV4Mine with one external relocation per entry."""
    symbols = ['__ZTV4Mine', *sorted(set(names))]
    index = {s: i for i, s in enumerate(symbols)}
    data = bytes(16 + 8 * len(names))
    relocations = b''.join(struct.pack('<iI', 16 + 8 * i, index[s] | 3 << 25 | 1 << 27) for i, s in enumerate(names))
    strings = b'\0' + b''.join(s.encode() + b'\0' for s in symbols)
    header, segment = 32, 72 + 80
    data_at = header + segment + 24
    reloc_at = data_at + len(data)
    sym_at = reloc_at + len(relocations)
    str_at = sym_at + 16 * len(symbols)
    out = struct.pack('<IiiIIIII', 0xFEEDFACF, 0x0100000C, 0, 1, 2, segment + 24, 0, 0)
    out += struct.pack('<II16sQQQQiiII', 0x19, segment, b'', 0, len(data), data_at, len(data), 3, 3, 1, 0)
    out += struct.pack(
        '<16s16sQQIIIIIIII', b'__const', b'__DATA', 0, len(data), data_at, 3, reloc_at, len(names), 0, 0, 0, 0
    )
    out += struct.pack('<6I', 2, 24, sym_at, len(symbols), str_at, len(strings))
    out += data + relocations
    at = 1
    for i, s in enumerate(symbols):
        out += struct.pack('<IBBHQ', at, 0x0F if i == 0 else 0x01, 1 if i == 0 else 0, 0, 0)
        at += len(s) + 1
    return out + strings


class VtableTest(unittest.TestCase):
    def check(self, names: list[str], own: tuple[str, ...] = ('kick',)) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            obj = Path(tmp) / 'a.o'
            obj.write_bytes(vtable_object(names))
            subclass.check_vtable(INTERFACE, TABLE, 'Mine', list(own), [str(obj)])

    def test_the_table_s_slots_pass(self):
        self.check(entries())

    def test_an_override_own_does_not_name_is_refused(self):
        with self.assertRaises(SystemExit):
            self.check(entries(), own=())

    def test_a_slot_off_the_table_is_refused(self):
        with self.assertRaises(SystemExit):
            self.check(entries(s168='__ZN4Base9opaque170Ev'))

    def test_a_pure_virtual_is_refused(self):
        with self.assertRaises(SystemExit):
            self.check(entries(s169='___cxa_pure_virtual'), own=())

    def test_a_short_vtable_is_refused(self):
        with self.assertRaises(SystemExit):
            self.check(entries()[:-1])


if __name__ == '__main__':
    unittest.main()
