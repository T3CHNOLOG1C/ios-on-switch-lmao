"""A cryptex image with a pretranslated delta applied and sealed again, for Preboot (iOS 27).

The decrypted cryptex is a raw read/write image of one APFS volume (UDRW, no partition map). APFS checks every block
of a cryptex against its seal, so after the delta the volume is sealed again with macOS's own cryptex sealer.
"""

import re
import shutil
from collections.abc import Callable
from pathlib import Path

from tools.install import card as cards, delta, host

SEALER = '/System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_prepare_cryptex'
APFS_VOLUME = '41504653-0000-11AA-AA11-00306543ECAC'
# Room beyond the replacements' blocks: the metadata their new extents take, and the seal
SLACK = 64 << 20


def _attach(image: Path, log: host.Log, writable: bool = True) -> tuple[str, str]:
    mode = '-readwrite' if writable else '-readonly'
    out = host.plist('hdiutil', 'attach', '-plist', mode, '-nomount', '-noverify', '-noautofsck', str(image), log=log)
    entities = out['system-entities']
    volumes = [e['dev-entry'][len('/dev/') :] for e in entities if e.get('content-hint', '').upper() == APFS_VOLUME]
    disks = [e['dev-entry'] for e in entities if re.fullmatch(r'/dev/disk\d+', e.get('dev-entry', ''))]
    if len(volumes) != 1:
        for d in disks:
            host.run('hdiutil', 'detach', d, log=log, check=False)
        raise host.HostError(f'{image}: expected one APFS volume, got {entities}')
    return volumes[0], disks[0]


def _grow(image: Path, by: int, log: host.Log) -> None:
    """Make `image` `by` bytes (whole MiB) larger and its APFS container fill it: Apple ships cryptexes nearly full."""
    with open(image, 'r+b') as f:
        f.truncate(image.stat().st_size + -(-by // (1 << 20)) * (1 << 20))
    _, whole = _attach(image, log)
    try:
        host.run('diskutil', 'apfs', 'resizeContainer', whole, '0', log=log)
    finally:
        host.run('hdiutil', 'detach', whole, log=log, check=False)


def rebuild(source: str, translate: Callable[[Path], list[dict]], work_dir: Path, log: host.Log, what: str) -> Path:
    """Clone `source` into `work_dir` (APFS clones share blocks until written), apply its delta and seal it again.

    `translate(mount point)` makes the delta from the mounted clone and returns its entries. A volume without room for
    them is grown first.
    """
    image = Path(work_dir) / Path(source).name
    if image.exists():
        image.unlink()
    host.run('/bin/cp', '-c', source, image, log=log)
    image.chmod(0o644)
    entries = None
    while True:
        volume, whole = _attach(image, log)
        try:
            with cards.mounted(volume, log, owners=True) as root:
                if entries is None:
                    entries = translate(root)
                short = delta.added_bytes(root, entries) + SLACK - shutil.disk_usage(root).free
                if short <= 0:
                    delta.apply(root, entries, log, what, consume=True)
                    host.run(SEALER, str(root), log=log)
                    break
        finally:
            host.run('hdiutil', 'detach', whole, log=log, check=False)
        log(f'{what}: growing the volume by {(short + SLACK) >> 20} MiB for its translated files')
        _grow(image, short + SLACK, log)
    log(f'{what}: sealed again as {image}')
    return image
