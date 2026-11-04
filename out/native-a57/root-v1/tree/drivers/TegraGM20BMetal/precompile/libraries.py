#!/usr/bin/env python3
"""The Metal libraries the shader cache precompile (precompile.mm) loads on the host, kept in a store outside the tree (Apple's
binaries; default out/gpu-artifacts/m2v-libs):

  <store>/root/<device path>        a library the device loaded from a file (the journal's lib=<path>): the 21H461 OS volume's
                                    or OS cryptex's, or an app bundle's from the Data volume
  <store>/data/<sha256>.metallib    a library the device made from bytes (lib=data:<sha256>: Core Image's compiled source and
                                    stitched kernels, an app's own data), which the frontend dumps to <cache>/libs/ and
                                    harvest.py collects
  <store>/names.json                every function name of the OS volume's and cryptex's libraries -> their device paths

usage: libraries.py <store> index <OS volume mount> <OS cryptex mount> [<Data volume mount>]
                                                                           names.json and every library of the volumes, copied into
                                                                           root/ (the Data volume's: its app bundles', read only)
       libraries.py <store> resolve <corpus.log> <out.log>                 the corpus with lib=<path> added to the stages of journal
                                                                           lines from before the journal named libraries (no lib=)
                                                                           whose function names are in exactly one library (QuartzCore's
                                                                           names stay QuartzCore's, the precompile's default; a line
                                                                           whose names are in two or three is replayed in each), and
                                                                           the coverage table: per library, lines named and resolved

A journal line names its library from the frontend that writes lib= on; an older line's library is only guessed by a unique
function name, and the table keeps the two apart. A wrong guess costs nothing on the device: m2v's keys hash the function's AIR,
so a translation of another library's function is never found there.
"""
import json
import re
import shutil
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

QC = '/System/Library/Frameworks/QuartzCore.framework/default.metallib'


def names(data):
    """The function names of a metallib's bytes (the first MTLB slice of a fat one), or None when it is not one."""
    if data[:4] == b'\xca\xfe\xba\xbe':
        count, = struct.unpack_from('>I', data, 4)
        for i in range(count):
            _, _, offset, size, _ = struct.unpack_from('>IIIII', data, 8 + 20 * i)
            if data[offset:offset + 4] == b'MTLB':
                return names(data[offset:offset + size])
        return None
    if data[:4] != b'MTLB' or len(data) < 0x58:
        return None
    offset, _ = struct.unpack_from('<QQ', data, 24)   # the function list: u32 count, then entries of u32 size and tags
    if offset + 4 > len(data):
        return None
    count, = struct.unpack_from('<I', data, offset)
    at, found = offset + 4, []
    for _ in range(count):
        if at + 4 > len(data):
            return None
        size, = struct.unpack_from('<I', data, at)
        tag, end = at + 4, at + size
        while tag + 6 <= end and data[tag:tag + 4] != b'ENDT':
            length, = struct.unpack_from('<H', data, tag + 4)
            if data[tag:tag + 4] == b'NAME':
                found.append(data[tag + 6:tag + 6 + length].split(b'\0')[0].decode('utf-8', 'replace'))
            tag += 6 + length
        at = end
    return found


def index(store, os_volume, cryptex, data=None):
    table = defaultdict(list)
    copied = 0
    volumes = [(Path(os_volume), '', '.'), (Path(cryptex), '', '.')]
    if data:   # /private/var on the device; the installed apps' bundles alone
        volumes.append((Path(data), '/private/var', 'containers/Bundle/Application'))
    for mount, prefix, within in volumes:
        for path in sorted((mount / within).rglob('*.metallib')):
            device = prefix + '/' + str(path.relative_to(mount))
            data = path.read_bytes()
            found = names(data)
            if found is None:
                continue
            target = store / 'root' / device.lstrip('/')
            if not target.is_file() or target.stat().st_size != len(data):
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                copied += 1
            for name in set(found):
                table[name].append(device)
    (store / 'names.json').write_text(json.dumps(dict(sorted(table.items())), indent=0) + '\n')
    print('%s: %d function names in %d libraries (%d copied)' %
          (store, len(table), len({p for v in table.values() for p in v}), copied))


STAGE = re.compile(r'\| ([vfklp]) (\S+)( [^|]*?)?(?= \|)')


GUESSES = 3   # a name in up to this many libraries is replayed in each of them (seed space only: m2v's keys hash the AIR)


def resolve(store, corpus, out):
    table = json.loads((store / 'names.json').read_text())
    qc = set(n for n, paths in table.items() if QC in paths)
    lines, rows = [], defaultdict(Counter)

    def candidates(name):
        """The libraries a function from before the journal named libraries may be from: QuartzCore's (None, the default)
        when it has the name, else every library holding it."""
        return [None] if name in qc else table.get(name, [])

    for line in Path(corpus).read_text().splitlines():
        kind = line[:1]
        if kind not in 'SRC':
            lines.append(line)
            continue
        # Each stage: (its text, the journal's library, or the candidates of its name). An S line is one stage.
        stages = []
        if kind == 'S':
            m = re.match(r'S ok=1 (lib=(\S+) )?name=(\S+) ', line)
            if m:
                stages.append((m, m[2], None if m[2] else candidates(m[3])))
        else:
            for m in STAGE.finditer(line):
                lib = next((t[4:] for t in (m[3] or '').split() if t.startswith('lib=')), None)
                if m[2] != '-':
                    stages.append((m, lib, None if lib else candidates(m[2])))
        open_ = [c for _, lib, c in stages if not lib]
        if not open_:
            how, choices = ('journaled' if stages else 'unknown'), [None]
        else:
            # The stages' functions are taken to come from one library: the libraries every open stage's name is in.
            common = set(open_[0]).intersection(*open_[1:])
            how = 'qc' if common == {None} else 'resolved' if len(common) == 1 else \
                'guessed' if 1 < len(common) <= GUESSES else 'ambiguous' if any(open_) else 'unknown'
            choices = sorted(common, key=lambda c: c or '') if how in ('qc', 'resolved', 'guessed') else [None]
        for choice in choices:
            text = line
            for m, lib, _ in reversed(stages):   # from the end, so earlier offsets hold
                if lib or not choice:
                    continue
                if kind == 'S':
                    text = text[:m.start(3) - 5] + 'lib=%s ' % choice + text[m.start(3) - 5:]
                else:
                    text = text[:m.end(2)] + ' lib=' + choice + text[m.end(2):]
            lines.append(text)
            first = next((lib for _, lib, _ in stages if lib), None) or choice or QC
            rows[first if how in ('qc', 'journaled', 'resolved', 'guessed') else how][(kind, how)] += 1
    Path(out).write_text('\n'.join(lines) + '\n')
    print('%-80s %s' % ('library (lines: qc = QuartzCore by default, journaled, resolved by a unique name, guessed: in each '
                        'of up to %d)' % GUESSES, ''))
    for lib, counts in sorted(rows.items(), key=lambda kv: -sum(kv[1].values())):
        print('  %-78s %s' % (lib, ' '.join('%s.%s=%d' % (k, h, n) for (k, h), n in sorted(counts.items()))))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    store, verb, rest = Path(sys.argv[1]), sys.argv[2], sys.argv[3:]
    store.mkdir(parents=True, exist_ok=True)
    if verb == 'index' and len(rest) in (2, 3):
        index(store, *rest)
    elif verb == 'resolve' and len(rest) == 2:
        resolve(store, *rest)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
