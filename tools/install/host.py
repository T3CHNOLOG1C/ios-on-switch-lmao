"""The macOS tools the installer and deploy use, by absolute path, with plist output parsed.

These are the declared host inputs of //install:sd and //deploy:* (macOS base system, not downloadable): diskutil,
hdiutil, ditto, fsck_apfs, sw_vers, sync. Each run checks they exist and logs `sw_vers`.
"""

import datetime
import os
import plistlib
import subprocess
import sys
from pathlib import Path

TOOLS = {
    'diskutil': '/usr/sbin/diskutil',
    'hdiutil': '/usr/bin/hdiutil',
    'ditto': '/usr/bin/ditto',
    'fsck_apfs': '/sbin/fsck_apfs',
    'sw_vers': '/usr/bin/sw_vers',
    'sync': '/bin/sync',
}


class HostError(Exception):
    pass


class Log:
    """Everything printed also goes to the run's log file."""

    def __init__(self, path: str | Path):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.file = self.path.open('a')

    def __call__(self, *parts: object) -> None:
        line = ' '.join(str(p) for p in parts)
        print(line, flush=True)
        self.file.write('{} {}\n'.format(datetime.datetime.now().isoformat(timespec='seconds'), line))
        self.file.flush()

    def quiet(self, *parts: object) -> None:
        self.file.write(
            '{} {}\n'.format(datetime.datetime.now().isoformat(timespec='seconds'), ' '.join(str(p) for p in parts))
        )
        self.file.flush()


def check_tools(log: Log) -> str:
    missing = [name for name, path in TOOLS.items() if not os.access(path, os.X_OK)]
    if missing:
        raise HostError('missing macOS tools: {}'.format(', '.join(missing)))
    versions = run('sw_vers').strip().replace('\n', '; ')
    log.quiet('host:', versions, '| euid', os.geteuid())
    return versions


def run(tool: str, *args: object, check: bool = True, binary: bool = False, log: Log | None = None) -> str | bytes:
    command = [TOOLS.get(tool, tool), *map(str, args)]
    if log:
        log.quiet('$', ' '.join(command))
    result = subprocess.run(command, capture_output=True, text=not binary)
    if check and result.returncode:
        raise HostError(
            '{} failed ({}): {}'.format(
                ' '.join(command),
                result.returncode,
                (result.stderr or result.stdout or '').strip()
                if not binary
                else result.stderr.decode(errors='replace'),
            )
        )
    return result.stdout


def plist(tool: str, *args: object, log: Log | None = None) -> dict:
    return plistlib.loads(run(tool, *args, binary=True, log=log))


def disk_info(identifier: str) -> dict | None:
    try:
        return plist('diskutil', 'info', '-plist', identifier)
    except HostError:
        return None


def partitions(disk: str) -> list[dict]:
    """Diskutil's view of the whole disk's partitions: [{DeviceIdentifier, Size, Content, VolumeName, ...}]."""
    listing = plist('diskutil', 'list', '-plist', disk)
    for item in listing.get('AllDisksAndPartitions', []):
        if item.get('DeviceIdentifier') == disk:
            return item.get('Partitions', [])
    return []


def boot_disks() -> set[str]:
    """Whole disks that hold the running system (the boot volume's physical stores)."""
    disks = set()
    info = disk_info('/')
    if not info:
        raise HostError('cannot find the boot volume')
    for key in ('ParentWholeDisk',):
        if info.get(key):
            disks.add(info[key])
    container = disk_info(info.get('APFSContainerReference', '')) if info.get('APFSContainerReference') else None
    for store in (container or {}).get('APFSPhysicalStores', []):
        ident = store.get('APFSPhysicalStore', '')
        whole = disk_info(ident)
        if whole and whole.get('ParentWholeDisk'):
            disks.add(whole['ParentWholeDisk'])
    return disks


PARTITION_SCHEMES = ('GUID_partition_scheme', 'FDisk_partition_scheme')


def whole_disk_of(entities: list[dict]) -> str | None:
    """The image's own whole disk among hdiutil's system-entities (not an APFS container disk synthesized from it)."""
    for entity in entities:
        dev = entity.get('dev-entry', '')
        if (
            dev.startswith('/dev/disk')
            and 's' not in dev[len('/dev/disk') :]
            and entity.get('content-hint') in PARTITION_SCHEMES
        ):
            return dev[len('/dev/') :]
    return None


def images() -> dict[str, str]:
    """{whole disk: image path} of attached disk images."""
    out = {}
    for item in plist('hdiutil', 'info', '-plist').get('images', []):
        disk = whole_disk_of(item.get('system-entities', []))
        if disk:
            out[disk] = item.get('image-path', '')
    return out


def apfs_containers() -> list[dict]:
    return plist('diskutil', 'apfs', 'list', '-plist').get('Containers', [])


def container_on(store: str) -> dict | None:
    """The APFS container whose physical store is `store` (e.g. disk4s9), or None."""
    for container in apfs_containers():
        if any(s.get('DeviceIdentifier') == store for s in container.get('PhysicalStores', [])):
            return container
    return None


def mount_point(identifier: str) -> str | None:
    info = disk_info(identifier)
    return (info or {}).get('MountPoint') or None


def require_root_or(reason: str) -> None:
    if os.geteuid() != 0:
        raise HostError(f'{reason} needs root: rerun with sudo')


def python() -> str:
    return sys.executable
