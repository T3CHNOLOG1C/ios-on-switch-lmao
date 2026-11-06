#!/usr/bin/env python3
"""The corpus: every metallib on the 21H461 OS and SystemOS cryptex volumes, deduplicated by content.

With the ones embedded in dyld-cache images (dsc_metallibs.py).
usage: make_corpus.py <metal2 dir>  -> corpus.tsv (tag, path), corpus_all.tsv (tag, path, sha256, size, duplicate-of).
"""

import hashlib
import re
import sys
from pathlib import Path

base = Path(sys.argv[1])
files = sorted(p for root in (base / 'mnt/os', base / 'mnt/cryptex') for p in root.rglob('*.metallib') if p.is_file())
files += sorted((base / 'corpus/dsc').glob('*.metallib'))


def tag(p: Path) -> str:
    s = str(p)
    if '/corpus/dsc/' in s:
        return 'dsc:' + p.name.split('@')[0] + '/' + p.stem.split('@')[1]
    parts = p.parts
    owner = next(
        (
            x
            for x in reversed(parts[:-1])
            if re.search(r'\.(framework|bundle|app|appex|xpc|cifilter|videoprocessor)$', x)
        ),
        parts[-2],
    )
    owner = re.sub(r'\.(framework|bundle|app|appex|xpc|cifilter)$', '', owner)
    if 'AvatarKit.framework/stickers' in s:
        owner = 'AvatarKit-sticker:' + '/'.join(parts[parts.index('stickers') + 1 : -1])
    return owner + '/' + p.stem


seen, rows, uniq = {}, [], []
for p in files:
    h = hashlib.sha256(p.read_bytes()).hexdigest()
    t = tag(p)
    dup = seen.get(h)
    rows.append((t, str(p), h, p.stat().st_size, dup or ''))
    if not dup:
        seen[h] = t
        uniq.append((t, str(p)))
(base / 'corpus_all.tsv').write_text(''.join('%s\t%s\t%s\t%d\t%s\n' % r for r in rows))
(base / 'corpus.tsv').write_text(''.join('%s\t%s\n' % r for r in uniq))
print(len(rows), 'files', len(uniq), 'unique', sum(r[3] for r in rows) >> 20, 'MiB')
