"""Join the shards into the seed, and lay the seed and the raw pack out as the Data volume holds them.

usage: merge.py seed --shard <dir>... [--records <driver record store>] --tools <dir> --base <path>... --ipsw <build>
                     --plan <plan.json> --inventory <crawl inventory.json> --out <seed dir>
       merge.py tree --seed <seed dir> [--raw <raw pack dir>] --out <tree dir> --size <size.json>

seed: every shard's v1/ in one (names are what an entry holds, so two shards' files of one name are the same bytes),
the device's driver records of the tools' NVK identity beside them, SEED (the line the plugin logs) and MANIFEST.json.
Refuses tools that are not the GPU bundle's (--base: its tree or files): their NVK twin must have the bundle NVK's
LC_UUID, which its pipeline cache UUID hashes, and the bundle's Info.plist must name the tools' stack key (NVK, m2v, the
bridge and the frontend); a seed of other tools would never be hit.
tree: ngpu/addons/TegraGM20BMetal.m2v-seed/shared and ngpu/addons/TegraGM20BMetal.raw, directories 0755 and files
0644, and size.json (bytes, files, blocks4k: what the tree takes on APFS) beside it.
"""

import argparse
import hashlib
import json
import plistlib
import re
import shutil
import sys
from collections import Counter
from pathlib import Path

from tools.gpu.nvk_twin import lc_uuid

CONTENT = '529a20fd270ababa3e5ec2fe4d1674d93739ba5bcd61861674b4fb6db349af9f'
RECORD_MAGIC = b'M2VPPLC\0'
SEED = 'ngpu/addons/TegraGM20BMetal.m2v-seed/shared'
RAW = 'ngpu/addons/TegraGM20BMetal.raw'


def record_ok(data: bytes) -> bool:
    """Check a driver record's envelope: magic, identity, a length that ends at the checksum, the checksum."""
    return (
        len(data) >= 76
        and data[:8] == RECORD_MAGIC
        and 44 + int.from_bytes(data[36:44], 'little') + 32 == len(data)
        and hashlib.sha256(data[:-32]).digest() == data[-32:]
    )


def union(shards: list[Path], out: Path) -> tuple[str, int]:
    """Copy every shard's v1/ into `out`/v1; return the build directory's name and the files copied."""
    builds, copied = set(), 0
    for shard in shards:
        v1 = shard / 'v1'
        for path in sorted(p for p in v1.rglob('*') if p.is_file()) if v1.is_dir() else []:
            relative = path.relative_to(v1)
            if relative.parts[0] != CONTENT:
                builds.add(relative.parts[0])
            target = out / 'v1' / relative
            if not target.exists():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)
                copied += 1
    if len(builds) != 1:
        sys.exit(f'the shards made {len(builds)} m2v build directories ({", ".join(sorted(builds))}), not one')
    return builds.pop(), copied


def merge_records(store: Path | None, content: Path, identity: bytes) -> dict[str, int]:
    """Copy the device's records of `identity` whose envelope holds into `content` where it has none of that name."""
    counts = dict.fromkeys(('merged', 'here', 'other', 'refused'), 0)
    for path in sorted(store.glob('*/*')) if store and store.is_dir() else []:
        data = path.read_bytes()
        kind = (
            'refused'
            if not record_ok(data)
            else 'other'
            if data[8:36] != identity
            else 'here'
            if (content / path.parent.name / path.name).exists()
            else 'merged'
        )
        counts[kind] += 1
        if kind == 'merged':
            (content / path.parent.name).mkdir(parents=True, exist_ok=True)
            (content / path.parent.name / path.name).write_bytes(data)
    return counts


def crawled(inventory: dict) -> dict:
    """Summarize the crawl: what it found, what does not parse, and the Metal sources only the device compiles."""
    kinds = Counter(e['container'] for e in inventory['embedded_libraries'])
    return {
        'files': inventory['files'],
        'embedded': dict(kinds),
        'unreadable': inventory['unreadable'],
        'air': dict(Counter(a for e in inventory['libraries'] for a in e['air']).most_common()),
        'msl_sources': sum(inventory['msl_sources'].values()),
    }


STORE_PATH = re.compile(r'^.*/(store|crawl)/root/')


def refusals(lines: list[str]) -> dict[str, dict]:
    """Group the tool's refusals (`<kind> <what> (<library>): <error>`) by library: a count and the first few."""
    grouped: dict[str, dict] = {}
    for line in lines:
        library = line[line.find(' (') + 2 : line.find('): ')] if ' (' in line and '): ' in line else '?'
        library = STORE_PATH.sub('/', library)
        entry = grouped.setdefault(library, {'count': 0, 'first': []})
        entry['count'] += 1
        if len(entry['first']) < 3:
            entry['first'].append(line[:300])
    return dict(sorted(grouped.items(), key=lambda kv: -kv[1]['count']))


