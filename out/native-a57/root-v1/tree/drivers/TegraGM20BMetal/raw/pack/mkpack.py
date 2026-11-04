#!/usr/bin/env python3
"""mkpack (r7-raw): the GM20B pipeline pack for the raw Metal path, read off the probe run (capture/probe.mm through the live
frontend + m2v + NVK twin with nvdec in NVDEC_PROBE + NVDEC_SCRUB + NVDEC_TRACE=3 mode).

For every corpus render pipeline state the raw path can take (one draw, no m2v-internal passes or dispatches):
  * its two programs: the 80-byte shader header + Maxwell code NAK made (from the NVK driver records), laid out in one shader
    heap image the way NVK uploads them pre-Turing (header at +0x30, code at +0x80 of a 0x100-aligned slot);
  * the 3D registers it sets (the registers that differ between states), as (method, value) pairs;
  * its root-table push words and its set-0 layout: which byte of the 256-byte descriptor set holds which Metal buffer slot's
    uniform descriptor, texture/sampler handle or framebuffer-fetch handle;
  * which Metal vertex buffer slot feeds vertex stream 0.
Plus what every state shares: the context init NVK records at queue creation (with the addresses to relocate marked) and the
registers every state's draws leave the same (the baseline).

usage: mkpack.py <probe.out> <probe.log> <programs.bin> <out dir> [<probe16 dir prefix>]
  the optional prefix names the PROBE_BYTES=16 and 4096 runs (<prefix>16.out/.log, <prefix>4096.out/.log): each buffer entry's
  descriptor type (a uniform buffer's range NVK rounds up to 256 bytes) and the function's window on the slot
writes <out>/pack.bin (runtime/raw_pack.h layout), <out>/pack.json, <out>/programs.txt (decoder tables), and prints a report.
"""
import hashlib, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probelog, layouts

MAGIC = b'GM20BPK1'
VERSION = 2   # 2: the header's stack key (rawpack.py --stack) after the section table

def fnv(b, h=0xcbf29ce484222325):
    for x in b:
        h = ((h ^ x) * 0x100000001b3) & 0xffffffffffffffff
    return h

# Registers the runtime owns (sets per pass, per draw, or once at init with its own addresses): never in a blob or the baseline.
OWNED = [(0x0000, 0x0200), (0x0800, 0x0a00), (0x0a00, 0x0a18), (0x0c00, 0x0c08), (0x0d6c, 0x0d90),
         (0x0e00, 0x0e0c), (0x0fe0, 0x0ffc), (0x1970, 0x1974), (0x1644, 0x1648), (0x1118, 0x111c), (0x1434, 0x143c), (0x10f8, 0x10fc), (0x121c, 0x1220), (0x1538, 0x153c), (0x19e0, 0x1a00),
         (0x1f00, 0x1f80), (0x1608, 0x1610), (0x1614, 0x161c), (0x1574, 0x1580), (0x155c, 0x1568), (0x0f84, 0x0f8c),
         (0x1b00, 0x1b10), (0x2380, 0x2600), (0x3400, 0x4000)]
def owned(m):
    if 0x1c00 <= m < 0x1d00 and (m & 0xf) in (4, 8):   # vertex stream locations
        return True
    return any(a <= m < b for a, b in OWNED)

# Init relocations: (subc, method) -> what the runtime puts there
RELOC = {(0, 0x1608): 'heap_hi', (0, 0x160c): 'heap_lo', (1, 0x1608): 'heap_hi', (1, 0x160c): 'heap_lo',
         (0, 0x0f84): 'zero_hi', (0, 0x0f88): 'zero_lo', (0, 0x3468): 'root_hi', (0, 0x346c): 'root_lo',
         (0, 0x3470): 'zero_hi', (0, 0x3474): 'zero_lo',
         (0, 0x1574): 'tic_hi', (0, 0x1578): 'tic_lo', (0, 0x157c): 'tic_max', (1, 0x1574): 'tic_hi', (1, 0x1578): 'tic_lo',
         (1, 0x157c): 'tic_max', (0, 0x155c): 'tsc_hi', (0, 0x1560): 'tsc_lo', (0, 0x1564): 'tsc_max', (1, 0x155c): 'tsc_hi',
         (1, 0x1560): 'tsc_lo', (1, 0x1564): 'tsc_max'}
RELOC_CODE = {'heap_hi': 1, 'heap_lo': 2, 'zero_hi': 3, 'zero_lo': 4, 'root_hi': 5, 'root_lo': 6, 'tic_hi': 7, 'tic_lo': 8,
              'tic_max': 9, 'tsc_hi': 10, 'tsc_lo': 11, 'tsc_max': 12}

