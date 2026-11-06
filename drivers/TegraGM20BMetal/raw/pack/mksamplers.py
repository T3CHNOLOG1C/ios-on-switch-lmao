#!/usr/bin/env python3
"""mksamplers (r8-rawca): NVK's sampler header for each sampler of a PROBE_SAMPLERS run (probe.mm): one line per sampler,
"<i> <minFilter> <magFilter> <mipFilter> <s> <t> <r> <maxAnisotropy> <lod> <8 header words>".
usage: mksamplers.py <probe.out> <probe.log>"""

import sys

out = [l.split() for l in open(sys.argv[1]) if l.startswith('SAMPLER ')]
subs, cur = [], []
for l in open(sys.argv[2]):
    if l.startswith('SUBMIT'):
        subs.append(cur)
        cur = []
    else:
        cur.append(l.rstrip('\n'))
draws = [s for s in subs if any(x.startswith('DRAW') for x in s)][-len(out) :]
known = {'26092 291 fff000'}  # the neutral sampler m2v binds for an unbound slot (none here)
for f, s in zip(out, draws):
    tscs = {}
    for x in s:
        if x.startswith('P tsc '):
            t = x.split()
            tscs[int(t[2])] = [int(w, 16) for w in t[3:11]]
    # the handles of set 0 that name a sampler slot of the state all name ours: the TSC index most of them use
    words = None
    set0 = next((x.split()[3:] for x in s if x.startswith('P set0 ')), [])
    counts = {}
    for w in set0:
        v = int(w, 16)
        if v and (v >> 20) in tscs and (v & 0xFFFFF):
            counts[v >> 20] = counts.get(v >> 20, 0) + 1
    if counts:
        words = tscs[max(counts, key=counts.get)]
    print(' '.join(f[1:10]), ' '.join('%08x' % w for w in words) if words else 'none')
