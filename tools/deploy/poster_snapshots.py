"""PosterBoard's gallery snapshots on Data, removed per poster extension so the next gallery regenerates them.

PosterBoard renders each gallery tile once (a PBFPosterSnapshotter run of the poster's extension) and keeps the result,
an ATX image, beside the descriptor in its data store:

    <Data>/mobile/Containers/Data/Application/<PosterBoard>/Library/Application Support/PRBPosterExtensionDataStore/<n>/
        Extensions/<bundle>/{descriptors,staticdescriptors}/<UUID>/versions/<v>/SNAPSHOT_GALLERY-....atx

It never re-renders a tile whose file exists, so a tile rendered while a driver was broken stays as it was (r13-posters:
KaleidoscopePoster's tiles from before NBCommandBuffer presented drawables, PridePoster's from before any Metal device).
A missing tile is rendered again when the gallery shows it (11-05 10:17:48: GradientPoster's). Only the SNAPSHOT_GALLERY
files of the named extensions' descriptors go; descriptors, configurations (the user's own posters) and their snapshots
stay.
"""

import plistlib
from collections.abc import Callable
from pathlib import Path

POSTERBOARD = 'com.apple.PosterBoard'
APPLICATIONS = 'mobile/Containers/Data/Application'
METADATA = '.com.apple.mobile_container_manager.metadata.plist'
STORE = 'Library/Application Support/PRBPosterExtensionDataStore'
DESCRIPTOR_KINDS = ('descriptors', 'staticdescriptors')


def posterboard_container(data_root: Path) -> Path | None:
    """PosterBoard's data container on a mounted Data volume, or None."""
    applications = Path(data_root) / APPLICATIONS
    if not applications.is_dir():
        return None
    for metadata in sorted(applications.glob('*/' + METADATA)):
        try:
            with open(metadata, 'rb') as handle:
                if plistlib.load(handle).get('MCMMetadataIdentifier') == POSTERBOARD:
                    return metadata.parent
        except (OSError, plistlib.InvalidFileException, ValueError):
            continue
    return None


def gallery_snapshots(data_root: Path, bundles: list[str]) -> list[Path]:
    """Return the SNAPSHOT_GALLERY files of `bundles`' descriptors in every data store version, sorted."""
    container = posterboard_container(data_root)
    if container is None:
        return []
    found = []
    for store in sorted((container / STORE).glob('*/Extensions')):
        for bundle in bundles:
            for kind in DESCRIPTOR_KINDS:
                found += [p for p in (store / bundle / kind).glob('*/versions/*/SNAPSHOT_GALLERY*') if p.is_file()]
    return sorted(found)


def reset(data_root: Path, bundles: list[str], log: Callable[..., None], dry_run: bool = False) -> int:
    """Remove `bundles`' gallery snapshots; return how many (would) go."""
    snapshots = gallery_snapshots(data_root, bundles)
    if posterboard_container(data_root) is None:
        log('gallery snapshots: no PosterBoard container on Data')
        return 0
    for path in snapshots:
        if not dry_run:
            path.unlink()
    counts = {}
    for path in snapshots:
        bundle = path.parents[4].name  # <bundle>/<kind>/<UUID>/versions/<v>/file
        counts[bundle] = counts.get(bundle, 0) + 1
    for bundle in bundles:
        log(
            'gallery snapshots: {} {} of {}'.format(
                'would remove' if dry_run else 'removed', counts.get(bundle, 0), bundle
            )
        )
    return len(snapshots)
