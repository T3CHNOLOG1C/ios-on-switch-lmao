"""A pretranslated delta (//translate/arm64e/pretranslate, iOS 27) applied to a mounted volume.

The manifest's "files" list [{"path": volume path, "file": replacement, relative to the manifest, "sha256_in",
"sha256_out"}]. Each file is rewritten in place, so its inode, owner, mode, flags and other attributes stay; a
compressed file is stored uncompressed afterwards.
"""

import json
import os
import shutil
from pathlib import Path

from tools.install import card as cards, host

COMPRESSED = 0x20  # UF_COMPRESSED: decmpfs, which a write in place drops


class DeltaError(host.HostError):
    pass


def load(manifest):
    manifest = Path(manifest)
    entries = json.loads(manifest.read_text())['files']
    for e in entries:
        if not e['path'].startswith('/') or '..' in Path(e['path']).parts:
            raise DeltaError('{}: {!r} is not an absolute volume path'.format(manifest, e['path']))
        e['source'] = manifest.parent / e['file']
    return entries


def entries_under(entries, prefix):
    """The entries below `prefix` (e.g. /private/var), with paths relative to it: Data holds a copy of that tree."""
    prefix = prefix.rstrip('/') + '/'
    return [dict(e, path='/' + e['path'][len(prefix) :]) for e in entries if e['path'].startswith(prefix)]


def apply(root, entries, log, what, consume=False):
    """Rewrite each entry's file below `root`. A file already holding sha256_out is left alone (a resumed run).
    `consume` deletes each replacement once written, so a delta and its volume never both hold the bytes."""
    changed = 0
    for e in entries:
        target = Path(root) / e['path'].lstrip('/')
        if not target.is_file() or target.is_symlink():
            raise DeltaError('{}: {} is not a regular file'.format(what, e['path']))
        current = cards.sha256_file(target)
        if current == e['sha256_out']:
            continue
        if current != e['sha256_in']:
            raise DeltaError('{}: {} is not the file the delta was made from'.format(what, e['path']))
        flags = os.lstat(target).st_flags
        if flags & ~COMPRESSED:
            os.chflags(target, 0)
        with open(e['source'], 'rb') as source, open(target, 'r+b') as out:
            out.truncate(0)
            shutil.copyfileobj(source, out, 8 << 20)
            out.flush()
            os.fsync(out.fileno())
        if flags & ~COMPRESSED:
            os.chflags(target, flags & ~COMPRESSED)
        if cards.sha256_file(target) != e['sha256_out']:
            raise DeltaError('{}: {} does not hold the translated bytes after the write'.format(what, e['path']))
        if consume:
            e['source'].unlink()
        changed += 1
    log('{}: {} of {} files translated'.format(what, changed, len(entries)))
    return changed
