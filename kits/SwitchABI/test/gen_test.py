"""The committed ABI tables agree with their interfaces, and gen.py lays out headers and bindings from them."""

import json
import struct
import tempfile
import unittest
from pathlib import Path

from kits.SwitchABI import gen

ROOT = Path(__file__).resolve().parents[1]


def tables():
    for path in sorted((ROOT / 'kinds').glob('*/*.json')):
        yield path, gen.load(ROOT / 'interface' / path.name), gen.load(path)


def macho_object(undefined):
    """A minimal MH_OBJECT whose symbol table holds only these undefined external symbols."""
    strings = b'\0' + b''.join(s.encode() + b'\0' for s in undefined)
    symoff = 32 + 24
    stroff = symoff + 16 * len(undefined)
    head = struct.pack('<IiiIIIII', 0xFEEDFACF, 0x0100000C, 0, 1, 1, 24, 0, 0)
    symtab = struct.pack('<6I', 2, 24, symoff, len(undefined), stroff, len(strings))
    nlist, at = b'', 1
    for s in undefined:
        nlist += struct.pack('<IBBHQ', at, 0x01, 0, 0, 0)
        at += len(s) + 1
    return head + symtab + nlist + strings


INTERFACE = {
    'class': 'Base',
    'superclass': 'IOService',
    'structors': 'abstract',
    'includes': ['IOKit/IOService.h'],
    'methods': {
        'stop': {'decl': 'void stop(IOService *provider) override', 'mangled': '__ZN4Base4stopEP9IOService'},
        'kick': {'decl': 'virtual void kick() = 0'},
    },
    'fields': {'armed': {'type': 'bool', 'access': 'get set'}},
    'functions': {'switch_base_check': {'decl': 'void switch_base_check(Base *base)'}},
}
TABLE = {
    'class': 'Base',
    'kind': 'TEST',
    'size': '0x100',
    'first_slot': 168,
    'slots': 171,
    'slide': '-0x1000',
    'layout': {'87': 'stop', '169': 'kick'},
    'fields': {'armed': '0x90'},
    'addresses': {
        'gMetaClass': '0xfffffff007000000',
        'stop': '0xfffffff006000000',
        'opaque168': '0xfffffff006000100',
        'opaque170': 'unported',
        'switch_base_check': '0xfffffff006000200',
    },
}


class TablesTest(unittest.TestCase):
    def test_every_table_matches_its_interface(self):
        for path, interface, table in tables():
            with self.subTest(table=str(path.relative_to(ROOT))):
                self.assertEqual(table['class'], interface['class'])
                self.assertEqual(table['kind'], path.parent.name)
                self.assertTrue(set(table['layout'].values()) <= set(interface['methods']))
                self.assertEqual(set(table['fields']), set(interface.get('fields', {})))
                self.assertLessEqual(table['first_slot'], table['slots'])
                if interface.get('header', True):
                    gen.header(interface, table, path.name)

    def test_one_slide_per_kind(self):
        slides = {}
        for path, _, table in tables():
            slides.setdefault(table['kind'], set()).add(table['slide'])
        self.assertEqual({k: len(v) for k, v in slides.items()}, {k: 1 for k in slides})

    def test_every_virtual_the_class_adds_has_a_slot(self):
        for path, interface, table in tables():
            with self.subTest(table=str(path.relative_to(ROOT))):
                added = {k for k, m in interface['methods'].items() if not m['decl'].endswith(' override')}
                self.assertLessEqual(added, set(table['layout'].values()))


class HeaderTest(unittest.TestCase):
    def test_slots_in_order_with_opaque_fillers(self):
        text = gen.header(INTERFACE, TABLE, 'kinds/TEST/Base.json')
        body = text[text.index('public:') :]
        self.assertLess(body.index('void stop('), body.index('SWITCH_ABI_OPAQUE(168)'))
        self.assertLess(body.index('SWITCH_ABI_OPAQUE(168)'), body.index('virtual void kick() = 0'))
        self.assertLess(body.index('virtual void kick() = 0'), body.index('SWITCH_ABI_OPAQUE(170)'))
        self.assertIn('static_assert(sizeof(Base) == 0x100', text)
        self.assertIn('switch_abi_load<uint8_t>(this, 0x90) != 0', text)

    def test_a_slot_the_interface_lacks_is_refused(self):
        table = dict(TABLE, layout={'168': 'missing'})
        with self.assertRaises(SystemExit):
            gen.header(INTERFACE, table, 'kinds/TEST/Base.json')


class BindingsTest(unittest.TestCase):
    def bind(self, undefined):
        with tempfile.TemporaryDirectory() as tmp:
            obj = Path(tmp) / 'a.o'
            obj.write_bytes(macho_object(undefined))
            return gen.bindings([(INTERFACE, TABLE, 'kinds/TEST/Base.json')], [str(obj)])

    def test_every_table_address_is_bound_with_the_slide(self):
        text = self.bind(['__ZN4Base4stopEP9IOService'])
        self.assertIn('.set __ZN4Base4stopEP9IOService, 0xfffffff005fff000', text)
        self.assertIn('.set __ZN4Base10gMetaClassE, 0xfffffff006fff000', text)
        self.assertIn('.set _switch_base_check, 0xfffffff005fff200', text)

    def test_unported_becomes_a_named_trap(self):
        text = self.bind(['__ZN4Base9opaque170Ev'])
        self.assertIn('__ZN4Base9opaque170Ev:', text)
        self.assertIn('b _switch_abi_unported', text)
        self.assertIn('.asciz "__ZN4Base9opaque170Ev"', text)

    def test_an_import_the_table_lacks_is_refused(self):
        with self.assertRaises(SystemExit):
            self.bind(['__ZN4Base7unknownEv'])


if __name__ == '__main__':
    unittest.main()
