#!/usr/bin/env python3
"""Summarize a corpus run: per framework (library tag) the entry points tried and passed, and every failure by category.
usage: categorize.py <run dir> [--rows]  -> prints a table; writes <run dir>/summary.tsv and categories.tsv"""
import re, sys, collections
from pathlib import Path
run = Path(sys.argv[1])
rows = []
for f in sorted(run.glob('*.tsv')):
    if f.name in ('summary.tsv', 'categories.tsv'): continue
    for line in f.read_text(errors='replace').splitlines():
        p = line.split('\t')
        if len(p) >= 8: rows.append(p)

RULES = [  # (category, regex on phase+detail)
    ('crash (NVK/NAK abort)', r'^crash'),
    ('hang', r'^hang'),
    ('ray tracing (A10 has none)', r'RayTracing|air\.intersection|acceleration_structure|Intersection\)'),
    ('framebuffer fetch / raster order (no ROAA/interlock on Maxwell)', r'FramebufferFetch|RasterOrderGroups'),
    ('Core Image kernel ABI (air.ci) / stitchable', r'_ci_|ci_kernel|air\.ci\b|CoreImage'),
    ('linked/visible functions not supplied (harness)', r'NeedsInput\(LinkedFunction|NeedsInput\(VisibleFunctionTable|IntersectionFunctionTable'),
    ('compute stage-in descriptor not supplied (harness)', r'NeedsInput\(StageInputDescriptor'),
    ('sampler lowering (not implemented)', r'Lowering\(Sampler\)'),
    ('texture lowering (not implemented)', r'Lowering\(Texture\)'),
    ('imageblock / tile shading', r'imageblock|TileShading|Imageblock|TileAttachments'),
    ('stencil export', r'StencilExport'),
    ('argument buffer objects', r'ArgumentBufferObjects|ArgumentBufferPointers'),
    ('static initializer', r'StaticInitializer'),
    ('bitcode record unsupported', r'ModuleRecord'),
    ('AIR metadata key unknown', r'UnknownKey'),
    ('unknown intrinsic', r'UnknownIntrinsic'),
    ('AGX-only (unmodeled)', r'AgxUnmodeled'),
    ('descriptor guessed wrong (harness)', r'InvalidDescriptor|PointSizeTopology|^pair'),
    ('VkPipeline creation failed (NVK)', r'PipelineCreation|vkpipeline|VK_ERROR'),
    ('not AIR (AGX binary archive / runtime library)', r'^container'),
]
def category(phase, detail):
    s = phase + ' ' + detail
    for name, rx in RULES:
        if re.search(rx, s): return name
    return 'other: ' + re.sub(r'\d+', 'N', detail[:90])

per = collections.OrderedDict()
cats = collections.Counter(); catlibs = collections.defaultdict(set); examples = {}
for tag, name, stage, variant, result, phase, detail, ms in (r[:8] for r in rows):
    fw = tag
    e = per.setdefault(fw, collections.Counter())
    if result == 'SKIP':
        e['library functions'] += 1; continue
    if variant not in ('z', '-', '?'):
        e['variants tried'] += 1
        e['variants ok'] += result == 'OK'
        if result == 'OK': continue
    else:
        e['entry points'] += 1
        e['ok'] += result == 'OK'
    if result == 'ERR':
        c = category(phase, detail)
        cats[c] += 1; catlibs[c].add(tag); examples.setdefault(c, '%s %s: %s' % (tag, name, detail[:200]))
tot = collections.Counter()
lines = ['framework\tentry points\tok\tpass%\tvariants\tvariants ok\tlibrary functions']
for fw, e in per.items():
    tot.update(e)
    n = e['entry points']
    lines.append('%s\t%d\t%d\t%s\t%d\t%d\t%d' % (fw, n, e['ok'], ('%.1f' % (100.0 * e['ok'] / n)) if n else '-', e['variants tried'], e['variants ok'], e['library functions']))
lines.append('TOTAL\t%d\t%d\t%.1f\t%d\t%d\t%d' % (tot['entry points'], tot['ok'], 100.0 * tot['ok'] / max(1, tot['entry points']), tot['variants tried'], tot['variants ok'], tot['library functions']))
(run / 'summary.tsv').write_text('\n'.join(lines) + '\n')
clines = ['category\tfailures\tlibraries\texample']
for c, n in cats.most_common():
    clines.append('%s\t%d\t%d\t%s' % (c, n, len(catlibs[c]), examples[c].replace('\t', ' ')))
(run / 'categories.tsv').write_text('\n'.join(clines) + '\n')
print(lines[-1]); print('\n'.join(clines))
