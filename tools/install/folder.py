"""The folder //:image makes, merged onto the SD card's root, and how it is merged.

    switchroot/ios/install/<build>/   one kind's image: manifest.bin, part-NN.bin and that kind's boot entry files
    bootloader/ini/ios.ini            the hekate entry, and its icon bootloader/res/icon_ios_hue.bmp
    bootloader/payloads/install-ios.bin

The boot entry hekate starts (switchroot/ios/bl31.bin, ...) is not in the folder: install-ios.bin copies the installed
image's own there, and deploy writes it to an installed card. So the folder of one kind never changes which kernel the
card starts, and each kind's image keeps the boot files it was made with.

Merging writes only those paths. A kind's folder on the card loses the files the folder does not have, and the other
kinds' folders stay as they are. The image from before one folder per kind (switchroot/ios/install/manifest.bin and its
parts) goes, as does a copy of the entry under the project's former names (renamed.py). Nothing else is read or written.
"""

import json
import os
import re
import shutil
from collections.abc import Iterable
from pathlib import Path

from tools.install import card as cards, host, renamed

INSTALL = cards.BOOT_ENTRY + '/install'
MANIFEST = 'manifest.bin'
PAYLOAD = 'bootloader/payloads/install-ios.bin'
MARKER = '.switch-ios-image'
SHARED = (cards.INI, cards.ICON, PAYLOAD)
KIND_FILE = re.compile(re.escape(INSTALL) + r'/([0-9A-Za-z]+)/([^/]+)')


def kind_dir(build: str) -> str:
    if not re.fullmatch(r'[0-9A-Za-z]+', build):
        raise host.HostError(f'{build!r} is not an IPSW build')
    return f'{INSTALL}/{build}'


def kind_of(relative: str) -> str | None:
    """Return the kind whose image folder holds `relative`, None for any other path."""
    found = KIND_FILE.fullmatch(relative)
    return found.group(1) if found else None


def _allowed(relative: str) -> bool:
    return relative in SHARED or kind_of(relative) is not None


def records(out: Path) -> dict[str, dict]:
    """Return {build: what finish() recorded} for each image the folder holds."""
    try:
        saved = json.loads((Path(out) / MARKER).read_text())
    except (OSError, ValueError):
        return {}
    return saved.get('images', {}) if isinstance(saved, dict) else {}


def record(out: Path, build: str, entry: dict) -> None:
    saved = records(out)
    saved[build] = entry
    (Path(out) / MARKER).write_text(json.dumps({'images': saved}, indent=2) + '\n')


def _clear(directory: Path, keep: set[Path]) -> None:
    """Remove everything in `directory` but the `keep` paths and the folders that lead to them."""
    for path in directory.iterdir():
        if path in keep:
            continue
        if any(path in k.parents for k in keep):
            _clear(path, keep)
        elif path.is_dir() and not path.is_symlink():
            shutil.rmtree(path)
        else:
            path.unlink()


def prepare(out: str | Path, build: str) -> Path:
    """Return `out` ready for `build`'s image.

    A new folder, or one an earlier run made: that keeps the other kinds' images and loses the rest. Refuses any other
    directory.
    """
    out = Path(out)
    kept = {}
    if out.exists():
        if any(out.iterdir()) and not (out / MARKER).is_file():
            raise host.HostError(f'{out} is not empty and was not made by //:image: give --out a new folder')
        kept = {k: v for k, v in records(out).items() if k != build and (out / kind_dir(k)).is_dir()}
        _clear(out, {out / MARKER} | {out / kind_dir(k) for k in kept})
    (out / kind_dir(build)).mkdir(parents=True)
    (out / MARKER).write_text(json.dumps({'images': kept}, indent=2) + '\n')
    return out


def add(out: Path, relative: str, source: str | Path) -> None:
    if not _allowed(relative):
        raise host.HostError(f'{relative} is not a path the folder may hold')
    target = Path(out) / relative
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    target.chmod(0o644)


def boot_files(package: str | Path, build: str) -> dict[str, Path]:
    """{path in the folder: source} of the boot package: its boot entry into the kind's folder, the ini and icon."""
    entry = cards.BOOT_ENTRY + '/'
    return {
        f'{kind_dir(build)}/{Path(relative).name}' if relative.startswith(entry) else relative: source
        for relative, source in cards.boot_entry_files(package).items()
    }


def listing(package: str | Path, build: str) -> list[str]:
    """Return what a finished folder for `build` puts on the card."""
    image = [f'{kind_dir(build)}/{MANIFEST}', f'{kind_dir(build)}/part-NN.bin']
    return sorted([*boot_files(package, build), *image, PAYLOAD])


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


def remove_legacy(fat_root: Path) -> list[str]:
    """Remove the image from before one folder per kind: the files directly in switchroot/ios/install/."""
    install = Path(fat_root) / INSTALL
    removed = []
    if install.is_dir():
        for path in sorted(install.iterdir()):
            if path.is_file():
                path.unlink()
                removed.append(f'{INSTALL}/{path.name}')
    return removed


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
    removed = remove_legacy(fat_root)
    for kind in sorted({kind_of(r) for r in kept} - {None}):
        folder = fat_root / kind_dir(kind)
        for path in sorted(folder.iterdir()) if folder.is_dir() else []:
            relative = f'{kind_dir(kind)}/{path.name}'
            if path.is_file() and relative not in kept:
                path.unlink()
                removed.append(relative)
    removed += renamed.remove_older_boot_entries(fat_root, cards.INI, cards.BOOT_ENTRY, False)
    host.run('sync')
    log(f'card: {len(changed)} files written, {len(wanted) - len(changed)} unchanged, {len(removed)} stale removed')
    return changed, removed
