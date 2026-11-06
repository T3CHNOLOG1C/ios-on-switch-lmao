"""Translate one mounted volume (the System volume, the SystemOS cryptex or the App cryptex) offline.

    translate_tree --role ROLE --tool arm64e --volume MNT --out DIR [--apple-trustcache IM4P]...

The tree is read-only (a decrypted image mounted on the Mac). For every Mach-O whose translation changes bytes, the
replacement file goes to DIR under its volume path and an entry to DIR/<role>.manifest.json. The new cdhashes become
DIR/<role>.trustcache, a version-2 module carrying each binary's launch-constraint category from Apple's own trust
caches. For the SystemOS cryptex the dyld shared cache is translated with its plan (made here, DIR/<role>.a64xplan),
the prebuilt-loader cdhashes of on-disk binaries are updated to their translations, and the translated cache files'
cdhashes go to DIR/a64x-cache-pretranslated.bin for the device tree.

Each file is translated by one `arm64e` subprocess, skipped when DIR already holds its output for this input and
translator version (content addressed): reruns resume. It does not mount or write outside DIR.
"""

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

CACHE_DIR = 'System/Library/Caches/com.apple.dyld'
MACHO_MAGIC = (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe', b'\xca\xfe\xba\xbf')  # thin 64, fat BE, fat64 BE


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def is_macho(path):
    try:
        with open(path, 'rb') as f:
            return f.read(4) in MACHO_MAGIC
    except OSError:
        return False


def apple_trust_entries(paths):
    """cdhash (hex) -> (flags, category) from Apple's trust cache IM4Ps (version-2 modules)."""
    out = {}
    for p in paths:
        data = Path(p).read_bytes()
        payload = _im4p_payload(data)
        version = struct.unpack_from('<I', payload)[0]
        count = struct.unpack_from('<I', payload, 20)[0]
        stride = 24 if version == 2 else 22
        for i in range(count):
            e = payload[24 + stride * i : 24 + stride * (i + 1)]
            out[e[:20].hex()] = (e[21], e[22] if version == 2 else 0)
    return out


def _im4p_payload(data):
    def tlv(at):
        tag, length = data[at], data[at + 1]
        at += 2
        if length & 0x80:
            n = length & 0x7F
            length = int.from_bytes(data[at : at + n], 'big')
            at += n
        return tag, at, length

    _, at, length = tlv(0)
    end = at + length
    fields = []
    while at < end:
        tag, start, length = tlv(at)
        fields.append((tag, data[start : start + length]))
        at = start + length
    return next(v for t, v in fields[1:] if t == 0x04)


def trust_module(entries):
    """A version-2 trust cache module: sorted (cdhash, hashtype 2, flags, category)."""
    body = b''.join(h + bytes([2, flags, cat, 0]) for h, flags, cat in sorted(entries))
    uuid = hashlib.sha256(body).digest()[:16]
    return struct.pack('<I16sI', 2, uuid, len(entries)) + body


def run_tool(tool, *args):
    return subprocess.run([tool, *args], capture_output=True, text=True, check=True).stdout


def pretranslate_file(tool, src, out_path, report_path):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    run_tool(tool, 'pretranslate', '-r', str(report_path), str(src), str(out_path))
    return json.loads(report_path.read_text())


def _memoized(tool, src, dst, memo, in_sha, version):
    """Translate src, or reuse an earlier run whose report records the same input and translator (resume)."""
    if memo.exists():
        prev = json.loads(memo.read_text())
        if prev.get('_in') == in_sha and prev.get('_tool') == version:
            translated = any(s['status'] == 'translated' for s in prev['slices'])
            if translated == dst.exists():
                return prev
    r = pretranslate_file(tool, src, dst, memo)
    r['_in'] = in_sha
    r['_tool'] = version
    memo.write_text(json.dumps(r))
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--role', required=True, choices=('os', 'system_cryptex', 'app_cryptex'))
    ap.add_argument('--tool', required=True, help='the arm64e CLI')
    ap.add_argument('--volume', required=True, type=Path, help='the mounted, read-only volume')
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--apple-trustcache', action='append', default=[], help='Apple trust cache IM4P (flags/category)')
    ap.add_argument(
        '--cdhash-pairs',
        action='append',
        default=[],
        help="another role's <role>.cdhash-pairs, for the cache's prebuilt loaders",
    )
    args = ap.parse_args()

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    work = out / '.work'
    work.mkdir(exist_ok=True)
    version = _tool_version(args.tool)
    apple = apple_trust_entries(args.apple_trustcache)

    manifest = {'role': args.role, 'translator': version, 'files': []}
    trust = []  # (cdhash_bytes, flags, category)
    # (old_hex, new_hex): prebuilt loaders in the cache validate on-disk binaries of every volume, so the cache apply
    # takes this role's pairs and those of the roles run before it (os and app_cryptex, for system_cryptex).
    pairs = []
    external_pairs = []
    for p in args.cdhash_pairs:
        for line in Path(p).read_text().splitlines():
            old, new = line.split()
            external_pairs.append((old, new))
    no_category = 0

    cache = args.volume / CACHE_DIR
    machos = []
    for root, _, names in os.walk(args.volume):
        if os.path.abspath(root).startswith(os.path.abspath(cache)):
            continue
        for name in names:
            p = Path(root) / name
            if not p.is_symlink() and is_macho(p):
                machos.append(p)

    for src in sorted(machos):
        rel = src.relative_to(args.volume)
        dst = out / 'files' / rel
        in_sha = sha256(src)
        memo = work / (str(rel).replace('/', '_') + '.json')
        r = _memoized(args.tool, src, dst, memo, in_sha, version)
        translated = [s for s in r['slices'] if s['status'] == 'translated']
        if not translated:
            dst.unlink(missing_ok=True)
            continue
        manifest['files'].append(
            {
                'path': '/' + str(rel),
                'file': os.path.relpath(dst, out),
                'sha256_in': in_sha,
                'sha256_out': sha256(dst),
                'cdhash': [s['cdhash_out'] for s in translated],
            }
        )
        for s in translated:
            pairs.append((s['cdhash_in'], s['cdhash_out']))
            if s['cdhash_in'] not in apple:
                no_category += 1
            flags, cat = apple.get(s['cdhash_in'], (0, 0))
            trust.append((bytes.fromhex(s['cdhash_out']), flags, cat))

    (out / f'{args.role}.cdhash-pairs').write_text(''.join(f'{a} {b}\n' for a, b in pairs))
    if args.role == 'system_cryptex' and cache.is_dir():
        _translate_cache(args, cache, out, pairs + external_pairs, trust, apple, manifest)

    (out / f'{args.role}.manifest.json').write_text(json.dumps(manifest, indent=1))
    (out / f'{args.role}.trustcache').write_bytes(trust_module(trust))
    if no_category:
        print(
            f"{args.role}: WARNING {no_category} binaries not in Apple's trust caches (trust category 0)",
            file=sys.stderr,
        )
    print(f'{args.role}: {len(manifest["files"])} files translated, {len(trust)} trust entries')


def _translate_cache(args, cache, out, pairs, trust, apple, manifest):
    main_cache = cache / 'dyld_shared_cache_arm64e'
    plan = out / f'{args.role}.a64xplan'
    run_tool(args.tool, 'plan', '-o', str(plan), str(main_cache))
    pairs_file = out / '.work' / 'cache_pairs.txt'
    pairs_file.write_text(''.join(f'{a} {b}\n' for a, b in pairs))

    suffixes = _subcache_suffixes(main_cache)
    cdhashes = []
    for name in ['dyld_shared_cache_arm64e'] + [f'dyld_shared_cache_arm64e{s}' for s in suffixes]:
        src = cache / name
        if name.endswith('.symbols') or name.endswith('.atlas') or not src.exists():
            continue
        dst = out / 'files' / CACHE_DIR / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        line = run_tool(args.tool, 'dsc-apply', '-c', str(pairs_file), str(plan), str(src), str(dst))
        cdhash = line.split()[0]
        cdhashes.append(bytes.fromhex(cdhash))
        if sha256(dst) == sha256(src):
            dst.unlink()
            continue
        manifest['files'].append(
            {
                'path': '/' + str(dst.relative_to(out / 'files')),
                'file': os.path.relpath(dst, out),
                'sha256_in': sha256(src),
                'sha256_out': sha256(dst),
                'cdhash': [cdhash],
            }
        )
        flags, cat = apple.get(_cache_cdhash_in(args.tool, src), (0, 0))
        trust.append((bytes.fromhex(cdhash), flags, cat))
    (out / 'a64x-cache-pretranslated.bin').write_bytes(b''.join(cdhashes))


def _subcache_suffixes(main_cache):
    data = main_cache.read_bytes()
    off, count = struct.unpack_from('<II', data, 0x188)
    return [data[off + 56 * i + 24 : off + 56 * i + 56].split(b'\0')[0].decode() for i in range(count)]


def _cache_cdhash_in(tool, src):
    # dsc-apply reports the new cdhash; the original is read directly for the trust-category lookup.
    data = src.read_bytes()
    sig_off, sig_size = struct.unpack_from('<QQ', data, 0x28)
    cd = data.index(b'\xfa\xde\x0c\x02', sig_off)
    length = struct.unpack_from('>I', data, cd + 4)[0]
    return hashlib.sha256(data[cd : cd + length]).digest()[:20].hex()


def _tool_version(tool):
    return hashlib.sha256(Path(tool).read_bytes()).hexdigest()[:16]


if __name__ == '__main__':
    sys.exit(main())
