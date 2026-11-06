"""kinds/<kind>/<Class>.json from a fileset (arm64e) kernelcache: what can be read is read, the rest is the table's hints.

    extract.py --kernelcache KC --ipsw IPSW [--kind K] [--class C]... [--check]
    bazel run [--config=ios27] //kits/SwitchABI:extract     (rewrites the selected kind's tables)
    bazel run [--config=ios27] //kits/SwitchABI:verify      (compares them)

The kind is the one whose pinned kernelcache sha256 (ipsw/pins.bzl) the kernelcache has.

Read from the kernelcache: the instance size, gMetaClass and the vtable (the metaclass initialisers), the slot count,
each interface method's slot (its PAC discriminator, from Apple's mangled root name), every other slot the class adds
(opaque<N>, or a refusal if it is pure: a new contract), the C2 constructor and D2 destructor (the functions storing
the vtable), and every function a hint locates by a string it references. Hints are what no reading can name: a
renamed method's slot, the string that finds a function, a field's offset or a constant with the instruction that shows
it (checked here). The same kernelcache always gives the same JSON; --check compares instead of writing.

A kernelcache that is not a fileset (iOS 17's arm64 prelinked kernel, unstripped of nothing but without PAC) cannot
name slots; its tables are checked instead: the vtable has the table's slot count, every bound slot holds the
table's address, and getMetaClass returns the table's gMetaClass (for a table that binds the vtable); each function hint
with a body holds those words at the address the table binds.
"""

import argparse
import collections
import hashlib
import json
import os
import sys
from collections.abc import Callable
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import kernelcache  # noqa: E402

HERE = Path(__file__).parent


# Slots OSDeclare*Structors fills for every class: the D1 and D0 destructors and getMetaClass.
STRUCTOR_SLOTS = {0, 1, 7}


def pure_virtual(fs):
    """___cxa_pure_virtual: the kernel function most slots a class adds (beyond its superclass's) point at."""
    by_meta = {info['meta']: name for name, info in fs.inheritance.items()}
    targets = collections.Counter()
    for cls, info in fs.inheritance.items():
        parent = by_meta.get(info['parent'])
        if cls in fs.classes and parent in fs.classes:
            first = len(fs.classes[parent]['methods'])
            targets.update(t for i, t, _ in fs.slots(cls) if i >= first and fs.owner(t) == 'com.apple.kernel')
    return targets.most_common(1)[0][0]


def pac_root(method):
    return method.get('pac') or method['mangled'][1:]


def layout(fs, interface, table, pure):
    cls = interface['class']
    first = len(fs.classes[interface['superclass']]['methods'])
    parent = dict((i, t) for i, t, _ in fs.slots(interface['superclass']))
    slots = fs.slots(cls)
    by_pac = collections.defaultdict(list)
    for i, _, pac in slots:
        by_pac[pac].append(i)
    hints = {int(k): v for k, v in table.get('hints', {}).get('layout', {}).items()}
    named, found = {}, {}
    for key, method in interface['methods'].items():
        hit = [i for i in by_pac.get(kernelcache.discriminator(pac_root(method)), []) if i not in hints]
        if len(hit) == 1:
            named[hit[0]] = key
            found[key] = 'slot {} by PAC of {}'.format(hit[0], pac_root(method))
    for slot, key in hints.items():
        named[slot] = key
        found[key] = 'slot {} by hint'.format(slot)
    errors = []
    for i, target, _ in slots:
        own = parent.get(i) != target
        key = named.get(i)
        if i < first and own and key is None and i not in STRUCTOR_SLOTS:
            errors.append(
                'slot {} overrides {} at {:#x}; the interface must declare it'.format(
                    i, interface['superclass'], target
                )
            )
        if i < first and key is not None and not own:
            del named[i]
            found.pop(key)
        if i >= first and target == pure and key is None:
            errors.append('slot {} is a pure virtual the interface does not declare'.format(i))
        if key is not None and i < len(slots) and (target == pure) != interface['methods'][key]['decl'].endswith('= 0'):
            errors.append('{} (slot {}) is {}pure in the kernelcache'.format(key, i, '' if target == pure else 'not '))
    missing = sorted(set(interface['methods']) - set(named.values()))
    errors += [
        '{}: no slot (no PAC match and no hint)'.format(k)
        for k in missing
        if interface['methods'][k]['decl'].endswith('= 0') or 'override' not in interface['methods'][k]['decl']
    ]
    return first, slots, named, found, errors


