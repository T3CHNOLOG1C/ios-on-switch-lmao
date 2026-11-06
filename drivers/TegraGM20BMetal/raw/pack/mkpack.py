#!/usr/bin/env python3
"""mkpack (r7-raw): the GM20B pipeline pack for the raw Metal path, read off the probe run.

The probe run: capture/probe.mm through the live frontend + m2v + NVK twin with nvdec in NVDEC_PROBE + NVDEC_SCRUB +
NVDEC_TRACE=3 mode.

For every corpus render pipeline state the raw path can take (one draw, no m2v-internal passes or dispatches):
  * its two programs: the 80-byte shader header + Maxwell code NAK made (from the NVK driver records), laid out in one
    shader heap image the way NVK uploads them pre-Turing (header at +0x30, code at +0x80 of a 0x100-aligned slot);
  * the 3D registers it sets (the registers that differ between states), as (method, value) pairs;
  * its root-table push words and its set-0 layout: which byte of the 256-byte descriptor set holds which Metal buffer
    slot's uniform descriptor, texture/sampler handle or framebuffer-fetch handle;
  * which Metal vertex buffer slot feeds vertex stream 0.
Plus what every state shares: the context init NVK records at queue creation (with the addresses to relocate marked) and
the registers every state's draws leave the same (the baseline).

usage: mkpack.py <merged programs.bin out> <out dir> <bucket dir>...
  each bucket dir (rawpack.py's) holds its own probe runs: probe256.out/.log, and the PROBE_BYTES=16 and 4096 runs
  (probe16.*, probe4096.*: each buffer entry's descriptor type, a uniform buffer's range NVK rounds up to 256 bytes, and
  the function's window on the slot) and programs.bin; a state's index is (its bucket, its line in the bucket's corpus).
  The context init and post-init registers are the first bucket's (every probe process opens the device alike). A
  bucket dir named barrier<i> holds the fetching states' run with NVK's interlock hidden: a state whose own record is
  metal2vulkan's storage form gets a second record from it, its barrier form (PipelineBarrierForm), after every first.
writes the merged programs.bin, <out>/pack.bin (runtime/raw_pack.h layout), <out>/pack.json, <out>/programs.txt
(decoder tables), and prints a report.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import layouts
import packfile
import probelog

# Registers the runtime owns (sets per pass, per draw, or once at init with its own addresses): never in a blob or the
# baseline.
OWNED = [
    (0x0000, 0x0200),
    (0x0800, 0x0A00),
    (0x0A00, 0x0A18),
    (0x0C00, 0x0C08),
    (0x0D6C, 0x0D90),
    (0x0E00, 0x0E0C),
    (0x0FE0, 0x0FFC),
    (0x1970, 0x1974),
    (0x1644, 0x1648),
    (0x1118, 0x111C),
    (0x1434, 0x143C),
    (0x10F8, 0x10FC),
    (0x121C, 0x1220),
    (0x1538, 0x153C),
    (0x19E0, 0x1A00),
    (0x1F00, 0x1F80),
    (0x1608, 0x1610),
    (0x1614, 0x161C),
    (0x1574, 0x1580),
    (0x155C, 0x1568),
    (0x0F84, 0x0F8C),
    (0x1B00, 0x1B10),
    (0x2380, 0x2600),
    (0x3400, 0x4000),
]


def owned(m: int) -> bool:
    if 0x1C00 <= m < 0x1D00 and (m & 0xF) in (4, 8):  # vertex stream locations
        return True
    return any(a <= m < b for a, b in OWNED)


# Init relocations: (subc, method) -> what the runtime puts there
RELOC = {
    (0, 0x1608): 'heap_hi',
    (0, 0x160C): 'heap_lo',
    (1, 0x1608): 'heap_hi',
    (1, 0x160C): 'heap_lo',
    (0, 0x0F84): 'zero_hi',
    (0, 0x0F88): 'zero_lo',
    (0, 0x3468): 'root_hi',
    (0, 0x346C): 'root_lo',
    (0, 0x3470): 'zero_hi',
    (0, 0x3474): 'zero_lo',
    (0, 0x1574): 'tic_hi',
    (0, 0x1578): 'tic_lo',
    (0, 0x157C): 'tic_max',
    (1, 0x1574): 'tic_hi',
    (1, 0x1578): 'tic_lo',
    (1, 0x157C): 'tic_max',
    (0, 0x155C): 'tsc_hi',
    (0, 0x1560): 'tsc_lo',
    (0, 0x1564): 'tsc_max',
    (1, 0x155C): 'tsc_hi',
    (1, 0x1560): 'tsc_lo',
    (1, 0x1564): 'tsc_max',
}
RELOC_CODE = {
    'heap_hi': 1,
    'heap_lo': 2,
    'zero_hi': 3,
    'zero_lo': 4,
    'root_hi': 5,
    'root_lo': 6,
    'tic_hi': 7,
    'tic_lo': 8,
    'tic_max': 9,
    'tsc_hi': 10,
    'tsc_lo': 11,
    'tsc_max': 12,
}


def load_programs(path: str) -> dict[str, dict[str, bytes | list[int]]]:
    with open(path, 'rb') as programs:
        b = programs.read()
    progs, at = {}, 0
    while at + 12 <= len(b):
        assert b[at : at + 4] == b'NVPG'
        code, count = struct.unpack_from('<II', b, at + 4)
        at += 12
        types = list(b[at : at + count])
        at += count
        image = b[at : at + 80 + code]
        at += 80 + code
        progs[f'{packfile.fnv(image):x}'] = {'image': image, 'cbufs': types}
    return progs


def load_buckets(dirs: list[str], size: int) -> list[dict]:
    """Give every bucket's probes of the run with `size`-byte payloads, each with its bucket's number and form.

    The form is 'barrier' in a barrier bucket (the run with NVK's interlock hidden, rawpack.py), else ''.
    """
    probes = []
    for b, d in enumerate(dirs):
        form = 'barrier' if os.path.basename(os.path.normpath(d)).startswith('barrier') else ''
        for p in probelog.load(os.path.join(d, f'probe{size}.out'), os.path.join(d, f'probe{size}.log')):
            p['bucket'] = b
            p['form'] = form
            probes.append(p)
    return probes


def interlocked(p: dict) -> bool:
    """Whether the probe's draw had SET_PIXEL_SHADER_INTERLOCK_CONTROL on: metal2vulkan's storage form of a fetch."""
    return bool(p['records'][0]['regs'].get(0x1224, 0) & 3)


def barrier_forms(simple: list[dict]) -> list[dict]:
    """Keep a barrier-form probe only where the same state's default record is in the storage form.

    Elsewhere the two runs drew it alike.
    """
    storage = {p['key'] for p in simple if not p['form'] and interlocked(p)}
    return [p for p in simple if not p['form'] or p['key'] in storage]


def load_sizes(dirs: list[str]) -> dict[tuple, list[int]]:
    sizes = {}  # (bucket, state index, set offset) -> (bound size with 16-byte payloads, with 4096-byte payloads)
    for b in (16, 4096):
        for p in load_buckets(dirs, b):
            if p['draws'] != 1 or p['dispatches'] or not p['records'] or 'set0' not in p['records'][0]:
                continue
            w = p['records'][0]['set0'][1]
            for o in range(0, len(w) - 3, 4):
                if f'ptr0.{o * 4}' in p['records'][0]['ptr']:
                    sizes.setdefault((p['bucket'], p['index'], o * 4), [0, 0])[0 if b == 16 else 1] = w[o + 2]
    return sizes


def load_init(log_path: str) -> tuple[list[tuple[int, int, int]], dict[int, dict[int, int]]]:
    # init trace and post-init registers
    init, post = [], {0: {}, 1: {}}
    with open(log_path) as log:
        for line in log:
            if line.startswith('T '):
                _, sc, m, d = line.split()
                init.append((int(sc), int(m, 16), int(d, 16)))
            elif line.startswith('S regs '):
                f = line.split()
                post[int(f[2])] = {int(a, 16): int(v, 16) for a, v in (x.split('=') for x in f[3:])}
    return init, post


def skip_reason(p: dict) -> str | None:
    if p['draws'] != 1:
        return f'{p["draws"]} draws'
    if p['dispatches']:
        return f'{p["dispatches"]} dispatches'
    if not p['records']:
        return 'no record'
    r = p['records'][0]
    if any(v[2] != 'known' for v in r['prog'].values()):
        return 'unknown program'
    if sorted(r['prog']) != [1, 5]:
        return f'stages {sorted(r["prog"])}'
    return None


def split_probes(probes: list[dict]) -> tuple[list[dict], list[tuple[dict, str]]]:
    simple, skipped = [], []
    for p in probes:
        why = skip_reason(p)
        # (three color attachments: the render server's since r7n's harvest, Core Animation's "A3" states with an
        # R32Float third attachment, taken as any other; test/frame.h FRAME_A3 draws them)
        if why:
            skipped.append((p, why))
        else:
            simple.append(p)
    return simple, skipped


# a color target's own registers (per-target blend 0x1e00 + 0x20 j, SET_CT_WRITE 0x1a00 + 4 j) mean nothing for a
# state without target j: what the probe saw there is an earlier state's leftover. Not that state's, so neither in
# its blob nor in deciding what varies (r8-rawca: two states with a third target made targets 2's blend "vary" for
# every other state)
def targets(key: str) -> int:
    return sum(1 for t in key.split() if t[:1] == 'c' and t[1:2].isdigit() and '=' in t)


def foreign(m: int, n: int) -> bool:
    return (0x1E00 <= m < 0x1F00 and (m - 0x1E00) // 0x20 >= n) or (0x1A00 <= m < 0x1A20 and (m - 0x1A00) // 4 >= n)


def split_registers(simple: list[dict], post: dict[int, dict[int, int]]) -> tuple[list[int], dict[int, int]]:
    # registers: varying across states -> per-pipeline blob; equal -> baseline (when it differs from post-init)
    allregs = set(post[0])
    for p in simple:
        allregs |= set(p['records'][0]['regs'])
    vals = {m: {p['records'][0]['regs'].get(m, 0) for p in simple} for m in allregs if not owned(m)}
    varying = sorted(m for m, v in vals.items() if len(v) > 1)
    baseline = {m: next(iter(v)) for m, v in vals.items() if len(v) == 1 and next(iter(v)) != post[0].get(m, 0)}
    return varying, baseline


def lay_out_programs(
    simple: list[dict], programs: dict[str, dict], report: list[str]
) -> tuple[bytearray, dict[str, int], list[dict]]:
    # programs used, heap layout
    used = []
    for p in simple:
        for st in (1, 5):
            pid = p['records'][0]['prog'][st][0]
            if pid not in used:
                used.append(pid)
    heap, prog_index, prog_recs = bytearray(), {}, []
    for pid in used:
        pr = programs[pid]
        off = len(heap)
        slot = bytearray(0x80 - 0x50) + pr['image'][:80]  # header at +0x30
        slot += pr['image'][80:]  # code at +0x80
        slot += bytes((-len(slot)) % 0x100)
        heap += slot
        prog_index[pid] = len(prog_recs)
        if any(t not in (1, 3) for t in pr['cbufs']):
            report.append(f'  program {pid} has cbufs {pr["cbufs"]} (only root + set 0 supported)')
        prog_recs.append(
            {
                'id': pid,
                'heap_offset': off,
                'program': off + 0x30,
                'bytes': len(slot),
                'code': len(pr['image']) - 80,
                'cbufs': pr['cbufs'],
            }
        )
    report.append(f'{len(prog_recs)} programs, shader heap image {len(heap) // 1024} KiB')
    return heap, prog_index, prog_recs


def buffer_entry(e: tuple, index: tuple[int, int], sizes: dict[tuple, list[int]]) -> dict:
    s16, s4k = sizes.get((*index, e[0]), (256, 256))
    # a 16-byte payload bound at 256: a uniform buffer (NVK rounds its range to the cbuf alignment); else storage. The
    # 4096-byte one's range: the window (0: the whole payload)
    ssbo = s16 < 256
    window = 0 if s4k >= 4096 else s4k
    return {
        'off': e[0],
        'kind': 'ubo',
        'stage': e[2],
        'slot': e[3],
        'size': e[4],
        'ssbo': ssbo,
        'window': window,
    }


def handle_entry(e: tuple, tsc: dict[int, list[int]], constexpr_tsc: list[list[int]], problems: list[str]) -> dict:
    tk, sk = e[2], e[3]
    ent = {'off': e[0], 'kind': 'handle'}
    if tk is None:
        ent['tex'] = None
    elif tk[0] == 'tex':
        ent['tex'] = ('slot', tk[1])
    elif tk[0] == 'fetch':
        ent['tex'] = ('fetch', tk[1])
    else:
        problems.append(f'texture {tk}')
    if sk is None:
        ent['smp'] = None
    elif sk[0] == 'smp':
        ent['smp'] = ('slot', sk[1])
    elif sk[0] == 'tsc?derived':
        problems.append('a sampler m2v derives from a bound one (YCbCr conversion)')
    else:
        # a sampler the probe did not bind: the function's constexpr sampler (made by m2v as an immutable one)
        words = tsc[e[4] >> 20]
        if words not in constexpr_tsc:
            constexpr_tsc.append(words)
        ent['smp'] = ('constexpr', constexpr_tsc.index(words))
    return ent


def vertex_streams(r: dict, problems: list[str]) -> list[dict]:
    vbs = []
    for j, (_loc, _lim, fmt) in sorted(r['vbinfo'].items()):
        w = r['vb'][j][1]
        if (w[0] >> 16) != 0xC0DE or (w[0] >> 8) & 0xFF != 1:
            problems.append(f'vertex stream {j} not a vertex slot')
        vbs.append({'stream': j, 'slot': w[0] & 0xFF, 'stride': fmt & 0xFFF})
    return vbs


def pipeline(
    p: dict,
    varying: list[int],
    prog_index: dict[str, int],
    sizes: dict[tuple[int, int], list[int]],
    constexpr_tsc: list[list[int]],
) -> dict:
    r = p['records'][0]
    ents, problems = layouts.layout(p)
    rec = {
        'index': p['index'],
        'key': p['key'],
        'form': p['form'],
        'vs': prog_index[r['prog'][1][0]],
        'fs': prog_index[r['prog'][5][0]],
    }
    rec['blob'] = [
        (m, r['regs'].get(m, 0)) for m in varying if m not in (0x2044, 0x2144) and not foreign(m, targets(p['key']))
    ]
    root = r['root'][1]
    rec['push'] = [(int(o, 16), int(x, 16)) for o, x in (w.split('=') for w in r.get('pushw', []))]
    rec['set_size'] = root[0x38 // 4 + 2] if 'set0' in r else 0
    entries = [
        buffer_entry(e, (p['bucket'], p['index']), sizes)
        if e[1] == 'ubo'
        else handle_entry(e, r['tsc'], constexpr_tsc, problems)
        for e in ents
    ]
    rec['entries'] = entries
    rec['vbs'] = vertex_streams(r, problems)
    rec['fetch'] = any(e.get('tex', [None])[0] == 'fetch' for e in entries if e['kind'] == 'handle')
    rec['clears'] = len(p['clears'])
    rec['problems'] = problems
    return rec


def main() -> None:
    prog_path, out_dir, dirs = sys.argv[1], sys.argv[2], sys.argv[3:]
    with open(prog_path, 'wb') as merged:
        for d in dirs:
            with open(os.path.join(d, 'programs.bin'), 'rb') as part:
                merged.write(part.read())
    sizes = load_sizes(dirs)
    os.makedirs(out_dir, exist_ok=True)
    seen: set[tuple[str, str]] = set()
    probes = [
        p for p in load_buckets(dirs, 256) if (p['form'], p['key']) not in seen and not seen.add((p['form'], p['key']))
    ]
    programs = load_programs(prog_path)
    init, post = load_init(os.path.join(dirs[0], 'probe256.log'))
    # drop the query-pool setup (submit 4 is not traced) and NVK's own MME bookkeeping we relocate
    report = []
    simple, skipped = split_probes(probes)
    simple = barrier_forms(simple)
    for p in simple:
        n = targets(p['key'])
        p['records'][0]['regs'] = {m: v for m, v in p['records'][0]['regs'].items() if not foreign(m, n)}
    varying, baseline = split_registers(simple, post)
    barriers = sum(1 for p in simple if p['form'])
    report.append(
        f'{len(probes)} states probed, {len(simple) - barriers} taken, {len(skipped)} skipped; '
        f'{barriers} storage-form states with their barrier form'
    )
    for p, why in skipped:
        report.append(f'  skipped {p["index"]} ({why}): {p["key"][:150]}')
    names = ' '.join(f'{m:x}' for m in varying)
    report.append(f'{len(varying)} per-pipeline registers: {names}')
    report.append(f'{len(baseline)} baseline registers over post-init')
    heap, prog_index, prog_recs = lay_out_programs(simple, programs, report)
    # pipelines
    pipes, constexpr_tsc = [], []
    for p in simple:
        rec = pipeline(p, varying, prog_index, sizes, constexpr_tsc)
        if rec['problems']:  # m2v draws it (RawMetal.mm: a state not in the pack goes there)
            report.append(f'  skipped {p["index"]} ({"; ".join(rec["problems"])}): {p["key"][:150]}')
            continue
        pipes.append(rec)
    pipes.sort(key=lambda rec: rec['form'] == 'barrier')  # a key's first record is its default (Pack::find)
    # init template with relocations (submissions 1-3 as NVK recorded them)
    init_recs = [(sc, m, d, RELOC_CODE.get(RELOC.get((sc, m)), 0)) for sc, m, d in init]
    report.append(f'init template: {len(init_recs)} methods, {sum(1 for x in init_recs if x[3])} relocated')
    pack = packfile.Pack(pipes, prog_recs, constexpr_tsc, baseline, init_recs, heap)
    data, blob = packfile.pack_bytes(pack)
    with open(os.path.join(out_dir, 'pack.bin'), 'wb') as pack_bin:
        pack_bin.write(data)
    packfile.write_json(os.path.join(out_dir, 'pack.json'), pack, varying)
    packfile.write_layouts(os.path.join(out_dir, 'layouts.txt'), pack)
    report.append(
        f'pack.bin {len(data) // 1024} KiB: {len(pipes)} pipelines, {len(prog_recs)} programs, '
        f'{len(constexpr_tsc)} constexpr samplers, {len(blob)} blob dwords, {len(baseline)} baseline regs'
    )
    with open(os.path.join(out_dir, 'report.txt'), 'w') as report_txt:
        report_txt.write('\n'.join(report) + '\n')
    print('\n'.join(report))


if __name__ == '__main__':
    main()
