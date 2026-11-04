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
--save    also copy each m2v cache found, whole (v1/, msl/, libs/, the journals), to <dir>/<where it was>, e.g. the boot's log folder.
--libs    the library store precompile.mm reads (libraries.py; default out/gpu-artifacts/m2v-libs): with --volume, every cache's
          libs/<SHA-256>.metallib (the libraries made from bytes) is copied to <store>/data/, every msl/*.reply to <store>/msl/, and
          every library the corpus names by a path on the Data volume (an app's bundle) to <store>/root/<path>.
"""
import os
import re
import shutil
import stat
import sys
from contextlib import contextmanager
from pathlib import Path

DESCRIPTOR = re.compile(r'(samples|topo|ds|opt|c\d|a\d+)=')
COMPUTE = re.compile(r'(form|opt|max|tgm|icb|depth|abf|mut)=')
STORE = Path(__file__).resolve().parents[7] / 'out/gpu-artifacts/m2v-libs'   # tree/drivers/TegraGM20BMetal/precompile -> project
# /private/var/containers/Data/System/<UUID>/Library/Caches/m2v of the system containers whose processes kept an m2v cache there
# (the plugin's "device ... ready in" lines, r5d and r6h): their Library and Library/Caches are mode 000.
SYSTEM_CACHES = {'6C7A65AF-CB18-421C-A07A-D724B05A7AB6': 'backboardd', '532E562F-2888-4C55-9BF8-E6F0B246BBDA': 'SpringBoard',
                 '5BDBB9F1-C07A-4290-9D80-EA49A6BA2D68': 'Preferences', 'AA90E4B6-B0E4-4776-8E70-A4444E65642F': 'Spotlight'}
JOURNALS = ('pipelines.log.old', 'pipelines.log')     # oldest first: the frontend renames the journal past 2 MiB


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


def collect(volume, m2v, store):
    """A cache's libraries made from bytes and the compiler's replies, into the store."""
    for sub, pattern, into in (('libs', '*.metallib', 'data'), ('msl', '*.reply', 'msl')):
        for path in sorted((m2v / sub).glob(pattern)) if (m2v / sub).is_dir() else []:
            target = store / into / path.name
            if not target.is_file():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(path, target)


def bundles(volume, lines, store):
    """The libraries the corpus names by a path on the Data volume (/private/var/..., an app's bundle), into the store."""
    copied = 0
    for path in sorted({m for line in lines for m in re.findall(r' lib=(/\S+)', line)}):
        device = path.replace('%20', ' ').replace('%7C', '|').replace('%25', '%')
        relative = re.sub(r'^/(private/)?var/', '', device) if re.match(r'^/(private/)?var/', device) else None
        target = store / 'root/private/var' / relative if relative else None   # one spelling, as libraries.py's index
        if relative and not target.is_file() and (volume / relative).is_file():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(volume / relative, target)
            copied += 1
    print('%s: %d libraries of app bundles copied' % (store, copied))


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
    store = Path(option(args, '--libs') or STORE)
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
                collect(volume, m2v, store)
                if save:
                    target = Path(save) / where.replace('/', '_')
                    shutil.rmtree(target, ignore_errors=True)
                    shutil.copytree(m2v, target, copy_function=shutil.copyfile, ignore=shutil.ignore_patterns('.*.tmp'))
        print('%s: %d m2v caches, %d journals; %d closed containers not looked in (--open-all)' %
              (volume, len(found), len(sources) - (len(args) - 1), len(set(skipped))))
    lines = merge(corpus, sources)
    if volume:
        bundles(volume, lines, store)


if __name__ == '__main__':
    main()
