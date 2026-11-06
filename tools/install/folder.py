"""The folder //:image makes, merged onto the SD card's root, and how it is merged.

    switchroot/ios/            the boot package's files and install/ (manifest.bin, part-NN.bin)
    bootloader/ini/ios.ini     the hekate entry, and its icon bootloader/res/icon_ios.bmp
    bootloader/payloads/install-ios.bin

Merging writes only those paths: switchroot/ios/ and its install/ lose files the folder does not have (except the
user's bootargs.txt), and a copy of the entry under the project's former names goes (renamed.py). Nothing else on the
card is read or written.
"""

import os
import shutil
from collections.abc import Iterable
from pathlib import Path

from tools.install import card as cards, host, renamed

INSTALL = cards.BOOT_ENTRY + '/install'
MANIFEST_PATH = INSTALL + '/manifest.bin'
MARKER = '.switch-ios-image'
USER_FILES = {'bootargs.txt'}
PATHS = (cards.BOOT_ENTRY + '/', cards.INI, cards.ICON, 'bootloader/payloads/install-ios.bin')


def prepare(out: str | Path) -> Path:
    """Return an empty `out`, or one an earlier run made (its old contents go). Refuses any other directory."""
    out = Path(out)
    if out.exists():
        if any(out.iterdir()) and not (out / MARKER).is_file():
            raise host.HostError(f'{out} is not empty and was not made by //:image: give --out a new folder')
        shutil.rmtree(out)
    (out / INSTALL).mkdir(parents=True)
    (out / MARKER).write_text('{}\n')
    return out


def _allowed(relative: str) -> bool:
    return any(relative == p or (p.endswith('/') and relative.startswith(p)) for p in PATHS)


def add(out: Path, relative: str, source: str | Path) -> None:
    if not _allowed(relative):
        raise host.HostError(f'{relative} is not a path the folder may hold')
    target = Path(out) / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(0o644)


def add_boot_package(out: Path, package: str | Path) -> None:
    package = Path(package)
    for top in ('switchroot', 'bootloader'):
        for path in sorted((package / top).rglob('*')):
            if path.is_file():
                add(out, path.relative_to(package).as_posix(), path)


def files(out: Path) -> list[str]:
    """List what of the folder goes onto the card: the paths it may hold, never what is kept beside them for the Mac."""
    out = Path(out)
    found = (p.relative_to(out).as_posix() for p in out.rglob('*') if p.is_file() and not p.name.startswith('._'))
    return sorted(relative for relative in found if _allowed(relative))


def needed_bytes(out: Path, fat_root: Path) -> int:
    total = 0
    for relative in files(out):
        target = Path(fat_root) / relative
        if not (target.is_file() and target.stat().st_size == (Path(out) / relative).stat().st_size):
            total += (Path(out) / relative).stat().st_size
    return total


def merge(out: Path, fat_root: Path, log: host.Log, keep: Iterable[str] = ()) -> tuple[list[str], list[str]]:
    """Copy the folder onto a mounted FAT.

    Unchanged files (same size and sha256) are skipped; `keep` names files already on the card that belong to it.
    """
    out, fat_root = Path(out), Path(fat_root)
    wanted = files(out)
    kept = set(wanted) | set(keep)
    changed = []
    for relative in wanted:
        source, target = out / relative, fat_root / relative
        if (
            target.is_file()
            and target.stat().st_size == source.stat().st_size
            and cards.sha256_file(target) == cards.sha256_file(source)
        ):
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        partial = target.with_name('.switch-partial-' + target.name)
        shutil.copyfile(source, partial)
        os.replace(partial, target)
        changed.append(relative)
    removed = []
    for directory in (cards.BOOT_ENTRY, INSTALL):
        folder = fat_root / directory
        if not folder.is_dir():
            continue
        for path in sorted(folder.iterdir()):
            relative = f'{directory}/{path.name}'
            if path.is_file() and relative not in kept and path.name not in USER_FILES:
                path.unlink()
                removed.append(relative)
    removed += renamed.remove_older_boot_entries(fat_root, cards.INI, cards.BOOT_ENTRY, False)
    host.run('sync')
    log(f'card: {len(changed)} files written, {len(wanted) - len(changed)} unchanged, {len(removed)} stale removed')
    return changed, removed
