#!/usr/bin/env python3
"""Compare two corpus runs (before, after) per framework.

Counted are the entry points (kernels, vertex and fragment functions) that compile and make their pipeline on the host
NVK, where an entry point passes if any constant variant ("z" all zero, "1" all one) did. Failures are put in three
buckets: real gaps, harness limits, and features the A10 (iPad7,1, Apple3 family) does not have.
usage: report.py <before run dir> <after run dir> [--ms]   -> TSV on stdout
"""

import collections
import contextlib
import re
import sys
from pathlib import Path

A10_NA = [
    r'RayTracing|acceleration_structure|air\.intersection',
    r'StencilExport',
    r'TileShading|imageblock|Imageblock|TileAttachments',
    r'AgxUnmodeled',
    r'Barycentrics',
    r'MetalTools/|GPUCompiler/libtracepoint',
    r'AMDConvolution|AMD_N|A14\b|WinogradAMD|ConvolutionA14',
    r'CoreOCModules/|CorePhotogrammetry/',
    r'UnknownIntrinsic.*(imageblock|tile)',
    r'CoreRE/.*UnknownIntrinsic',
]
HARNESS = [
    r'NeedsInput\(LinkedFunction',
    r'NeedsInput\(VisibleFunctionTable',
    r'NeedsInput\(StageInputDescriptor',
    r'InvalidDescriptor',
    r'^pair',
    r'PointSizeTopology',
    r'Linking, needs',
]


def load(run: str) -> list[list[str]]:
    rows = []
    for f in sorted(Path(run).glob('*.tsv')):
        if f.name in ('summary.tsv', 'categories.tsv'):
            continue
        for line in f.read_text(errors='replace').splitlines():
            p = line.split('\t')
            if len(p) >= 8:
                rows.append(p[:8])
    return rows


def framework(tag: str) -> str:
    t = tag.split('/', maxsplit=1)[0]
    return 'dsc:' + tag.split('/')[1].split('@', maxsplit=1)[0] if t == 'dsc' else t


def bucket(tag: str, name: str, phase: str, detail: str) -> str:
    s = f'{tag}/{name} {phase} {detail} '
    if phase == 'container':
        return 'not AIR'
    if any(re.search(r, s) for r in A10_NA):
        return 'not on A10'
    if any(re.search(r, phase + ' ' + detail) for r in HARNESS):
        return 'harness'
    return 'gap'


def summarize(
    rows: list[list[str]],
) -> tuple[collections.OrderedDict[tuple[str, str], dict], collections.OrderedDict[str, collections.Counter[str]]]:
    entries = collections.OrderedDict()  # (tag, name) -> [results]
    for tag, name, stage, variant, result, phase, detail, ms in rows:
        if result == 'SKIP' or name == '-':
            continue
        e = entries.setdefault((tag, name), {'ok': False, 'z': None, 'why': None, 'ms': []})
        if result == 'OK':
            e['ok'] = True
            with contextlib.suppress(ValueError):
                e['ms'].append(int(ms))
        elif e['why'] is None:
            e['why'] = (phase, detail)
        if variant in ('z', '?'):
            e['z'] = result == 'OK'
    per = collections.OrderedDict()
    for (tag, name), e in entries.items():
        c = per.setdefault(framework(tag), collections.Counter())
        c['entry'] += 1
        c['ok'] += e['ok']
        c['z'] += bool(e['z'])
        if not e['ok']:
            c[bucket(tag, name, *e['why'])] += 1
    return entries, per


before, after = load(sys.argv[1]), load(sys.argv[2])
_, pb = summarize(before)
ea, pa = summarize(after)
print('framework\tentry points\tpass before\tpass after\tafter %\tz-variant after\tgap\tharness\tnot on A10')
tot_b, tot_a = collections.Counter(), collections.Counter()
for fw in sorted(set(pb) | set(pa), key=lambda f: -pa.get(f, {}).get('entry', 0)):
    b, a = pb.get(fw, collections.Counter()), pa.get(fw, collections.Counter())
    tot_b.update(b)
    tot_a.update(a)
    print(
        f'{fw}\t{a["entry"]}\t{b["ok"]}\t{a["ok"]}\t{100.0 * a["ok"] / max(1, a["entry"]):.1f}\t{a["z"]}'
        f'\t{a["gap"]}\t{a["harness"]}\t{a["not on A10"]}'
    )
print(
    f'TOTAL\t{tot_a["entry"]}\t{tot_b["ok"]}\t{tot_a["ok"]}\t{100.0 * tot_a["ok"] / max(1, tot_a["entry"]):.1f}'
    f'\t{tot_a["z"]}\t{tot_a["gap"]}\t{tot_a["harness"]}\t{tot_a["not on A10"]}'
)
reachable = tot_a['entry'] - tot_a['not on A10'] - tot_a['harness']
print(
    f'reachable on A10, harness-supplied inputs aside: {reachable}, pass {tot_a["ok"]} '
    f'({100.0 * tot_a["ok"] / max(1, reachable):.1f}%)'
)
if '--ms' in sys.argv:
    for fw in ('QuartzCore',):
        ms = sorted(m for (t, n), e in ea.items() if framework(t) == fw for m in e['ms'])
    allms = sorted(m for e in ea.values() for m in e['ms'])

    def q(xs: list[int], p: float) -> int:
        return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else 0

    print(
        f'compile+make ms (host): all p50 {q(allms, 0.5)} p99 {q(allms, 0.99)} max {allms[-1] if allms else 0}; '
        f'QuartzCore p50 {q(ms, 0.5)} p99 {q(ms, 0.99)} max {ms[-1] if ms else 0}'
    )
    gaps = collections.Counter()
    for (t, n), e in ea.items():
        if not e['ok'] and bucket(t, n, *e['why']) == 'gap':
            d = re.sub(r'location: Some\(.*?\)\)?,', '', e['why'][1])
            d = re.sub(r'^with [^:]*: ', '', d)
            gaps[re.sub(r'\d+', 'N', d[:110])] += 1
    for g, n in gaps.most_common():
        print(f'gap\t{n}\t{g}')
