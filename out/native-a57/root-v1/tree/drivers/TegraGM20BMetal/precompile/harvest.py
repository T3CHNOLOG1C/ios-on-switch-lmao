#!/usr/bin/env python3
"""Merge device journals (the frontend's pipelines.log: S lines, one per function specialization, and R lines, one per render
pipeline state) into the precompile corpus: each specialization and state made once, by any process, in the order first seen,
without the times, threads, costs and callers. The corpus keeps what it had; new journals only add. A line names no library:
precompile.mm replays QuartzCore's and skips the rest (Core Image's kernels, an app's own library), counted.

usage: harvest.py <corpus.log> <pipelines.log>...    e.g. harvest.py corpus.log out/switch/device-logs/*/m2v-cache/pipelines.log
       harvest.py <corpus.log> --volume <mounted Data volume> [--save <dir>] [--open <container UUID>]... [--open-all]

--volume  every process's m2v cache on the iPad's Data volume (the plugin's cacheCandidates: a container's Library/Caches/m2v
          or tmp/m2v, tmp/m2v-<process>, a daemon's user cache or temp directory), its pipelines.log and pipelines.log.old. Read
          only, but for one write: a system container's Library and Library/Caches are mode 000, so for a container named by
          --open (default: backboardd's, SpringBoard's and Preferences', SYSTEM_CACHES) or every one with --open-all, each closed
          directory on the way is made u+rx for the read and given its mode back at once (and checked); other closed containers
          are skipped and counted.
--save    also copy each m2v cache found, whole (v1/, msl/, the journals), to <dir>/<where it was>, e.g. the boot's log folder.
"""
import os
import re
import shutil
import stat
import sys
from contextlib import contextmanager
from pathlib import Path

DESCRIPTOR = re.compile(r'(samples|topo|ds|c\d|a\d+)=')
# /private/var/containers/Data/System/<UUID>/Library/Caches/m2v of the system containers whose processes kept an m2v cache there
# (the plugin's "device ... ready in" lines, r5d): their Library and Library/Caches are mode 000.
SYSTEM_CACHES = {'6C7A65AF-CB18-421C-A07A-D724B05A7AB6': 'backboardd', '532E562F-2888-4C55-9BF8-E6F0B246BBDA': 'SpringBoard',
                 '5BDBB9F1-C07A-4290-9D80-EA49A6BA2D68': 'Preferences'}
JOURNALS = ('pipelines.log.old', 'pipelines.log')     # oldest first: the frontend renames the journal past 2 MiB


def normal(line):
    """The corpus form of a journal line (None for refusals and other lines) and the text it is deduplicated by."""
    if ' ok=1 ' not in line:
        return None, None
    if line.startswith('S '):
        m = re.search(r' name=(\S+) k=(.*?)( \| |$)', line)
        if not m:
            return None, None
        return 'S ok=1 name=%s k=%s | ' % (m[1], m[2]), m[1] + ' ' + m[2]
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
    print('%s: %d specializations, %d states' % (corpus, sum(l[0] == 'S' for l in lines), sum(l[0] == 'R' for l in lines)))


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
                if save:
                    target = Path(save) / where.replace('/', '_')
                    shutil.rmtree(target, ignore_errors=True)
                    shutil.copytree(m2v, target, copy_function=shutil.copyfile, ignore=shutil.ignore_patterns('.*.tmp'))
        print('%s: %d m2v caches, %d journals; %d closed containers not looked in (--open-all)' %
              (volume, len(found), len(sources) - (len(args) - 1), len(set(skipped))))
    merge(corpus, sources)


if __name__ == '__main__':
    main()
