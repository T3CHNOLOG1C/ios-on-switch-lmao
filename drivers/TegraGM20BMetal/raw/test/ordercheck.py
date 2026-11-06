#!/usr/bin/env python3
"""ordercheck (r8-rawca): reads the mock GPU's log (mock_iokit.c MOCK_GPU_LOG) of an orderframe run: the raw channel is the last
one made (the facade opens after m2v), the rest are NVK's. Every submission must start after every submission of the other
driver kicked before it ended (the commit order, kept on the GPU), and the raw submit thread must have made no WaitFence call.
usage: ordercheck.py <log> <begin_us>"""

import sys

runs, stats = [], {}
for line in open(sys.argv[1]):
    w = line.split()
    if w[0] == 'run':
        runs.append(dict(ch=int(w[1]), seq=int(w[2]), kick=int(w[3]), start=int(w[4]), end=int(w[5]), acq=int(w[6])))
    elif w[0] == 'order':
        stats['order'] = (int(w[2]), int(w[4]))
    elif w[0] == 'waitfence':
        stats['wait'] = (int(w[2]), int(w[4]))
begin = int(float(sys.argv[2]) * 1000)
rawch = max(r['ch'] for r in runs)
window = sorted((r for r in runs if r['kick'] >= begin), key=lambda r: r['kick'])
bad = 0
for i, r in enumerate(window):
    for p in window[:i]:
        if (p['ch'] == rawch) != (r['ch'] == rawch) and r['start'] < p['end']:
            bad += 1
            print(
                'ORDER BROKEN: ch %d #%d started at %d before ch %d #%d (kicked earlier) ended at %d'
                % (r['ch'], r['seq'], r['start'], p['ch'], p['seq'], p['end'])
            )
raw = [r for r in window if r['ch'] == rawch]
nvk = [r for r in window if r['ch'] != rawch]
print(
    'runs in window: raw %d (channel %d), nvk %d (channels %s); with acquires: %d; kicks within %.1f ms, GPU busy %.1f ms'
    % (
        len(raw),
        rawch,
        len(nvk),
        sorted({r['ch'] for r in nvk}),
        sum(1 for r in window if r['acq']),
        (window[-1]['kick'] - window[0]['kick']) / 1000,
        (window[-1]['end'] - window[0]['start']) / 1000,
    )
)
print(
    'order calls %d, acquires queued %d; WaitFence calls %d, from the raw submit thread %d'
    % (stats['order'][0], stats['order'][1], stats['wait'][0], stats['wait'][1])
)
print('ordering', 'OK' if not bad else 'BROKEN (%d)' % bad)
sys.exit(1 if bad else 0)