def structors(fs, cls):
    """C2 (the first of the constructors that store the vtable after calling the superclass's) and the D2 right after
    them (a bare branch to the superclass's D2)."""
    info = fs.inheritance[cls]
    lo, hi = fs.text(info['bundle'])
    words = fs.words(lo, hi)
    stores = sorted({fs.function(pc) for pc, a in kernelcache.references(words) if a == info['vtable']})
    plain = [f for f in stores if f[1] - f[0] == min(e - s for s, e in stores)]
    if not plain:
        return {}
    c2 = plain[0][0]
    starts = fs.function_starts(info['bundle'])
    after = starts[starts.index(plain[-1][0]) + 1]
    body = [w for _, w in fs.words(*fs.function(after)) if w != 0xD503245F]
    out = {'C2': c2}
    if body and kernelcache.branch(after, body[0]) and not kernelcache.branch(after, body[0])[1]:
        out['D2'] = after
    return out


def locate(fs, bundle, hint):
    """The function referencing a string (hint "string": its first characters), then hint "up" callers up."""
    lo, hi = fs.text(bundle)
    words = fs.words(lo, hi)
    strings = set(fs.strings(hint['string']))
    hits = sorted({fs.function(pc)[0] for pc, a in kernelcache.references(words) if a in strings})
    if len(hits) != 1:
        raise SystemExit('{!r}: {} functions reference it'.format(hint['string'], len(hits)))
    function = hits[0]
    for _ in range(hint.get('up', 0)):
        callers = sorted(
            {
                fs.function(pc)[0]
                for pc, w in words
                if kernelcache.branch(pc, w) and kernelcache.branch(pc, w)[0] == function
            }
        )
        if len(callers) != 1:
            raise SystemExit('{:#x}: {} callers'.format(function, len(callers)))
        function = callers[0]
    return function


def fields(fs, table):
    out, found = {}, {}
    for key, hint in sorted(table.get('hints', {}).get('fields', {}).items()):
        at, word = int(hint['at'], 16), int(hint['word'], 16)
        if fs.word(at) != word:
            raise SystemExit('field {}: the word at {:#x} is {:08x}, not {:08x}'.format(key, at, fs.word(at), word))
        out[key] = hint['offset']
        found[key] = 'offset shown by {:08x} at {:#x}'.format(word, at)
    return out, found


def constants(fs: kernelcache.Fileset, table: dict) -> tuple[dict, dict]:
    """Read hints.constants: each value with the instruction word that shows it, checked like a field's."""
    out, found = {}, {}
    for key, hint in sorted(table.get('hints', {}).get('constants', {}).items()):
        at, word = int(hint['at'], 16), int(hint['word'], 16)
        if fs.word(at) != word:
            raise SystemExit(f'constant {key}: the word at {at:#x} is {fs.word(at):08x}, not {word:08x}')
        out[key] = hint['value']
        found[key] = f'value shown by {word:08x} at {at:#x}'
    return out, found


