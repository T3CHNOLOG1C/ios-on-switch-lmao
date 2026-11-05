#!/usr/bin/env python3
"""Merge device journals (the frontend's pipelines.log: S lines, one per function specialization, R lines, one per render pipeline
state, and C lines, one per compute pipeline state) into the precompile corpus: each made once, by any process, in the order first
seen, without the times, threads, costs and callers. The corpus keeps what it had; new journals only add. Each function names its
library (lib=: the file's path, or data:<SHA-256> of a library made from bytes); a line from before the journal named libraries
has none, and precompile/libraries.py guesses it by the function's name.

usage: harvest.py <corpus.log> <pipelines.log>...    e.g. harvest.py corpus.log out/switch/device-logs/*/m2v-cache/pipelines.log
       harvest.py <corpus.log> --volume <mounted Data volume> [--save <dir>] [--open <container UUID>]... [--open-all]

--volume  every process's m2v cache on the iPad's Data volume (the plugin's cacheCandidates: a container's
          Library/Caches/m2v, Library/Caches/m2v-<process> or tmp/m2v, tmp/m2v-<process>, a daemon's user cache or
          temp directory), its pipelines.log and pipelines.log.old. Read only, but for one write: a system container's
          Library and Library/Caches are mode 000, so for a container named by --open (default: backboardd's,
          SpringBoard's and Preferences', SYSTEM_CACHES) or every one with --open-all, each closed directory on the
          way is made u+rx for the read and given its mode back at once (and checked); other closed containers are
          skipped and counted.
--save    also copy each m2v cache found, whole (v1/, msl/, libs/, the journals, makings.log), to <dir>/<where it was>, e.g. the boot's
          log folder. makings.log (m2v 0118, the plugin turns it on in every cache): one line per driver record the process wrote
          (a compile), its name and the making's description; a precompile run with METAL2VULKAN_MAKING_LOG=<file> writes the
          host's, so a seeded making the device made again shows where the two descriptions part.
--device  the device data staging directory (default out/gpu-device-data/staging of the checkout): with --volume, and for every
          saved cache below --records-from, each driver record (the content directory's v1/<content>/<xx>/<name>, m2v's
          VkPipelineCache data of one pipeline making, named by what the making reads and the NVK identity) whose envelope
          checks out is copied to <device>/records/<xx>/<name>, and every cache's libs/<SHA-256>.metallib (a library a process
          made from bytes) to <device>/data/. The device's own makings: those of draws' variants that no creation predicts,
          and each one the precompile makes otherwise than the device (r8-webgpu: 1268 of the 1293 records of the 23:17
          boot's caches were not in the seed). The seed takes those of its NVK's identity.
          harvest.py --records-from <dir>... [--device <dir>]: only the records of every m2v cache below each <dir> (the
          --save copies, e.g. out/switch/device-logs/*/m2v), no corpus.
--pack    harvest.py --pack [--device <dir>]: the staging directory as the build's @gpu_device_data: a tar (sorted, no times or
          owners) of its records and of the libraries made from bytes that the corpus names and the build cannot make itself
          (not one of bazel/gpu/apple-libs.sha256's, not the frontend's TwoPlaneStandIn.h: Core Image's kernels compiled on the
          device), written to out/gpu-device-data/m2v-device-<sha256[:16]>.tar; prints the DEVICE_DATA pin for
          bazel/gpu/deps.bzl. The first build after the pin copies the tar into the repository cache; no build reads the
          staging directory.
"""
import hashlib
import io
import os
import re
import shutil
import stat
import sys
import tarfile
from contextlib import contextmanager
from pathlib import Path

