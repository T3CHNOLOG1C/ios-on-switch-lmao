"""Original<Class>.h and a driver's bindings from the ABI tables (interface/<Class>.json, kinds/<kind>/<Class>.json).

    gen.py header --interface I --table T --out Original<Class>.h
    gen.py bindings --pair I T [--pair I T]... --object O... --out bindings.S

The header declares the original class as the kind lays it out: its instance size, its virtuals in slot order (a slot
no driver names is opaque<N>), field accessors at the kind's offsets, the kind's constants, the members drivers call
and SWITCH_ABI_KIND (the kind's name, for messages), after the interface's declarations (the types its methods name
that no included header declares). A function without a "decl" is only bound: a C++ member the driver's own header
declares. A "guard" class's unnamed slots are pure, except the ones its table inherits (hints.inherit), and
SWITCH_ABI_GUARDS(trap) overrides each pure one in the subclass with trap(<slot>): no original code runs in a slot the
table did not choose. The bindings bind every symbol of the tables' addresses (and refuse an original import they
lack) at its kernelcache address plus the kind's prelink slide; an entry the table marks "unported" becomes a
function that panics naming it.
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

M64 = (1 << 64) - 1
ACCESS = """#ifndef SWITCH_ABI_ACCESS
#define SWITCH_ABI_ACCESS
template <typename T> static inline T switch_abi_load(const void *object, unsigned long offset)
{
    T value;
    __builtin_memcpy(&value, static_cast<const unsigned char *>(object) + offset, sizeof(value));
    return value;
}
template <typename T> static inline void switch_abi_store(void *object, unsigned long offset, T value)
{
    __builtin_memcpy(static_cast<unsigned char *>(object) + offset, &value, sizeof(value));
}
#endif
"""


def load(path):
    return json.loads(Path(path).read_text())


def number(value):
    return int(value, 0) if isinstance(value, str) else value


def mangled_class(name):
    return '{}{}'.format(len(name), name)


def doc(entry, indent):
    """An entry's "doc" as comment lines, wrapped at 120 columns."""
    words, lines, line = entry.get('doc', '').split(), [], indent + '//'
    for word in words:
        if len(line) + 1 + len(word) > 120:
            lines.append(line)
            line = indent + '//'
        line += ' ' + word
    return lines + [line] if words else lines


def declaration(decl, indent):
    """decl; wrapped after its commas at 120 columns."""
    text = indent + decl + ';'
    if len(text) <= 120 or '(' not in decl:
        return [text]
    head, _, rest = decl.partition('(')
    parts, lines, line = rest.split(', '), [], indent + head + '('
    pad = ' ' * len(line)
    for i, part in enumerate(parts):
        piece = part + (', ' if i + 1 < len(parts) else ';')
        if len(line) + len(piece.rstrip()) > 120 and line.strip():
            lines.append(line.rstrip())
            line = pad
        line += piece
    return lines + [line]


def slot_declarations(interface, table):
    """The class body's virtuals: the interface's overrides of inherited slots, then every slot the class adds."""
    methods = interface['methods']
    first, count = table['first_slot'], table['slots']
    named = {int(slot): key for slot, key in table['layout'].items()}
    unknown = sorted(set(named.values()) - set(methods))
    if unknown:
        raise SystemExit('{}: slots name methods the interface lacks: {}'.format(table['class'], unknown))
    lines = []
    for slot, key in sorted(named.items()):
        if slot < first:
            lines += doc(methods[key], '    ') + declaration(methods[key]['decl'], '    ')
    guarded = guarded_slots(interface, table)
    for slot in range(first, count):
        if slot in named:
            lines += doc(methods[named[slot]], '    ') + declaration(methods[named[slot]]['decl'], '    ')
        elif slot in guarded:
            lines.append('    virtual void opaque{}() = 0;'.format(slot))
        else:
            lines.append('    SWITCH_ABI_OPAQUE({});'.format(slot))
    return lines


def guarded_slots(interface, table):
    """Return a guard class's added slots the table neither names nor inherits: pure, and trapped by the subclass."""
    if not interface.get('guard'):
        return []
    first, count = table['first_slot'], table['slots']
    named = {int(slot) for slot in table['layout']}
    inherit = {int(slot) for slot in table.get('hints', {}).get('inherit', {})}
    stray = sorted(s for s in inherit if s in named or not first <= s < count)
    if stray:
        raise SystemExit(
            '{}: inherited slots that are named or not among the added ones: {}'.format(table['class'], stray)
        )
    return [s for s in range(first, count) if s not in named and s not in inherit]