def extract(fs, interface, table, kind, pure):
    cls = interface['class']
    info = fs.inheritance[cls]
    if interface.get('functions_only'):
        addresses, found = {}, {}
        for key, hint in sorted(table.get('hints', {}).get('functions', {}).items()):
            if hint == 'unported':
                addresses[key], found[key] = 'unported', 'not located in this kind yet'
                continue
            at = int(hint['at'], 16)
            body = [w for _, w in fs.words(*fs.function(at)) if w != 0xD503245F]
            want = [int(w, 16) for w in hint['body'].split()]
            if fs.function(at)[0] != at or body[: len(want)] != want:
                raise SystemExit('{}: {:#x} is not the function the hint describes'.format(key, at))
            addresses[key], found[key] = hex(at), hint['why']
        return dump(
            {
                'class': cls,
                'kind': kind,
                'size': hex(info['size']),
                'first_slot': 0,
                'slots': len(fs.slots(cls)),
                'slide': table['slide'],
                'layout': {},
                'fields': {},
                'addresses': addresses,
                'found': found,
                'hints': table.get('hints', {}),
            }
        )
    if interface.get('metaclass_only'):
        return dump(
            {
                'class': cls,
                'kind': kind,
                'size': hex(info['size']),
                'first_slot': len(fs.classes[interface['superclass']]['methods']),
                'slots': len(fs.slots(cls)),
                'slide': table['slide'],
                'layout': {},
                'fields': {},
                'addresses': {'gMetaClass': hex(info['meta'])},
                'found': {'gMetaClass': 'metaclass initialiser'},
                'hints': table.get('hints', {}),
            }
        )
    first, slots, named, found, errors = layout(fs, interface, table, pure)
    if errors:
        raise SystemExit('{} ({}):\n  {}'.format(cls, kind, '\n  '.join(errors)))
    targets = {i: t for i, t, _ in slots}
    addresses = {'gMetaClass': info['meta'], 'vtable': info['vtable'] - 16}
    found.update(gMetaClass='metaclass initialiser', vtable='metaclass initialiser')
    for slot, key in named.items():
        if targets[slot] != pure:
            addresses[key] = targets[slot]
    for i, target, _ in slots:
        if i >= first and i not in named:
            addresses['opaque{}'.format(i)] = target
    for key, address in structors(fs, cls).items():
        addresses[key] = address
        found[key] = 'stores the vtable' if key == 'C2' else 'branch after the constructors'
    for key, hint in sorted(table.get('hints', {}).get('functions', {}).items()):
        addresses[key] = locate(fs, info['bundle'], hint)
        found[key] = 'references "{}"'.format(hint['string']) + (
            ', {} caller(s) up'.format(hint['up']) if hint.get('up') else ''
        )
    offsets, field_found = fields(fs, table)
    found.update(field_found)
    values, value_found = constants(fs, table)
    found.update(value_found)
    out = {
        'class': cls,
        'kind': kind,
        'size': hex(info['size']),
        'first_slot': first,
        'slots': len(slots),
        'slide': table['slide'],
        'layout': {str(i): named[i] for i in sorted(named)},
        'fields': offsets,
    }
    if values:
        out['constants'] = values
    out.update(
        addresses={k: hex(v) for k, v in sorted(addresses.items(), key=lambda kv: (kv[1], kv[0]))},
        found=dict(sorted(found.items())),
        hints=table.get('hints', {}),
    )
    return dump(out)


def dump(table):
    return json.dumps(table, indent=2) + '\n'


def check_plain(path: Path, interface: dict, table: dict) -> None:
    sys.path.insert(0, str(HERE.parents[1] / 'tools/prelink'))
    from native_prelink import backed_offset, macho

    data = path.read_bytes()
    segments = macho(data)['segments']

    def read(va: int, size: int) -> int:
        return int.from_bytes(data[backed_offset(segments, va, size, len(data)) :][:size], 'little')

    addresses = {k: int(v, 16) for k, v in table['addresses'].items() if v != 'unported'}
    errors = plain_function_errors(lambda va: read(va, 4), addresses, table)
    if 'vtable' in addresses:
        code = [(g['va'], g['va'] + g['size']) for g in segments if g['initprot'] & 4]
        errors += plain_vtable_errors(read, code, addresses, table)
    if errors:
        joined = '\n  '.join(errors)
        raise SystemExit(f'{interface["class"]} ({table["kind"]}):\n  {joined}')


