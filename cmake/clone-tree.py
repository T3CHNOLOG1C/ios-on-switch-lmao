#!/usr/bin/env python3
"""Make an APFS copy-on-write clone of a kernel tree that builds on its own.

Usage: clone-tree.py <tree> <new-tree>

The tree's generated XNU makefiles, object file lists, .d files, compile records and
.CFLAGS files hold the tree's absolute path. After `cp -cR`, text files and symlinks that
mention the old path are rewritten to the new one, keeping their modification times so
make and ninja see nothing new. Binary files (objects, archives, kernels) are left alone.
Objects compiled before the clone keep the old path in their strings and debug info, so
to compare a clone's kernel byte for byte with the original's, give the clone a path of
the same length."""
import os
import subprocess
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    old, new = (Path(p).absolute() for p in sys.argv[1:])
    if new.exists():
        raise SystemExit(f'{new} already exists')
    new.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['/bin/cp', '-cR', str(old), str(new)], check=True)
    before, after = str(old).encode(), str(new).encode()
    texts = links = 0
    for root, dirs, names in os.walk(new):
        for name in dirs + names:
            path = Path(root) / name
            if path.is_symlink():
                target = os.readlink(path)
                if target.startswith(str(old)):
                    path.unlink()
                    path.symlink_to(str(new) + target[len(str(old)):])
                    links += 1
            elif name in names and path.is_file():
                data = path.read_bytes()
                if before not in data:
                    continue
                try:
                    data.decode()
                except UnicodeDecodeError:
                    continue
                stat = path.stat()
                path.write_bytes(data.replace(before, after))
                os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
                texts += 1
    # cp gives directories the current time; XNU's make treats some as prerequisites
    # ($(OBJROOT)/SETUP/config regenerates every component Makefile), so copy them over too.
    for root, dirs, _ in os.walk(old):
        for name in ['.', *dirs]:
            source = Path(root) / name
            if not source.is_symlink():
                stat = source.stat()
                os.utime(new / source.relative_to(old), ns=(stat.st_atime_ns, stat.st_mtime_ns))
    print(f'{new}: {texts} text files and {links} symlinks rebased')
    if len(before) != len(after):
        print('note: the clone path length differs from the original; prebuilt objects still embed the old path')


if __name__ == '__main__':
    main()
