#!/usr/bin/env python3
"""The Metal libraries the shader cache precompile (precompile.mm) loads on the host. The seed build (tools/gpu/seed.py) lays
them out as the store precompile.mm reads:

  <store>/root/<device path>        a library the device loaded from a file (the journal's lib=<path>): the IPSW's volumes',
                                    crawled by //gpu/seed:crawl (tools/gpu/shaders/crawl.py)
  <store>/data/<sha256>.metallib    a library the device made from bytes (lib=data:<sha256>): any of the above or of the
                                    frontend's own (TwoPlaneStandIn.h) whose bytes they are, or one only the device made (Core
                                    Image's compiled kernels: @gpu_device_data, precompile/harvest.py --pack)
  <names.json>                      every function name of the pinned libraries -> their device paths (`names`, a build action)

usage: libraries.py pins <mounted OS volume>                               the `shasum -a 256` listing of every Metal library of
                                                                           the volume with a function (gpu/apple-libs.sha256)
       libraries.py names <root> <names.json> [<device path>...]           names.json of the libraries at <root>/<device path>
                                                                           (none given: every file below <root>)
       libraries.py resolve <names.json> <corpus.log> <out.log>            the corpus with lib=<path> added to the stages of journal
                                                                           lines from before the journal named libraries (no lib=)
                                                                           whose function names are in exactly one library (QuartzCore's
                                                                           names stay QuartzCore's, the precompile's default; a line
                                                                           whose names are in two or three is replayed in each), and
                                                                           the coverage table: per library, lines named and resolved

A journal line names its library from the frontend that writes lib= on; an older line's library is only guessed by a unique
function name, and the table keeps the two apart. A wrong guess costs nothing on the device: m2v's keys hash the function's AIR,
so a translation of another library's function is never found there.
"""

import hashlib
import json
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

QC = '/System/Library/Frameworks/QuartzCore.framework/default.metallib'


def names(data):
    """The function names of a metallib's bytes (the first MTLB slice of a fat one), or None when it is not one."""
    if data[:4] == b'\xca\xfe\xba\xbe':
        (count,) = struct.unpack_from('>I', data, 4)
        for i in range(count):
            _, _, offset, size, _ = struct.unpack_from('>IIIII', data, 8 + 20 * i)
            if data[offset : offset + 4] == b'MTLB':
                return names(data[offset : offset + size])
        return None
    if data[:4] != b'MTLB' or len(data) < 0x58:
        return None
    offset, _ = struct.unpack_from('<QQ', data, 24)  # the function list: u32 count, then entries of u32 size and tags
    if offset + 4 > len(data):
        return None
    (count,) = struct.unpack_from('<I', data, offset)
    at, found = offset + 4, []
    for _ in range(count):
        if at + 4 > len(data):
            return None
        (size,) = struct.unpack_from('<I', data, at)
        tag, end = at + 4, at + size
        while tag + 6 <= end and data[tag : tag + 4] != b'ENDT':
            (length,) = struct.unpack_from('<H', data, tag + 4)
            if data[tag : tag + 4] == b'NAME':
                found.append(data[tag + 6 : tag + 6 + length].split(b'\0')[0].decode('utf-8', 'replace'))
            tag += 6 + length
        at = end
    return found


def pins(volume):
    """`<sha256>  <path>` of every *.metallib below `volume` that is a Metal library with a function, by path (LC_ALL=C)."""
    for path in sorted(str(p.relative_to(volume)) for p in Path(volume).rglob('*.metallib') if p.is_file()):
        data = (Path(volume) / path).read_bytes()
        if names(data):
            print('%s  %s' % (hashlib.sha256(data).hexdigest(), path))


def index(root, out, devices):
    """names.json: every function name of the libraries at <root>/<device path> -> the device paths of those that have it, each
    list in the order of the paths' components."""
    table = defaultdict(list)
    for device in sorted(('/' + d.lstrip('/') for d in devices), key=lambda d: Path(d).parts):
        found = names((Path(root) / device.lstrip('/')).read_bytes())
        if found is None:
            sys.exit('%s/%s is not a Metal library' % (root, device))
        for name in set(found):
            table[name].append(device)
    Path(out).write_text(json.dumps(dict(sorted(table.items())), indent=0) + '\n')
    print('%s: %d function names in %d libraries' % (out, len(table), len({p for v in table.values() for p in v})))


STAGE = re.compile(r'\| ([vfklp]) (\S+)( [^|]*?)?(?= \|)')


GUESSES = (
    3  # a name in up to this many libraries is replayed in each of them (seed space only: m2v's keys hash the AIR)
)


def resolve(index_json, corpus, out):
    table = json.loads(Path(index_json).read_text())
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
            how = (
                'qc'
                if common == {None}
                else 'resolved'
                if len(common) == 1
                else 'guessed'
                if 1 < len(common) <= GUESSES
                else 'ambiguous'
                if any(open_)
                else 'unknown'
            )
            choices = sorted(common, key=lambda c: c or '') if how in ('qc', 'resolved', 'guessed') else [None]
        for choice in choices:
            text = line
            for m, lib, _ in reversed(stages):  # from the end, so earlier offsets hold
                if lib or not choice:
                    continue
                if kind == 'S':
                    text = text[: m.start(3) - 5] + 'lib=%s ' % choice + text[m.start(3) - 5 :]
                else:
                    text = text[: m.end(2)] + ' lib=' + choice + text[m.end(2) :]
            lines.append(text)
            first = next((lib for _, lib, _ in stages if lib), None) or choice or QC
            rows[first if how in ('qc', 'journaled', 'resolved', 'guessed') else how][(kind, how)] += 1
    Path(out).write_text('\n'.join(lines) + '\n')
    print(
        '%-80s %s'
        % (
            'library (lines: qc = QuartzCore by default, journaled, resolved by a unique name, guessed: in each '
            'of up to %d)' % GUESSES,
            '',
        )
    )
    for lib, counts in sorted(rows.items(), key=lambda kv: -sum(kv[1].values())):
        print('  %-78s %s' % (lib, ' '.join('%s.%s=%d' % (k, h, n) for (k, h), n in sorted(counts.items()))))


def main():
    verb, rest = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else (None, [])
    if verb == 'pins' and len(rest) == 1:
        pins(rest[0])
    elif verb == 'names' and len(rest) >= 2:
        index(
            rest[0], rest[1], rest[2:] or [str(p.relative_to(rest[0])) for p in Path(rest[0]).rglob('*') if p.is_file()]
        )
    elif verb == 'resolve' and len(rest) == 3:
        resolve(*rest)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