def check_tools(tools: Path, base: list[Path]) -> dict:
    """Return the tools' manifest, after checking it is the base bundle's."""
    manifest = json.loads((tools / 'MANIFEST.json').read_text())
    files = [p for b in base for p in ([b] if b.is_file() else b.rglob('*')) if p.is_file()]
    info = [p for p in files if p.name == 'Info.plist' and p.parent.name == 'TegraGM20BMetal.bundle']
    nvk = [p for p in files if p.name == 'libvulkan_nouveau.dylib' and p.parent.name == 'TegraGM20BMetal.bundle']
    if len(info) != 1 or len(nvk) != 1:
        sys.exit('--base has no single TegraGM20BMetal.bundle (Info.plist and libvulkan_nouveau.dylib)')
    if lc_uuid(tools / 'libvulkan_nouveau.dylib') != lc_uuid(nvk[0]):
        sys.exit("the shader tools' NVK twin is not the GPU bundle's NVK: tools and bundle come from different builds")
    stack = plistlib.loads(info[0].read_bytes()).get('SwitchShaderStack')
    if stack != manifest['stack']:
        sys.exit(
            "the GPU bundle's stack is {}, the shader tools' {}: tools and bundle come from different builds".format(
                stack, manifest['stack']
            )
        )
    return manifest


def seed(args: argparse.Namespace) -> None:
    """Write the seed directory."""
    manifest = check_tools(args.tools, args.base)
    out = args.out
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    build, copied = union(args.shard, out)
    records = sorted((out / 'v1' / CONTENT).glob('*/*'))
    if not records:
        sys.exit('the shards made no driver record')
    identity = records[0].read_bytes()[8:36]
    device = merge_records(args.records, out / 'v1' / CONTENT, identity)
    digest = hashlib.sha256()
    for path in sorted(p for p in (out / 'v1').rglob('*') if p.is_file()):
        digest.update(str(path.relative_to(out)).encode() + b'\0' + path.read_bytes())
    line = f'generated m2v-build {build[:8]} nvk {identity[12:28].hex()[:8]} content {digest.hexdigest()[:8]}'
    (out / 'SEED').write_text(line + '\n')
    reports = [json.loads((s / 'report.json').read_text()) for s in args.shard]
    (out / 'MANIFEST.json').write_text(
        json.dumps(
            {
                'seed': line,
                'ipsw': args.ipsw,
                'tools': {k: manifest[k] for k in ('stack', 'nvk_uuid', 'arch')},
                'plan': json.loads(args.plan.read_text()),
                'crawl': crawled(json.loads(args.inventory.read_text())),
                'device_records': device,
                'crashed': [line for r in reports for line in r.get('crashed', [])],
                'refused': refusals([line for r in reports for line in r.get('refused', [])]),
                'seconds': round(sum(r.get('seconds', 0) for r in reports), 1),
            },
            indent=1,
        )
        + '\n'
    )
    print(f'seed: {line} ({copied} files from {len(args.shard)} shards; device records {device})')


def tree(args: argparse.Namespace) -> None:
    """Lay the seed and the raw pack out under ngpu/addons, and write the size."""
    out = args.out
    shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(args.seed, out / SEED, copy_function=shutil.copyfile)
    if args.raw:
        shutil.copytree(args.raw, out / RAW, copy_function=shutil.copyfile)
    files = [p for p in out.rglob('*') if p.is_file()]
    for path in [out, *out.rglob('*')]:
        path.chmod(0o755 if path.is_dir() else 0o644)
    sizes = [p.stat().st_size for p in files]
    args.size.write_text(
        json.dumps({'bytes': sum(sizes), 'files': len(files), 'blocks4k': sum((s + 4095) // 4096 for s in sizes)})
        + '\n'
    )


def main() -> None:
    """Run one of the two steps."""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    steps = parser.add_subparsers(dest='step', required=True)
    s = steps.add_parser('seed')
    s.add_argument('--shard', type=Path, action='append', required=True)
    s.add_argument('--records', type=Path)
    s.add_argument('--tools', type=Path, required=True)
    s.add_argument('--base', type=Path, action='append', required=True)
    s.add_argument('--ipsw', required=True)
    s.add_argument('--plan', type=Path, required=True)
    s.add_argument('--inventory', type=Path, required=True)
    s.add_argument('--out', type=Path, required=True)
    t = steps.add_parser('tree')
    t.add_argument('--seed', type=Path, required=True)
    t.add_argument('--raw', type=Path)
    t.add_argument('--out', type=Path, required=True)
    t.add_argument('--size', type=Path, required=True)
    args = parser.parse_args()
    {'seed': seed, 'tree': tree}[args.step](args)


if __name__ == '__main__':
    main()