DESCRIPTOR = re.compile(r'(samples|topo|ds|opt|c\d|a\d+)=')
COMPUTE = re.compile(r'(form|opt|max|tgm|icb|depth|abf|mut)=')
# The checkout: bazel run's (also what //deploy:pull_logs runs this in), else the one this file is in.
PROJECT = Path(os.environ.get('BUILD_WORKSPACE_DIRECTORY') or Path(__file__).resolve().parents[7])
DEVICE = PROJECT / 'out/gpu-device-data'
# /private/var/containers/Data/System/<UUID>/Library/Caches/m2v of the system containers whose processes kept an m2v cache there
# (the plugin's "device ... ready in" lines, r5d and r6h): their Library and Library/Caches are mode 000.
SYSTEM_CACHES = {'6C7A65AF-CB18-421C-A07A-D724B05A7AB6': 'backboardd', '532E562F-2888-4C55-9BF8-E6F0B246BBDA': 'SpringBoard',
                 '5BDBB9F1-C07A-4290-9D80-EA49A6BA2D68': 'Preferences', 'AA90E4B6-B0E4-4776-8E70-A4444E65642F': 'Spotlight'}
JOURNALS = ('pipelines.log.old', 'pipelines.log')
CONTENT = '529a20fd270ababa3e5ec2fe4d1674d93739ba5bcd61861674b4fb6db349af9f'   # m2v's content directory (seed.py's CONTENT)
RECORD_MAGIC = b'M2VPPLC\0'     # m2v shader_cache/driver.rs: magic, identity (28 bytes), data length (u64), data, SHA-256     # oldest first: the frontend renames the journal past 2 MiB


def normal(line):
    """The corpus form of a journal line (None for refusals and other lines) and the text it is deduplicated by."""
    if ' ok=1 ' not in line:
        return None, None
    if line.startswith('S '):
        m = re.search(r' (lib=\S+ )?name=(\S+) k=(.*?)( \| |$)', line)
        if not m:
            return None, None
        return 'S ok=1 %sname=%s k=%s | ' % (m[1] or '', m[2], m[3]), '%s%s %s' % (m[1] or '', m[2], m[3])
    if line.startswith('C '):
        parts = line.split(' | ')
        head = parts[0].split()
        stages = [p for p in parts[1:] if p[:2] in ('k ', 'l ', 'p ')]
        if not stages:
            return None, None
        label = next((t for t in head if t.startswith('label=')), None)
        fields, stages = ' '.join(t for t in head if COMPUTE.match(t)), ' | '.join(stages)
        return 'C ok=1 %s%s | %s | ' % (fields, ' ' + label if label else '', stages), fields + ' | ' + stages
    if line.startswith('R '):
        v = line.find(' | v ')
        f = line.find(' | f ', v)
        end = line.find(' | ', f + 5) if f >= 0 else -1
        if v < 0 or f < 0:
            return None, None
        head = line[:v].split()
        label = next((t for t in head if t.startswith('label=')), 'label=-')
        state = ' '.join(t for t in head if DESCRIPTOR.match(t)) + line[v:end if end >= 0 else len(line)]
        return 'R ok=1 %s %s | ' % (label, state), state
    return None, None


@contextmanager
def opened(dirs, log):
    """Each of `dirs` (outermost first) that this user may not enter made u+rx while the block runs, then given its mode back.
    The only write the volume sees."""
    restore = []
    try:
        for d in dirs:
            try:
                mode = stat.S_IMODE(os.lstat(d).st_mode)
            except FileNotFoundError:   # nothing deeper either: the block finds no cache
                break
            if mode & stat.S_IRUSR and mode & stat.S_IXUSR:
                continue
            os.chmod(d, mode | stat.S_IRUSR | stat.S_IXUSR)
            restore.append((d, mode))
        yield
    finally:
        for d, mode in reversed(restore):
            os.chmod(d, mode)
            now = stat.S_IMODE(os.lstat(d).st_mode)
            if now != mode:
                log('MODE NOT RESTORED: %s is %o, was %o' % (d, now, mode))


def readable(d):
    return os.access(d, os.R_OK | os.X_OK)


