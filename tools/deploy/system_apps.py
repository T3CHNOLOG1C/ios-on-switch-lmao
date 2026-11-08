"""System apps the overlay adds under /Applications, registered the way iOS registers an OS update's.

LaunchServices scans the system app directories only during datamigrator's upgrade migration
(00LaunchServicesMigrator.migrator's populateLSDatabase), which runs when the build recorded in com.apple.migration
differs from the running one, or when /private/var/datamigrator/migrate exists: the sentinel datamigrator's own
forceMigrationOnNextReboot writes and removes once the migration has run. The deploy writes that sentinel when the
overlay's /Applications changed, so the next boot registers the new bundles and drops the removed ones.

The kernel runs a migration once: its marker (MARKER) left from an earlier migration makes a sentinel it finds at boot
read as a migration that never ended, which it deletes unrun. So a requested migration also removes the marker.

The same migration installs the removable apps a restore leaves in /private/var/staged_system_apps (MISystemAppMigrator)
and consumes each staged copy whether installd accepts it or not. An app MobileInstallation still wants installed that
Data holds neither staged nor installed is copied back from the System volume (restage), so the next migration
installs it.
"""

import os
import plistlib
import shutil
import stat
from pathlib import Path

from tools.install import card as cards, host, renamed

SENTINEL = 'datamigrator/migrate'
MARKER = f'{renamed.DATA_FOLDER}/system-app-migration'
APPLICATIONS = 'overlay/Applications'
STAGED = 'staged_system_apps'
INSTALLED = 'containers/Bundle/Application'
APP_STATES = 'installd/Library/MobileInstallation/BackedUpState/SystemAppInstallState.plist'
# MobileInstallation's state of an app its migration installs (24B119 marks Fitness 2, uninstalled, and Vision Pro 6).
WANTED = 1
MOBILE = 501


def digest(data_root: Path) -> str | None:
    return cards.tree_digest(Path(data_root) / renamed.DATA_FOLDER / APPLICATIONS)


def request_migration(data_root: Path, before: str | None, log: host.Log, force: bool = False) -> bool:
    """Write the migration sentinel when the overlay's apps differ from `before`, or when `force`.

    `before` is the apps' digest ahead of the sync.
    """
    if not force and digest(data_root) == before:
        return False
    sentinel = Path(data_root) / SENTINEL
    sentinel.parent.mkdir(parents=True, exist_ok=True)
    sentinel.touch()
    (Path(data_root) / MARKER).unlink(missing_ok=True)
    why = 'requested' if force else 'system apps changed'
    log(f'{why}: upgrade migration on the next boot (<Data>/{SENTINEL})')
    return True


def _info(app: Path) -> dict:
    try:
        return plistlib.loads((app / 'Info.plist').read_bytes())
    except (OSError, plistlib.InvalidFileException):
        return {}


def _wanted(data_root: Path) -> set[str]:
    try:
        states = plistlib.loads((data_root / APP_STATES).read_bytes())
    except (OSError, plistlib.InvalidFileException):
        return set()
    return {bundle_id for bundle_id, state in states.items() if state == WANTED}


def _installed(data_root: Path) -> set[str]:
    """Names of the bundles Data holds installed with their executable (a placeholder has none)."""
    names = set()
    for app in (data_root / INSTALLED).glob('*/*.app'):
        executable = _info(app).get('CFBundleExecutable')
        if executable and (app / executable).is_file():
            names.add(app.name)
    return names


def missing(system_root: Path, data_root: Path) -> list[str]:
    """Return the System volume's staged apps MobileInstallation wants that Data holds neither staged nor installed."""
    system_root, data_root = Path(system_root), Path(data_root)
    wanted = _wanted(data_root)
    if not wanted:
        return []
    held = _installed(data_root) | {app.name for app in (data_root / STAGED).glob('*.app')}
    staged = sorted((system_root / 'private/var' / STAGED).glob('*.app'))
    return [app.name for app in staged if app.name not in held and _info(app).get('CFBundleIdentifier') in wanted]


def _copy_bundle(source: Path, target: Path) -> None:
    target.mkdir()
    for entry in sorted(source.iterdir()):
        copied = target / entry.name
        if entry.is_symlink():
            copied.symlink_to(os.readlink(entry))
        elif entry.is_dir():
            _copy_bundle(entry, copied)
        else:
            shutil.copyfile(entry, copied)
        if not entry.is_symlink():
            copied.chmod(stat.S_IMODE(entry.stat().st_mode))
        if os.geteuid() == 0:
            os.lchown(copied, MOBILE, MOBILE)


def restage(system_root: Path, data_root: Path, log: host.Log) -> bool:
    """Copy missing() back into Data's staged_system_apps, each bundle whole or not at all; True when any was."""
    names = missing(system_root, data_root)
    staged = Path(data_root) / STAGED
    for name in names:
        partial = staged / f'.{name}.partial'
        if partial.exists():
            shutil.rmtree(partial)
        partial.parent.mkdir(exist_ok=True)
        _copy_bundle(Path(system_root) / 'private/var' / STAGED / name, partial)
        partial.rename(staged / name)
    if names:
        log(f'system apps: restaged {len(names)} the last migration did not install: {", ".join(names)}')
    return bool(names)
