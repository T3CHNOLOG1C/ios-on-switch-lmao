#!/usr/bin/env python3
"""eqcheck (r7-raw): compare two nvdec logs of the same frames, draw by draw.

The logs (capture/cap.cpp RAWCAP_LOG, NVDEC_REGS=1) are one from the shipped m2v + NVK path and one from the raw path.
Submissions without draws or clears (context init, NVK's own setup) are skipped; then the i-th frame of one log is
compared with the i-th of the other.

A draw is the same GPU work when every field matches: the live 3D registers (R line), the render targets (by
first-appearance name), the vertex data, each stage's program and register count, and the bytes each program's constant
buffers hold (pointers replaced by what they point to, handles by their texture/sampler headers). Barriers are compared
as sets: the raw path must cover what NVK emitted before each draw that reads memory written earlier (WFI + TEXINV
before a framebuffer fetch).

usage: eqcheck.py <nvk.log> <raw.log> [--show N] [--flat] [--layouts] [--own]
"""

import collections
import re
import sys


def frames(path: str) -> list[list[dict]]:
    subs, cur = [], []
    with open(path) as log:
        for text in log:
            line = text.rstrip('\n')
            if line.startswith('SUBMIT'):
                subs.append(cur)
                cur = []
            elif line.startswith(('DRAW', 'CLEAR', '  ', 'END-OF-SUBMIT')) and not line.startswith(
                ('  STATE', 'P ')
            ):  # DRAW-INDEXED too
                cur.append(line)
    out = []
    for s in subs:
        events, ev = [], None
        for line in s:
            if line.startswith(('DRAW', 'CLEAR')):
                ev = {'head': line, 'lines': []}
                events.append(ev)
            elif line.startswith('  ') and ev is not None:
                ev['lines'].append(line.strip())
        if any(e['head'].startswith('DRAW') for e in events):
            out.append(events)
    # --flat: every submission's events as one stream (a command buffer the raw path submits in segments is several
    # submissions, each of its driver, where m2v makes one)
    if FLAT and out:
        out = [[e for events in out for e in events]]
    return out


def rename_all(out: list[list[dict]]) -> list[list[dict]]:
    # persistent resources renamed in order of first appearance in draws (clears of textures no draw uses, such as m2v's
    # initialisation of new textures, would otherwise shift every name)
    names = {}

    def rename(text: str, assign: bool) -> str:
        def sub(m: re.Match[str]) -> str:
            if m.group(0) not in names:
                if not assign:
                    return 'C' + m.group(0)[1:]
                names[m.group(0)] = f'N{len(names) + 1}'
            return names[m.group(0)]

        return re.sub(r'\bR\d+\b', sub, text)

    for events in out:
        for e in events:
            if e['head'].startswith('DRAW'):
                e['lines'] = [rename(line, True) for line in e['lines']]
    for events in out:
        for e in events:
            if e['head'].startswith('CLEAR'):
                e['head'] = rename(e['head'], False)
    return out


# --layouts: render targets and sampled images compared by what their texels are, not where or how they lie. The raw
# path renders a target NVK renders through a tiled shadow (rows not 128 apart) through a pitch pool image, and samples
# a surface m2v samples through a shadow image (rows not 32 apart) through a pitch pool image: the texels are the same,
# their headers' addresses, pitches and layouts are not. So: a texture header is its format word, width and height; a
# target its format; a target's address, pitch, memory layout and array registers are dropped; and the hash of a
# constant buffer that holds handles is not compared (its handles are, normalized; buffer contents keep their own
# hashes).
def layout_tic(m: re.Match[str]) -> str:
    w = [int(x, 16) for x in m.group(2).split()]
    return f'tic[{w[0]:08x} {w[4] & 0xFFFF:04x} {w[5] & 0xFFFF:04x}]'