def guards(interface, table):
    """Return SWITCH_ABI_GUARDS(trap): the subclass's override of every guarded slot, calling trap(<slot>)."""
    slots = guarded_slots(interface, table)
    if not slots:
        return []
    body = ['    void opaque{0}() override {{ trap({0}); }}'.format(s) for s in slots]
    return ['#define SWITCH_ABI_GUARDS(trap) \\'] + [line + ' \\' for line in body[:-1]] + body[-1:]


def accessors(interface, table, public):
    lines = []
    for key, field in interface.get('fields', {}).items():
        if bool(field.get('public')) != public:
            continue
        offset = table['fields'].get(key)
        if offset is None:
            raise SystemExit('{}: no offset for field {}'.format(table['class'], key))
        kind, access = field['type'], field['access'].split()
        stored = 'uint8_t' if kind == 'bool' else kind
        setter = 'set' + key[0].upper() + key[1:]
        name = kind + ('' if kind.endswith('*') else ' ')
        lines += doc(field, '    ')
        if 'get' in access:
            value = 'switch_abi_load<{}>(this, {:#x})'.format(stored, number(offset))
            if kind == 'bool':
                value += ' != 0'
            lines.append('    {}{}() const {{ return {}; }}'.format(name, key, value))
        if 'set' in access:
            lines.append(
                '    void {}({} value) {{ switch_abi_store<{}>(this, {:#x}, value); }}'.format(
                    setter, kind, stored, number(offset)
                )
            )
    return lines


def constant_declarations(interface: dict, table: dict) -> list[str]:
    """Declare the interface's constants as static members, at the kind's values."""
    lines = []
    for key, constant in interface.get('constants', {}).items():
        value = table.get('constants', {}).get(key)
        if value is None:
            raise SystemExit(f'{table["class"]}: no value for constant {key}')
        lines += [*doc(constant, '    '), f'    static constexpr unsigned long {key} = {number(value):#x};']
    return lines


def header(interface, table, table_name):
    cls, sup = interface['class'], interface['superclass']
    size = number(table['size'])
    structors = 'OSDeclareAbstractStructors' if interface['structors'] == 'abstract' else 'OSDeclareDefaultStructors'
    out = ['// Generated by kits/SwitchABI/gen.py from {}: do not edit.'.format(table_name)] + doc(interface, '')
    out += ['#pragma once'] + ['#include <{}>'.format(h) for h in interface['includes']]
    if interface.get('declarations'):
        out += [''] + interface['declarations']
    out += ['', ACCESS.rstrip('\n'), '', '#define SWITCH_ABI_KIND "{}"'.format(table['kind']), '']
    out.append(
        '#define SWITCH_ABI_OPAQUE(n) '
        'virtual __attribute__((deprecated("inherited original slot: never call"))) void opaque##n()'
    )
    out += ['', 'class {} : public {} {{'.format(cls, sup), '    {}({})'.format(structors, cls)]
    out.append('    unsigned char switchAbiStorage_[{:#x} - sizeof({})];'.format(size, sup))
    fields = accessors(interface, table, public=False)
    if fields:
        out += ['', 'protected:'] + fields
    out += [
        '',
        'public:',
        *accessors(interface, table, public=True),
        *constant_declarations(interface, table),
        *slot_declarations(interface, table),
    ]
    out += ['    {};'.format(decl) for decl in interface.get('members', [])]
    out += ['};', '#undef SWITCH_ABI_OPAQUE']
    out.append('static_assert(sizeof({}) == {:#x}, "{} as {} lays it out");'.format(cls, size, cls, table['kind']))
    trap = guards(interface, table)
    if trap:
        out += ['', *trap]
    functions = [f for f in interface.get('functions', {}).values() if 'decl' in f]
    if functions:
        out += ['', 'extern "C" {']
        for f in functions:
            out += doc(f, '') + declaration(f['decl'], '')
        out.append('}')
    return '\n'.join(out) + '\n'