def caches(volume, opens, open_all, log):
    """(where, m2v directory, the directories to open on the way) of every m2v cache on the volume, and the closed containers
    skipped."""
    found, skipped = [], []

    def look(where, m2v, through=()):
        found.append((where, m2v, through))

    def containers(base):
        try:
            return sorted(p for p in base.iterdir() if p.is_dir())
        except OSError:
            return []

    for kind, base in (('System', volume / 'containers/Data/System'),
                       ('Application', volume / 'mobile/Containers/Data/Application'),
                       ('PluginKitPlugin', volume / 'mobile/Containers/Data/PluginKitPlugin')):
        for c in containers(base):
            for sub in ('Library/Caches/m2v', 'tmp/m2v'):
                parts = sub.split('/')
                way = [c / '/'.join(parts[:i + 1]) for i in range(len(parts) - 1)]
                shut = [d for d in way if os.path.lexists(d) and not readable(d)]
                if not shut:
                    if (c / sub).is_dir():
                        look('%s/%s/%s' % (kind, c.name, sub), c / sub)
                elif open_all or c.name in opens:
                    look('%s/%s/%s' % (kind, c.name, sub), c / sub, tuple(way))
                elif sub.startswith('Library'):
                    skipped.append(c.name)
            # the plugin's Library/Caches/m2v-<process> of a data container whose tmp is the process's temp directory (66b8ec2:
            # WebKit's GPU process in Safari's container, a poster extension in its own)
            if readable(c / 'Library') and readable(c / 'Library/Caches'):
                for m2v in sorted((c / 'Library/Caches').glob('m2v-*')):
                    if m2v.is_dir() and readable(m2v):
                        look('%s/%s/%s' % (kind, c.name, m2v.relative_to(c)), m2v)
    for pattern in ('tmp/m2v-*', 'tmp/m2v', 'mobile/tmp/*/m2v', 'mobile/Library/Caches/m2v', 'root/Library/Caches/m2v', 'folders/*/*/C/m2v', 'folders/*/*/T/m2v'):
        for m2v in sorted(volume.glob(pattern)):
            if m2v.is_dir() and readable(m2v):
                look(str(m2v.relative_to(volume)), m2v)
    return found, skipped


def merge(corpus, sources):
    lines, seen = [], set()
    kept = [(corpus, corpus.read_text(errors='replace'))] if corpus.is_file() else []
    for name, text in kept + sources:
        added = 0
        for raw in text.splitlines():
            line, key = normal(raw.rstrip('\n'))
            if line and key not in seen:
                seen.add(key)
                lines.append(line)
                added += 1
        print('%s: %d new' % (name, added))
    corpus.write_text('\n'.join(lines) + '\n')
    print('%s: %d specializations, %d render states, %d compute states' %
          (corpus, sum(l[0] == 'S' for l in lines), sum(l[0] == 'R' for l in lines), sum(l[0] == 'C' for l in lines)))
    return lines


def collect(m2v, store):
    """A cache's libraries made from bytes, into <store>/data/."""
    for path in sorted((m2v / 'libs').glob('*.metallib')) if (m2v / 'libs').is_dir() else []:
        target = store / 'data' / path.name
        if not target.is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)


def record_ok(data):
    """Whether `data` is a whole driver record: magic, identity, a length that ends where the checksum begins, the checksum."""
    if len(data) < 8 + 28 + 8 + 32 or data[:8] != RECORD_MAGIC:
        return False
    length = int.from_bytes(data[36:44], 'little')
    return 44 + length + 32 == len(data) and hashlib.sha256(data[:-32]).digest() == data[-32:]


def records(caches, store):
    """Every driver record of each cache directory in `caches` whose envelope checks out, into `store` (by name; one already
    there is kept: a name is what the making reads, so two records of one name hold the same driver's data)."""
    seen = copied = bad = 0
    for cache in caches:
        content = Path(cache) / 'v1' / CONTENT
        for path in sorted(content.glob('*/*')) if content.is_dir() else []:
            if path.name.startswith('.') or not path.is_file():
                continue
            seen += 1
            target = store / path.parent.name / path.name
            if target.is_file():
                continue
            data = path.read_bytes()
            if not record_ok(data):
                bad += 1
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            copied += 1
    print('%s: %d driver records of %d caches, %d new, %d refused (envelope), %d in the store' %
          (store, seen, len(caches), copied, bad, sum(1 for _ in store.glob('*/*')) if store.is_dir() else 0))