def normalize_layouts(text: str) -> str:
    text = re.sub(r'tic\[(\S+) ([0-9a-f]{8}(?: [0-9a-f]{8}){7})\]', layout_tic, text)
    if 'handle{' in text:
        text = re.sub(r'(?<=[={])[0-9a-f]{1,16}:', '*:', text)
    return text


LAYOUT_REGS = {'%x' % (0x800 + 0x40 * j + k) for j in range(8) for k in (0x8, 0x14, 0x18, 0x1C)}


def fields(ev: dict) -> collections.OrderedDict:
    f = collections.OrderedDict()
    if LAYOUTS:
        ev = {
            'head': re.sub(r'rt=\w+,\d+x\d+,(fmt=\w+),mem=\w+', r'rt=\1', ev['head']),
            'lines': [
                re.sub(r'^(rt\d+)=\S+ \S+ (fmt=\w+).*$', r'\1=\2', normalize_layouts(line)) for line in ev['lines']
            ],
        }
    h = ev['head']
    if h.startswith('DRAW'):
        m = re.match(r'DRAW(?:-INDEXED)? first=(\d+) count=(\d+) begin=(\w+) barriers=(.*)', h)
        f['draw'] = f'{h.split()[0]} {m.group(1)} {m.group(2)} {m.group(3)}'
        f['barriers'] = {x for x in m.group(4).split(',') if x}
    else:
        m = re.match(r'CLEAR (.*) barriers=(.*)', h)
        f['clear'] = m.group(1)
        f['barriers'] = {x for x in m.group(2).split(',') if x}
    stage = None
    for line in ev['lines']:
        if line.startswith('R '):
            f['R'] = dict(x.split('=') for x in line[2:].split())
            if LAYOUTS:
                f['R'] = {k: v for k, v in f['R'].items() if k not in LAYOUT_REGS}
        elif line.startswith('regs='):
            f['regs'] = line[5:]
        elif line.startswith('stage'):
            stage = line.split()[0]
            f[stage] = line
        elif line.startswith('cb'):
            suffix = ' ' + line.split()[2].split('=')[0] if line.split()[1] == 'root' else ''
            f[f'{stage}.{line.split()[0]}{suffix}'] = line
        else:
            f[line.split('=')[0].split()[0]] = line
    return f


# TIC_POOL_C / TSC_POOL_C: the header pools' last index. The raw path sizes its pools for the render server's live
# textures (8192 texture headers, 1024 samplers; NVK's init: 1024 and 4096): a limit, not draw state. Not compared.
POOL_LIMITS = {'1564', '157c'}

FLAT = '--flat' in sys.argv
LAYOUTS = '--layouts' in sys.argv
# --own: m2v's own draws left out (as --layouts does) with every header and layout still compared: a frame whose m2v
# work draws passes of its own the raw path does without (m2v's copies into an image of every level, its mip generation)
OWN = LAYOUTS or '--own' in sys.argv