def symbol_map(interface, table):
    """Mangled symbol -> (address or None for unported, what it is)."""
    cls = mangled_class(interface['class'])
    addresses = table['addresses']
    out = {}

    def put(symbol, key):
        if key in addresses:
            value = addresses[key]
            out[symbol] = (None if value == 'unported' else number(value), key)

    put('__ZN{}10gMetaClassE'.format(cls), 'gMetaClass')
    put('__ZNK{}12getMetaClassEv'.format(cls), 'getMetaClass')
    put('__ZTV{}'.format(cls), 'vtable')
    for structor, suffix in (('C2', 'C2EPK11OSMetaClass'), ('D2', 'D2Ev'), ('D1', 'D1Ev'), ('D0', 'D0Ev')):
        put('__ZN{}{}'.format(cls, suffix), structor)
    for key, method in interface['methods'].items():
        if 'mangled' in method:
            put(method['mangled'], key)
    for key in addresses:
        opaque = re.fullmatch(r'opaque(\d+)', key)
        if opaque:
            put('__ZN{}{}Ev'.format(cls, mangled_class(key)), key)
    for name, function in interface.get('functions', {}).items():
        for symbol in function.get('symbols', [name]):
            put('_' + symbol, name)
    return out


def undefined_symbols(path):
    data = Path(path).read_bytes()
    magic, _, _, _, ncmds = struct.unpack_from('<IiiII', data)
    if magic != 0xFEEDFACF:
        raise SystemExit('{}: not a 64-bit Mach-O object'.format(path))
    at, names = 32, set()
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == 2:
            symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, at + 8)
            for i in range(nsyms):
                strx, kind, _, _, _ = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
                if (kind & 0xE0) == 0 and (kind & 0x0E) == 0 and kind & 1:
                    names.add(data[stroff + strx : data.index(b'\0', stroff + strx)].decode())
        at += size
    return names


def bindings(pairs, objects):
    imports = set().union(*(undefined_symbols(o) for o in objects))
    lines, traps, sources = [], [], []
    for interface, table, table_name in pairs:
        cls = mangled_class(interface['class'])
        known = symbol_map(interface, table)
        slide = number(table['slide'])
        sources.append(table_name)
        ours = {s for s in imports if s in known or re.match(r'__Z(N|NK|TV)' + cls, s)}
        unknown = sorted(s for s in ours if s not in known)
        if unknown:
            raise SystemExit('{}: imports the table does not bind: {}'.format(table_name, ', '.join(unknown)))
        for symbol in sorted(known):
            address, key = known[symbol]
            if address is None:
                traps.append(symbol)
            else:
                lines.append('.globl {0}\n.set {0}, {1:#x}  // {2}'.format(symbol, (address + slide) & M64, key))
        imports -= set(ours)
    out = ['// Generated by kits/SwitchABI/gen.py from {}: do not edit.'.format(', '.join(sources))] + lines
    for i, symbol in enumerate(traps):
        out += [
            '.text',
            '.p2align 2',
            '.globl {}'.format(symbol),
            '{}:'.format(symbol),
            '    adrp x0, Lswitch_abi_trap{}@PAGE'.format(i),
            '    add x0, x0, Lswitch_abi_trap{}@PAGEOFF'.format(i),
            '    b _switch_abi_unported',
            '.section __TEXT,__cstring,cstring_literals',
            'Lswitch_abi_trap{}:'.format(i),
            '    .asciz "{}"'.format(symbol),
        ]
    return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    h = sub.add_parser('header')
    h.add_argument('--interface', required=True)
    h.add_argument('--table', required=True)
    h.add_argument('--out', required=True, type=Path)
    b = sub.add_parser('bindings')
    b.add_argument('--pair', nargs=2, action='append', required=True, metavar=('INTERFACE', 'TABLE'))
    b.add_argument('--object', action='append', required=True)
    b.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    if args.command == 'header':
        args.out.write_text(header(load(args.interface), load(args.table), table_label(args.table)))
    else:
        pairs = [(load(i), load(t), table_label(t)) for i, t in args.pair]
        args.out.write_text(bindings(pairs, args.object))


def table_label(path):
    parts = Path(path).parts
    return '/'.join(parts[parts.index('kinds') :]) if 'kinds' in parts else path


if __name__ == '__main__':
    sys.exit(main())