def embedded_libraries():
    """The sha256 of each metallib the build makes itself: the OS volume's (bazel/gpu/apple-libs.sha256) and the frontend's."""
    made = {line.split()[0] for line in (PROJECT / 'bazel/gpu/apple-libs.sha256').read_text().splitlines()
            if line.strip() and not line.startswith('#')}
    text = (Path(__file__).resolve().parents[2] / 'NativeMetal/TwoPlaneStandIn.h').read_text()
    made.add(hashlib.sha256(bytes(int(b, 16) for b in re.findall(r'0x([0-9a-fA-F]{2})', text[text.index('{'):]))).hexdigest())
    return made


def pack(store, corpus):
    """The tar of @gpu_device_data (DEVICE_DATA in bazel/gpu/deps.bzl) from the staging directory `store`."""
    named = set(re.findall(r'lib=data:([0-9a-f]{64})', corpus.read_text()))
    own = embedded_libraries()
    members = sorted(p for p in (store / 'records').glob('*/*') if p.is_file() and record_ok(p.read_bytes()))
    members += sorted(p for p in (store / 'data').glob('*.metallib')
                      if p.stem in named and p.stem not in own and hashlib.sha256(p.read_bytes()).hexdigest() == p.stem)
    if not members:
        sys.exit('%s: no driver records or libraries staged (harvest.py --volume or --records-from first)' % store)
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for path in members:
            info = tarfile.TarInfo(str(path.relative_to(store)))
            info.size, info.mode, info.mtime = path.stat().st_size, 0o644, 0
            with path.open('rb') as f:
                tar.addfile(info, f)
    digest = hashlib.sha256(data.getvalue()).hexdigest()
    out = DEVICE / ('m2v-device-%s.tar' % digest[:16])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data.getvalue())
    print('%s: %d driver records, %d libraries (of %d made from bytes the corpus names)' %
          (out, sum(1 for p in members if p.parts[-3] == 'records'), sum(1 for p in members if p.parts[-2] == 'data'),
           len(named)))
    print('bazel/gpu/deps.bzl:\nDEVICE_DATA = {\n    "file": "%s",\n    "sha256": "%s",\n}' % (out.name, digest))


def option(args, name, many=False):
    values = []
    while name in args:
        i = args.index(name)
        if i + 1 >= len(args):
            sys.exit(__doc__)
        values.append(args.pop(i + 1))
        args.pop(i)
    return values if many else (values[-1] if values else None)


def main():
    args = sys.argv[1:]
    volume = option(args, '--volume')
    save = option(args, '--save')
    store = Path(option(args, '--device') or DEVICE / 'staging')
    if '--pack' in args:
        pack(store, Path(__file__).resolve().parent / 'corpus.log')
        return
    records_from = option(args, '--records-from', many=True)
    if records_from:
        records_from += args
        records(sorted({str(v1.parent) for d in records_from for v1 in Path(d).rglob('v1') if (v1 / CONTENT).is_dir()}),
                store / 'records')
        return
    opens = set(option(args, '--open', many=True)) or set(SYSTEM_CACHES)
    open_all = '--open-all' in args
    args = [a for a in args if a != '--open-all']
    if not args or (volume is None and len(args) < 2):
        sys.exit(__doc__)
    corpus, sources = Path(args[0]), [(p, Path(p).read_text(errors='replace')) for p in args[1:]]
    if volume:
        volume = Path(volume)
        if not (volume / 'mobile').is_dir():
            sys.exit('%s: not a mounted iPad Data volume (no mobile/)' % volume)
        found, skipped = caches(volume, opens, open_all, print)
        for where, m2v, through in found:
            with opened(through + (m2v,) if through else (), print):
                if not m2v.is_dir():
                    continue
                for journal in JOURNALS:
                    if (m2v / journal).is_file():
                        sources.append(('%s/%s' % (where, journal), (m2v / journal).read_text(errors='replace')))
                collect(m2v, store)
                records([m2v], store / 'records')
                if save:
                    target = Path(save) / where.replace('/', '_')
                    shutil.rmtree(target, ignore_errors=True)
                    shutil.copytree(m2v, target, copy_function=shutil.copyfile, ignore=shutil.ignore_patterns('.*.tmp'))
        print('%s: %d m2v caches, %d journals; %d closed containers not looked in (--open-all)' %
              (volume, len(found), len(sources) - (len(args) - 1), len(set(skipped))))
    merge(corpus, sources)


if __name__ == '__main__':
    main()
