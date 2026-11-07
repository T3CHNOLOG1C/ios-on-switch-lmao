"""Disk images attached and volumes mounted only while a step uses them, whatever ends the step.

attached() and mounted() undo themselves on every exit; under interrupts() SIGTERM and SIGHUP end a run the way ^C
does, so their cleanup runs then too. A run that died anyway (SIGKILL, a power cut) leaves attachments behind:
sweep() detaches every image still attached from below a directory before the next run uses it, and removes the
partial files a run writes before renaming them into place.
"""

import contextlib
import os
import re
import shutil
import signal
import tempfile
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from types import FrameType

from tools.install import host

APFS_VOLUME = '41504653-0000-11AA-AA11-00306543ECAC'
PARTIAL = '.partial-'
FSEVENTS = '.fseventsd'
ATTACH = ('-nomount', '-noverify', '-noautofsck')
RAW = ('-imagekey', 'diskimage-class=CRawDiskImage')


@dataclass(frozen=True)
class Attachment:
    whole: str
    volumes: list[str]


def _entities(image: Path, writable: bool, raw: bool, log: host.Log) -> list[dict]:
    mode = ('-readwrite',) if writable else ('-readonly',)
    args = ['attach', '-plist', *mode, *(RAW if raw else ()), *ATTACH, str(image)]
    return host.plist('hdiutil', *args, log=log)['system-entities']


@contextlib.contextmanager
def attached(image: Path, log: host.Log, writable: bool = True, raw: bool = False) -> Iterator[Attachment]:
    """Attach `image` without mounting it; `raw` for a file of bytes with no image header (a cryptex, our scratch)."""
    entities = _entities(Path(image), writable, raw, log)
    disks = [e['dev-entry'][5:] for e in entities if re.fullmatch(r'/dev/disk\d+', e.get('dev-entry', ''))]
    if not disks:
        raise host.HostError(f'hdiutil attach gave no disk for {image}')
    volumes = [
        e['dev-entry'][5:]
        for e in entities
        if e.get('content-hint', '').upper() == APFS_VOLUME and re.fullmatch(r'/dev/disk\d+s\d+', e['dev-entry'])
    ]
    try:
        yield Attachment(disks[0], volumes)
    finally:
        host.run('hdiutil', 'detach', disks[0], log=log, check=False)


@contextlib.contextmanager
def mounted(device: str, log: host.Log, owners: bool, read_only: bool = False) -> Iterator[Path]:
    """Mount a volume at a private mount point, hidden from the Finder and Spotlight.

    `owners` shows the files' owners as they are; without, every file reads and writes as the user and new files are
    the user's (owners.py gives them theirs afterwards). On a writable volume fseventsd is told to log nothing
    (quiet()) and its directory goes before unmounting (settle()).
    """
    with tempfile.TemporaryDirectory(prefix='switch-') as directory:
        options = 'nobrowse,' + ('owners' if owners else 'noowners')
        mode = ['readOnly'] if read_only else []
        host.run('diskutil', 'mount', *mode, '-mountOptions', options, '-mountPoint', directory, device, log=log)
        try:
            if not read_only:
                quiet(Path(directory))
            yield Path(directory)
        finally:
            if not read_only:
                settle(Path(directory))
            host.run('diskutil', 'unmount', device, log=log, check=False)


def quiet(root: Path) -> None:
    """Make fseventsd log nothing on the volume mounted at `root` (its no_log marker).

    fseventsd makes .fseventsd on a volume when it is mounted, at a moment of its own, and writes its log there when
    the volume is unmounted: files that would land after their owners were recorded. Removing the directory alone
    races with its making.
    """
    events = root / FSEVENTS
    events.mkdir(exist_ok=True)
    (events / 'no_log').touch()


def settle(root: Path) -> None:
    """Remove .fseventsd: the device makes its own, and a cryptex has none. With no_log nothing writes it again."""
    shutil.rmtree(root / FSEVENTS, ignore_errors=True)


def sweep(directory: Path, log: host.Log) -> list[str]:
    """Detach every image attached from below `directory` and delete its partial files; return what was done."""
    root = Path(directory).resolve()
    done = []
    for item in host.plist('hdiutil', 'info', '-plist').get('images', []):
        path = Path(item.get('image-path', ''))
        if not path.is_relative_to(root):
            continue
        entities = item.get('system-entities', [])
        whole = [e['dev-entry'] for e in entities if re.fullmatch(r'/dev/disk\d+', e.get('dev-entry', ''))]
        if whole:
            host.run('hdiutil', 'detach', '-force', whole[0], log=log, check=False)
            done.append(f'detached {path.name} ({whole[0]})')
    if root.is_dir():
        for leftover in root.rglob(PARTIAL + '*'):
            if leftover.is_dir() and not leftover.is_symlink():
                shutil.rmtree(leftover)
            else:
                leftover.unlink()
            done.append(f'removed {leftover.name}')
    for line in done:
        log(f'cleanup: {line}')
    return done


def partial(path: Path) -> Path:
    """Where `path` is written before os.replace() puts it in place, so a run that stops leaves only a partial."""
    return Path(path).with_name(PARTIAL + Path(path).name)


def _raise_interrupt(signum: int, _frame: FrameType | None) -> None:
    raise KeyboardInterrupt(signal.Signals(signum).name)


@contextlib.contextmanager
def interrupts() -> Iterator[None]:
    """Make SIGTERM and SIGHUP end the run as ^C does, unwinding every attached() and mounted() on the way out."""
    caught = (signal.SIGTERM, signal.SIGHUP)
    previous = {s: signal.signal(s, _raise_interrupt) for s in caught}
    try:
        yield
    finally:
        for s, handler in previous.items():
            signal.signal(s, handler)


def free_bytes(path: str | os.PathLike) -> int:
    """Free bytes on the filesystem holding `path`, or its nearest existing parent."""
    path = Path(path)
    while not path.exists():
        path = path.parent
    return shutil.disk_usage(path).free