def main() -> int:
    show = int(sys.argv[sys.argv.index('--show') + 1]) if '--show' in sys.argv else 3
    a, b = frames(sys.argv[1]), frames(sys.argv[2])
    print(f'frames: nvk {len(a)} raw {len(b)}')
    if OWN:  # (before the names: what is left out names nothing)
        # each driver's own draws: programs no draw of the other log runs (m2v's shadow copies and mip work; the raw
        # path's 4:2:0 conversions through the pack's ycbcr states, where m2v converts with its own)
        def programs(e: dict) -> tuple[str, ...]:
            return tuple(line.split()[1] for line in e['lines'] if line.startswith('stage'))

        def drop(frames_of: list[list[dict]], other: list[list[dict]], who: str) -> None:
            theirs = {programs(e) for events in other for e in events if e['head'].startswith('DRAW')}
            dropped = 0
            for events in frames_of:
                keep = [e for e in events if not e['head'].startswith('DRAW') or programs(e) in theirs]
                dropped += len(events) - len(keep)
                events[:] = keep
            if dropped:
                print(f'{dropped} draws of programs only the {who} log runs left out (its own work)')

        theirs_b = [list(events) for events in b]
        drop(a, theirs_b, 'nvk')
        drop(b, a, 'raw')
    a, b = rename_all(a), rename_all(b)
    mism = collections.Counter()
    examples = collections.defaultdict(list)
    reg_diff = collections.Counter()
    bar_missing = collections.Counter()
    bar_extra = collections.Counter()
    draws = same = clears = 0
    for fi, (fa, fb) in enumerate(zip(a, b, strict=False)):
        da = [e for e in fa if e['head'].startswith('DRAW')]
        db = [e for e in fb if e['head'].startswith('DRAW')]
        ca = [fields(e)['clear'] for e in fa if e['head'].startswith('CLEAR')]
        cb = [fields(e)['clear'] for e in fb if e['head'].startswith('CLEAR')]
        if fi == 0:
            extra = [c for c in ca if c not in cb]
            if extra:
                textures = '; '.join(e.split(' color')[0] for e in extra)
                print(f'frame 0: {len(extra)} clears only in nvk (m2v initialises new textures: {textures})')
                ca = [c for c in ca if c in cb]
        if ca != cb:
            mism['clears'] += 1
            if len(examples['clears']) < show:
                examples['clears'].append(f'frame {fi}\n   nvk {ca}\n   raw {cb}')
        clears += len(ca)
        if len(da) != len(db):
            mism['draw count'] += 1
            examples['draw count'].append(f'frame {fi}: nvk {len(da)} raw {len(db)}')
        for di, (ea, eb) in enumerate(zip(da, db, strict=False)):
            draws += 1
            x, y = fields(ea), fields(eb)
            ok = True
            for k in sorted(set(x) | set(y)):
                if k in ('regs', 'barriers'):
                    continue
                if k == 'R':
                    ra, rb = x.get('R', {}), y.get('R', {})
                    d = sorted(m for m in set(ra) | set(rb) if ra.get(m) != rb.get(m) and m not in POOL_LIMITS)
                    if d:
                        ok = False
                        mism['registers'] += 1
                        for m in d:
                            reg_diff[m] += 1
                        if len(examples['registers']) < show:
                            regs = ' '.join(f'{m} nvk={ra.get(m, "-")} raw={rb.get(m, "-")}' for m in d[:12])
                            examples['registers'].append(f'frame {fi} draw {di}: {regs}')
                    continue
                if x.get(k) != y.get(k):
                    ok = False
                    mism[k] += 1
                    if len(examples[k]) < show:
                        examples[k].append(f'frame {fi} draw {di}\n   nvk {x.get(k)}\n   raw {y.get(k)}')
            missing = x['barriers'] - y['barriers']
            for m in missing:
                bar_missing[m] += 1
            for m in y['barriers'] - x['barriers']:
                bar_extra[m] += 1
            if 'TEXINV' in missing or 'WFI' in missing:
                ok = False
                mism['barrier (fetch/pass)'] += 1
                if len(examples['barrier (fetch/pass)']) < show:
                    examples['barrier (fetch/pass)'].append(
                        f'frame {fi} draw {di}: nvk {sorted(x["barriers"])} raw {sorted(y["barriers"])}'
                    )
            same += ok
    print(f'draws compared {draws}, identical {same}; clears compared {clears}')
    for k, n in mism.most_common():
        print(f'MISMATCH {k:<28} {n}')
        for e in examples[k]:
            print('  ' + e)
    if reg_diff:
        print('registers differing (draws):', ' '.join(f'{m}:{n}' for m, n in reg_diff.most_common(40)))
    if bar_missing:
        print('barriers nvk had, raw not (draws):', dict(bar_missing))
    if bar_extra:
        print('barriers raw had, nvk not (draws):', dict(bar_extra))
    return 0 if draws and same == draws and not mism else 1


if __name__ == '__main__':
    sys.exit(main())
