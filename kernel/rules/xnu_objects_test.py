"""Each component's objects.bzl names the objects doconf's Makefile lists, in its order.

objects.bzl is what the native build compiles and links; doconf's Makefile (r18-xnu-hdrs' config_makefile_<comp>,
from conf/files and the MASTER options) is what XNU's make would build. A conf/files or MASTER edit that adds, drops or
moves an object fails here until objects.bzl follows it.

usage: xnu_objects_test.py NAMES.txt MAKEFILE...   (NAMES.txt: "<component> <object>" lines; each MAKEFILE's path names
       its component: .../<component>/<CONFIG>/Makefile)
"""

import sys
from pathlib import Path


def make_objects(makefile: Path) -> list[str]:
    """Return a doconf Makefile's OBJS= list (backslash-continued), in order."""
    text = makefile.read_text()
    at = text.find('\nOBJS=')
    if at < 0:
        sys.exit(f'{makefile}: no OBJS=')
    words = []
    for line in text[at + len('\nOBJS=') :].splitlines():
        words += line.rstrip().rstrip('\\').split()
        if not line.rstrip().endswith('\\'):
            return words
    return words


def main() -> None:
    """Compare every component's objects with its Makefile's."""
    ours: dict[str, list[str]] = {}
    for line in Path(sys.argv[1]).read_text().splitlines():
        component, name = line.split(' ')
        ours.setdefault(component, []).append(name)
    failed = False
    for makefile in map(Path, sys.argv[2:]):
        component = makefile.parent.parent.name
        theirs = make_objects(makefile)
        mine = ours.get(component, [])
        if mine != theirs:
            failed = True
            missing = [n for n in theirs if n not in mine]
            extra = [n for n in mine if n not in theirs]
            print(f'{component}: objects.bzl differs from {makefile}')
            print(f'  missing from objects.bzl: {missing[:20]}')
            print(f'  not in the Makefile: {extra[:20]}')
            if not missing and not extra:
                at = next(i for i, (a, b) in enumerate(zip(mine, theirs, strict=True)) if a != b)
                print(f'  same objects, order differs from #{at}: {mine[at : at + 3]} vs {theirs[at : at + 3]}')
    if failed:
        sys.exit(1)
    print(f'{len(ours)} components match doconf')


if __name__ == '__main__':
    main()
