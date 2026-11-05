"""repath.py PATCH...: rewrite patches made before the layout move (S3) for the new paths, in place.

Every line, diff headers and hunks alike, goes through the move's own rules (rules.py). Hunks against the old kernel
tree BUILD.bazel cannot be repathed (its blocks moved into many packages): each is listed with the target it touches
and the package BUILD.bazel that target now lives in, to redo there by hand.

    bazel run //tools/repath -- $PWD/0001-my-change.patch
"""

import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rules

HERE = Path(__file__).resolve().parent
OLD_BUILD = rules.OLD_TREE + '/BUILD.bazel'
HUNK = re.compile(r'^@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@')
NAME = re.compile(r'^[ +-]\s*name = "([^"]+)"')
NO_TARGET = '(no target: a load, constant or comment)'
NO_PACKAGE = 'kernel/rules/driver_flags.bzl or kernel/BUILD.bazel'


def old_build_hunks(text: str) -> list[tuple[int, list[str]]]:
    """Return (old start line, hunk lines) of each hunk against the old tree BUILD.bazel."""
    hunks: list[tuple[int, list[str]]] = []
    inside, current = False, None
    for line in text.split('\n'):
        if line.startswith('diff --git '):
            inside = line == f'diff --git a/{OLD_BUILD} b/{OLD_BUILD}'
            current = None
        elif inside and (match := HUNK.match(line)):
            current = (int(match.group(1)), [])
            hunks.append(current)
        elif inside and current is not None:
            current[1].append(line)
    return hunks


def owners(start: int, lines: list[str], spans: list[list], labels: dict[str, str]) -> list[str]:
    """Return the targets a hunk touches: names in its lines, else the old block its first changed line falls in."""
    names = [m.group(1) for m in map(NAME.match, lines) if m and m.group(1) in labels]
    if names:
        return names
    line = start
    for text in lines:
        if text.startswith(('+', '-')):
            break
        line += 1
    return [name for first, last, name, _ in spans if first <= line <= last]


def main() -> None:
    """Rewrite each patch named on the command line in place and list its hunks against the old tree BUILD."""
    labels = rules.load_labels(HERE / 'labels.json')
    spans = json.loads((HERE / 'blocks.json').read_text())
    base = Path(os.environ.get('BUILD_WORKING_DIRECTORY', '.'))
    for name in sys.argv[1:]:
        path = base / name
        text = path.read_text()
        for start, lines in old_build_hunks(text):
            targets = owners(start, lines, spans, labels) or [NO_TARGET]
            where = sorted({labels[t].split(':')[0][2:] + '/BUILD.bazel' for t in targets if t in labels})
            packages = ', '.join(where) or NO_PACKAGE
            print(f'{name}: hunk at {OLD_BUILD}:{start} ({", ".join(targets)}) -> redo in {packages}')
        new = rules.rewrite(text, labels)
        path.write_text(new)
        print(f'{name}: {"rewritten" if new != text else "unchanged"}')


if __name__ == '__main__':
    main()