def load_programs(path):
    b = open(path, 'rb').read()
    progs, at = {}, 0
    while at + 12 <= len(b):
        assert b[at:at + 4] == b'NVPG'
        code, count = struct.unpack_from('<II', b, at + 4)
        at += 12
        types = list(b[at:at + count]); at += count
        image = b[at:at + 80 + code]; at += 80 + code
        progs['%x' % fnv(image)] = {'image': image, 'cbufs': types}
    return progs

def main():
    out_path, log_path, prog_path, out_dir = sys.argv[1:5]
    sizes = {}   # (state index, set offset) -> (bound size with 16-byte payloads, with 4096-byte payloads)
    if len(sys.argv) > 5:
        for b in (16, 4096):
            for p in probelog.load('%s%d.out' % (sys.argv[5], b), '%s%d.log' % (sys.argv[5], b)):
                if p['draws'] != 1 or p['dispatches'] or not p['records'] or 'set0' not in p['records'][0]: continue
                w = p['records'][0]['set0'][1]
                for o in range(0, len(w) - 3, 4):
                    if 'ptr0.%d' % (o * 4) in p['records'][0]['ptr']:
                        sizes.setdefault((p['index'], o * 4), [0, 0])[0 if b == 16 else 1] = w[o + 2]
    os.makedirs(out_dir, exist_ok=True)
    P = probelog.load(out_path, log_path)
    programs = load_programs(prog_path)
    # init trace and post-init registers
    init, post = [], {0: {}, 1: {}}
    for l in open(log_path):
        if l.startswith('T '):
            _, sc, m, d = l.split()
            init.append((int(sc), int(m, 16), int(d, 16)))
        elif l.startswith('S regs '):
            f = l.split()
            post[int(f[2])] = {int(a, 16): int(v, 16) for a, v in (x.split('=') for x in f[3:])}
    # drop the query-pool setup (submit 4 is not traced) and NVK's own MME bookkeeping we relocate
    report = []
    simple, skipped = [], []
    for p in P:
        why = None
        if p['draws'] != 1: why = '%d draws' % p['draws']
        elif p['dispatches']: why = '%d dispatches' % p['dispatches']
        elif not p['records']: why = 'no record'
        else:
            r = p['records'][0]
            if any(v[2] != 'known' for v in r['prog'].values()): why = 'unknown program'
            elif sorted(r['prog']) != [1, 5]: why = 'stages %s' % sorted(r['prog'])
        # (three color attachments: the render server's since r7n's harvest, Core Animation's "A3" states with an R32Float third
        # attachment, taken as any other; test/frame.h FRAME_A3 draws them)
        if why: skipped.append((p, why))
        else: simple.append(p)
    # a color target's own registers (per-target blend 0x1e00 + 0x20 j, SET_CT_WRITE 0x1a00 + 4 j) mean nothing for a state
    # without target j: what the probe saw there is an earlier state's leftover. Not that state's, so neither in its blob nor in
    # deciding what varies (r8-rawca: two states with a third target made targets 2's blend "vary" for every other state)
    def targets(key):
        return sum(1 for t in key.split() if t[:1] == 'c' and t[1:2].isdigit() and '=' in t)
    def foreign(m, n):
        return (0x1e00 <= m < 0x1f00 and (m - 0x1e00) // 0x20 >= n) or (0x1a00 <= m < 0x1a20 and (m - 0x1a00) // 4 >= n)
    for p in simple:
        n = targets(p['key'])
        p['records'][0]['regs'] = {m: v for m, v in p['records'][0]['regs'].items() if not foreign(m, n)}
    # registers: varying across states -> per-pipeline blob; equal -> baseline (when it differs from post-init)
    allregs = set(post[0])
    for p in simple: allregs |= set(p['records'][0]['regs'])
    vals = {m: set(p['records'][0]['regs'].get(m, 0) for p in simple) for m in allregs if not owned(m)}
    varying = sorted(m for m, v in vals.items() if len(v) > 1)
    baseline = {m: next(iter(v)) for m, v in vals.items() if len(v) == 1 and next(iter(v)) != post[0].get(m, 0)}
    report.append('%d states probed, %d taken, %d skipped' % (len(P), len(simple), len(skipped)))
    for p, why in skipped: report.append('  skipped %d (%s): %s' % (p['index'], why, p['key'][:150]))
    report.append('%d per-pipeline registers: %s' % (len(varying), ' '.join('%x' % m for m in varying)))
    report.append('%d baseline registers over post-init' % len(baseline))
    # programs used, heap layout
    used = []
    for p in simple:
        for st in (1, 5):
            pid = p['records'][0]['prog'][st][0]
            if pid not in used: used.append(pid)
    heap, prog_index, prog_recs = bytearray(), {}, []
    for pid in used:
        pr = programs[pid]
        off = len(heap)
        slot = bytearray(0x80 - 0x50) + pr['image'][:80]          # header at +0x30
        slot += pr['image'][80:]                                   # code at +0x80
        slot += bytes((-len(slot)) % 0x100)
        heap += slot
        prog_index[pid] = len(prog_recs)
        if any(t not in (1, 3) for t in pr['cbufs']):
            report.append('  program %s has cbufs %s (only root + set 0 supported)' % (pid, pr['cbufs']))
        prog_recs.append({'id': pid, 'heap_offset': off, 'program': off + 0x30, 'bytes': len(slot), 'code': len(pr['image']) - 80,
                          'cbufs': pr['cbufs']})
    report.append('%d programs, shader heap image %d KiB' % (len(prog_recs), len(heap) // 1024))
    # pipelines
    pipes, constexpr_tsc = [], []
    for p in simple:
        r = p['records'][0]
        ents, problems = layouts.layout(p)
        rec = {'index': p['index'], 'key': p['key'], 'vs': prog_index[r['prog'][1][0]], 'fs': prog_index[r['prog'][5][0]]}
        rec['blob'] = [(m, r['regs'].get(m, 0)) for m in varying if m not in (0x2044, 0x2144) and not foreign(m, targets(p['key']))]
        root = r['root'][1]
        rec['push'] = [(int(o, 16), int(x, 16)) for o, x in (w.split('=') for w in r.get('pushw', []))]
        rec['set_size'] = r['root'][1][0x38 // 4 + 2] if 'set0' in r else 0
        entries = []
        for e in ents:
            if e[1] == 'ubo':
                s16, s4k = sizes.get((p['index'], e[0]), (256, 256))
                # a 16-byte payload bound at 256: a uniform buffer (NVK rounds its range to the cbuf alignment); else storage.
                # The 4096-byte one's range: the window (0: the whole payload)
                ssbo = s16 < 256
                window = 0 if s4k >= 4096 else s4k
                entries.append({'off': e[0], 'kind': 'ubo', 'stage': e[2], 'slot': e[3], 'size': e[4], 'ssbo': ssbo, 'window': window})
            else:
                tk, sk = e[2], e[3]
                ent = {'off': e[0], 'kind': 'handle'}
                if tk is None: ent['tex'] = None
                elif tk[0] == 'tex': ent['tex'] = ('slot', tk[1])
                elif tk[0] == 'fetch': ent['tex'] = ('fetch', tk[1])
                else: problems.append('texture %s' % (tk,))
                if sk is None: ent['smp'] = None
                elif sk[0] == 'smp': ent['smp'] = ('slot', sk[1])
                elif sk[0] == 'tsc?derived': problems.append('a sampler m2v derives from a bound one (YCbCr conversion)')
                else:
                    # a sampler the probe did not bind: the function's constexpr sampler (made by m2v as an immutable one)
                    words = r['tsc'][e[4] >> 20]
                    if words not in constexpr_tsc: constexpr_tsc.append(words)
                    ent['smp'] = ('constexpr', constexpr_tsc.index(words))
                entries.append(ent)
        rec['entries'] = entries
        vbs = []
        for j, (loc, lim, fmt) in sorted(r['vbinfo'].items()):
            w = r['vb'][j][1]
            if (w[0] >> 16) != 0xc0de or (w[0] >> 8) & 0xff != 1: problems.append('vertex stream %d not a vertex slot' % j)
            vbs.append({'stream': j, 'slot': w[0] & 0xff, 'stride': fmt & 0xfff})
        rec['vbs'] = vbs
        rec['fetch'] = any(e.get('tex', [None])[0] == 'fetch' for e in entries if e['kind'] == 'handle')
        rec['clears'] = len(p['clears'])
        rec['problems'] = problems
        if problems:   # m2v draws it (RawMetal.mm: a state not in the pack goes there)
            report.append('  skipped %d (%s): %s' % (p['index'], '; '.join(problems), p['key'][:150]))
            continue
        pipes.append(rec)
    # init template with relocations (submissions 1-3 as NVK recorded them)
    init_recs = []
    for sc, m, d in init:
        init_recs.append((sc, m, d, RELOC_CODE.get(RELOC.get((sc, m)), 0)))
    report.append('init template: %d methods, %d relocated' % (len(init_recs), sum(1 for x in init_recs if x[3])))
    # --- pack.bin ---
    strings = bytearray()
    def string(s):
        off = len(strings); strings.extend(s.encode() + b'\0'); return off
    blob = []
    pipe_bin = bytearray()
    for rec in pipes:
        blob_off = len(blob)
        for m, v in rec['blob']: blob += [m, v]
        ents = bytearray()
        for e in rec['entries']:
            if e['kind'] == 'ubo':
                ents += struct.pack('<HBBBBH', e['off'], 1, 1 if e['stage'] == 'v' else 2, e['slot'], 1 if e.get('ssbo') else 0,
                                    e.get('window', 256) // 16)
            else:
                tk = e['tex']; sk = e['smp']
                tkind = 0 if tk is None else (2 if tk[0] == 'slot' else 3)
                skind = 0 if sk is None else (1 if sk[0] == 'slot' else 2)
                ents += struct.pack('<HBBBBBB', e['off'], tkind, 2, 0 if tk is None else tk[1], skind, 0 if sk is None else sk[1], 0)
        ents += bytes(8 * (16 - len(rec['entries'])))
        mask, words = 0, [0] * 32
        for o, val in rec['push']:
            mask |= 1 << ((o - 0x700) // 4); words[(o - 0x700) // 4] = val
        push = struct.pack('<I32I', mask, *words)
        vb = rec['vbs'][0] if rec['vbs'] else {'slot': 0xff, 'stride': 0}
        keyhash = fnv(rec['key'].encode())
        flags = (1 if rec['fetch'] else 0)
        pipe_bin += struct.pack('<QIHHIIBBBBHHI', keyhash, string(rec['key']), rec['vs'], rec['fs'], blob_off, len(rec['blob']) * 2,
                                len(rec['entries']), len(rec['push']), vb['slot'], 0, vb['stride'], rec['set_size'], flags)
        pipe_bin += ents + push
    prog_bin = bytearray()
    for pr in prog_recs:
        prog_bin += struct.pack('<IIII', pr['heap_offset'], pr['program'], pr['bytes'], len(pr['cbufs']))
    tsc_bin = b''.join(struct.pack('<8I', *w) for w in constexpr_tsc)
    base_bin = b''.join(struct.pack('<II', m, v) for m, v in sorted(baseline.items()))
    init_bin = b''.join(struct.pack('<HHII', sc, m, d, rc) for sc, m, d, rc in init_recs)
    blob_bin = struct.pack('<%dI' % len(blob), *blob)
    sections = [bytes(prog_bin), bytes(pipe_bin), tsc_bin, base_bin, init_bin, blob_bin, bytes(strings), bytes(heap)]
    header_size = 8 + 4 * 4 + 8 * 2 * len(sections)
    offs, at = [], (header_size + 255) // 256 * 256
    body = bytearray()
    for s in sections:
        pad = (-(at + len(body))) % 256
        body += bytes(pad)
        offs.append((at + len(body), len(s)))
        body += s
    corpus = fnv(''.join(sorted(r['key'] for r in pipes)).encode())
    hdr = MAGIC + struct.pack('<IIII', VERSION, len(prog_recs), len(pipes), len(constexpr_tsc))
    hdr += struct.pack('<%dQ' % (2 * len(sections)), *[x for o in offs for x in o])
    data = hdr + bytes((header_size + 255) // 256 * 256 - len(hdr)) + body
    open(os.path.join(out_dir, 'pack.bin'), 'wb').write(data)
    js = {'version': VERSION, 'corpus_hash': '%x' % corpus, 'programs': prog_recs, 'pipelines': pipes, 'constexpr_tsc': constexpr_tsc,
          'baseline': {'%x' % m: v for m, v in sorted(baseline.items())}, 'varying': ['%x' % m for m in varying],
          'init_methods': len(init_recs)}
    json.dump(js, open(os.path.join(out_dir, 'pack.json'), 'w'), indent=1, default=lambda o: o.hex() if isinstance(o, (bytes, bytearray)) else str(o))
    # decoder table: pipeline set-0 layouts by program pair, so nvdec compares only the bytes a layout defines
    with open(os.path.join(out_dir, 'layouts.txt'), 'w') as f:
        for rec in pipes:
            vs, fs = prog_recs[rec['vs']]['id'], prog_recs[rec['fs']]['id']
            kinds = lambda e: 'u' if e['kind'] == 'ubo' else ('h' if e.get('smp') else 't')   # t: a handle whose sampler is unused
            f.write('%s %s %x %s\n' % (vs, fs, rec['set_size'], ' '.join('%d:%s' % (e['off'], kinds(e)) for e in rec['entries'])))
    report.append('pack.bin %d KiB: %d pipelines, %d programs, %d constexpr samplers, %d blob dwords, %d baseline regs' % (
        len(data) // 1024, len(pipes), len(prog_recs), len(constexpr_tsc), len(blob), len(baseline)))
    open(os.path.join(out_dir, 'report.txt'), 'w').write('\n'.join(report) + '\n')
    print('\n'.join(report))

if __name__ == '__main__':
    main()