def plain_vtable_errors(
    read: Callable[[int, int], int], code: list[tuple[int, int]], addresses: dict[str, int], table: dict
) -> list[str]:
    """Check that the vtable has the slot count, every bound slot its address and getMetaClass the gMetaClass."""
    vtable = addresses['vtable'] + 16
    entries = [read(vtable + 8 * i, 8) for i in range(table['slots'] + 1)]
    errors = []
    if not all(any(lo <= e < hi for lo, hi in code) for e in entries[:-1]) or any(
        lo <= entries[-1] < hi for lo, hi in code
    ):
        errors.append(f'the vtable does not hold exactly {table["slots"]} code slots')
    for slot, key in table['layout'].items():
        if key in addresses and entries[int(slot)] != addresses[key]:
            errors.append(f'slot {slot} holds {entries[int(slot)]:#x}, the table binds {key} at {addresses[key]:#x}')
    getter = entries[7]
    adrp, add = (read(getter + 4 * i, 4) for i in range(2))
    page = (getter & ~0xFFF) + (kernelcache.sx(((adrp >> 5) & 0x7FFFF) << 2 | (adrp >> 29) & 3, 21) << 12)
    if page + ((add >> 10) & 0xFFF) != addresses['gMetaClass']:
        errors.append("getMetaClass does not return the table's gMetaClass")
    return errors


def plain_function_errors(word: Callable[[int], int], addresses: dict[str, int], table: dict) -> list[str]:
    """Check each hints.functions {at, body}: the words at `at`, and that `at` is the address the table binds."""
    errors = []
    for key, hint in sorted(table.get('hints', {}).get('functions', {}).items()):
        if not isinstance(hint, dict) or 'body' not in hint:
            continue
        at, want = int(hint['at'], 16), [int(w, 16) for w in hint['body'].split()]
        got = [word(at + 4 * i) for i in range(len(want))]
        if addresses.get(key) != at:
            errors.append(f'{key}: the hint is at {at:#x}, the table binds {table["addresses"].get(key)}')
        if got != want:
            words = ' '.join(f'{w:08x}' for w in got)
            errors.append(f'{key} at {at:#x}: the words are {words}, the hint says {hint["body"]}')
    return errors


def is_fileset(path):
    with open(path, 'rb') as f:
        return int.from_bytes(f.read(16)[12:16], 'little') == 12


def kind_of(kernelcache, pins):
    scope = {}
    exec(compile(Path(pins).read_text(), str(pins), 'exec'), {'True': True, 'False': False}, scope)
    digest = hashlib.sha256(Path(kernelcache).read_bytes()).hexdigest()
    kinds = [k for k, v in scope['IPSWS'].items() if v.get('derived', {}).get('kernelcache_sha256') == digest]
    if len(kinds) != 1:
        raise SystemExit('{}: no pinned IPSW kind has this kernelcache'.format(kernelcache))
    return kinds[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--kind')
    parser.add_argument('--pins', type=Path, default=HERE.parents[1] / 'ipsw/pins.bzl')
    parser.add_argument('--kernelcache', required=True, type=Path)
    parser.add_argument('--ipsw', required=True)
    parser.add_argument('--class', dest='classes', action='append')
    workspace = os.environ.get('BUILD_WORKSPACE_DIRECTORY')
    parser.add_argument(
        '--tables', type=Path, default=Path(workspace) / 'kits/SwitchABI/kinds' if workspace else HERE / 'kinds'
    )
    parser.add_argument('--check', action='store_true', help='compare with the committed tables instead of writing')
    args = parser.parse_args()
    kind = kind_of(args.kernelcache, args.pins)
    if args.kind and args.kind != kind:
        raise SystemExit('the kernelcache is {}, not {}: build with that kind (--config)'.format(kind, args.kind))
    args.kind = kind
    directory = args.tables / args.kind
    classes = args.classes or sorted(p.stem for p in directory.glob('*.json'))
    if not is_fileset(args.kernelcache):
        for cls in classes:
            check_plain(
                args.kernelcache,
                json.loads((HERE / 'interface' / (cls + '.json')).read_text()),
                json.loads((directory / (cls + '.json')).read_text()),
            )
        return
    fs = kernelcache.Fileset(args.kernelcache, args.ipsw)
    pure = pure_virtual(fs)
    stale = []
    for cls in classes:
        path = directory / (cls + '.json')
        table = json.loads(path.read_text())
        text = extract(fs, json.loads((HERE / 'interface' / (cls + '.json')).read_text()), table, args.kind, pure)
        if args.check:
            if path.read_text() != text:
                stale.append(cls)
        else:
            path.write_text(text)
    if stale:
        raise SystemExit('kinds/{}: differs from the kernelcache: {}'.format(args.kind, ', '.join(stale)))


if __name__ == '__main__':
    main()
