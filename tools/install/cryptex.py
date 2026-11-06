"""A cryptex image with a pretranslated delta applied and sealed again, for Preboot (iOS 27).

The decrypted cryptex is a raw read/write image of one APFS volume (UDRW, no partition map). APFS checks every block
of a cryptex against its seal, so after the delta the volume is sealed again with macOS's own cryptex sealer.
"""

import re
from collections.abc import Callable
from pathlib import Path

from tools.install import card as cards, delta, host

SEALER = '/System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_prepare_cryptex'
APFS_VOLUME = '41504653-0000-11AA-AA11-00306543ECAC'


def _attach(image: Path, log: host.Log) -> tuple[str, str]:
    out = host.plist(
        'hdiutil', 'attach', '-plist', '-readwrite', '-nomount', '-noverify', '-noautofsck', str(image), log=log
    )
    entities = out['system-entities']
    volumes = [e['dev-entry'][len('/dev/') :] for e in entities if e.get('content-hint', '').upper() == APFS_VOLUME]
    disks = [e['dev-entry'] for e in entities if re.fullmatch(r'/dev/disk\d+', e.get('dev-entry', ''))]
    if len(volumes) != 1:
        for d in disks:
            host.run('hdiutil', 'detach', d, log=log, check=False)
        raise host.HostError(f'{image}: expected one APFS volume, got {entities}')
    return volumes[0], disks[0]


def rebuild(source: str, translate: Callable[[Path], list[dict]], work_dir: Path, log: host.Log, what: str) -> Path:
    """Clone `source` into `work_dir` (APFS clones share blocks until written), apply its delta and seal it again.

    `translate(mount point)` makes the delta from the mounted clone and returns its entries.
    """
    image = Path(work_dir) / Path(source).name
    if image.exists():
        image.unlink()
    host.run('/bin/cp', '-c', source, image, log=log)
    image.chmod(0o644)
    volume, whole = _attach(image, log)
    try:
        with cards.mounted(volume, log, owners=True) as root:
            delta.apply(root, translate(root), log, what, consume=True)
            host.run(SEALER, str(root), log=log)
    finally:
        host.run('hdiutil', 'detach', whole, log=log, check=False)
    log(f'{what}: sealed again as {image}')
    return image
