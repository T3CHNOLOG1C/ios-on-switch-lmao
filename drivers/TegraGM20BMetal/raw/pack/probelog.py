#!/usr/bin/env python3
"""Read capture/probe.out + probe.log (nvdec NVDEC_PROBE records) into per-probe dicts."""


def fnv(b: bytes, h: int = 0xCBF29CE484222325) -> int:
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def load(out_path: str, log_path: str, init_submits: int = 4) -> list[dict]:
    probes = []
    with open(out_path) as out:
        if any(line.startswith('WARMUP') for line in out):
            init_submits += 1
    with open(out_path) as out:
        for line in out:
            if line.startswith('PROBE '):
                _, i, key = line.rstrip('\n').split(' ', 2)
                probes.append({'index': int(i), 'key': key.strip()})
    subs, cur = [], []
    with open(log_path) as log:
        for text in log:
            line = text.rstrip('\n')
            cur.append(line)
            if line.startswith('SUBMIT'):
                subs.append(cur)
                cur = []
    for n, p in enumerate(probes):
        s = subs[init_submits + n]
        p['draws'] = sum(1 for line in s if line.startswith('DRAW'))
        p['dispatches'] = sum(1 for line in s if line.startswith('DISPATCH'))
        p['clears'] = [line for line in s if line.startswith('CLEAR')]
        p['canon'] = s
        recs, rec = [], None
        for line in s:
            if line == 'P begin':
                rec = {'binds': {}, 'ptr': {}, 'tic': {}, 'tsc': {}, 'vb': {}, 'vbinfo': {}, 'prog': {}}
            elif line == 'P end':
                recs.append(rec)
                rec = None
            elif rec is not None and line.startswith('P '):
                f = line.split()
                t = f[1]
                if t == 'regs':
                    rec['regs'] = {int(a, 16): int(b, 16) for a, b in (x.split('=') for x in f[2:])}
                elif t == 'bind':
                    rec['binds'][(int(f[2]), int(f[3]))] = (int(f[4], 16), int(f[5], 16))
                elif t in ('root', 'set0', 'set1'):
                    rec[t] = (int(f[2], 16), [int(x, 16) for x in f[3:]])
                elif t.startswith(('ptr', 'pushptr')):
                    rec['ptr'][t] = (int(f[2], 16), [int(x, 16) for x in f[3:]])
                elif t == 'tic':
                    rec['tic'][int(f[2])] = (f[3], [int(x, 16) for x in f[4:]])
                elif t == 'tsc':
                    rec['tsc'][int(f[2])] = [int(x, 16) for x in f[3:]]
                elif t == 'vbinfo':
                    rec['vbinfo'][int(f[2])] = (int(f[3], 16), int(f[4], 16), int(f[5], 16))
                elif t.startswith('vb'):
                    rec['vb'][int(t[2:])] = (int(f[2], 16), [int(x, 16) for x in f[3:]])
                elif t == 'prog':
                    rec['prog'][int(f[2])] = (f[3], int(f[4], 16), f[5])
                elif t == 'draw':
                    rec['draw'] = (int(f[2]), int(f[3]), int(f[4], 16))
                elif t == 'pushw':
                    rec['pushw'] = f[2:]
        p['records'] = recs
    return probes
