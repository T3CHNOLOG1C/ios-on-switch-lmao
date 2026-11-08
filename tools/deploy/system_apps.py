"""System apps the overlay adds under /Applications, registered the way iOS registers an OS update's.

LaunchServices scans the system app directories only during datamigrator's upgrade migration
(00LaunchServicesMigrator.migrator's populateLSDatabase), which runs when the build recorded in com.apple.migration
differs from the running one, or when /private/var/datamigrator/migrate exists: the sentinel datamigrator's own
forceMigrationOnNextReboot writes and removes once the migration has run. The deploy writes that sentinel when the
overlay's /Applications changed, so the next boot registers the new bundles and drops the removed ones.

The kernel runs a migration once: its marker (MARKER) left from an earlier migration makes a sentinel it finds at boot
read as a migration that never ended, which it deletes unrun. So a requested migration also removes the marker.
"""

from pathlib import Path

from tools.install import card as cards, host, renamed

SENTINEL = 'datamigrator/migrate'
MARKER = f'{renamed.DATA_FOLDER}/system-app-migration'
APPLICATIONS = 'overlay/Applications'


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
