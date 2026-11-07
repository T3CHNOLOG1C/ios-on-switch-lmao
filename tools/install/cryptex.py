"""A cryptex image with a pretranslated delta applied and sealed again, for Preboot (iOS 27).

The decrypted cryptex is a raw read/write image of one APFS volume (UDRW, no partition map). APFS checks every block
of a cryptex against its seal, so after the delta the volume is sealed again with macOS's own cryptex sealer.
"""

import os
import re
from collections.abc import Callable
from pathlib import Path

from tools.install import card as cards, delta, host

SEALER = '/System/Library/Filesystems/apfs.fs/Contents/Resources/apfs_prepare_cryptex'
APFS_VOLUME = '41504653-0000-11AA-AA11-00306543ECAC'
# Room beyond the source's size and the replacements' blocks: the metadata their new extents take, and the seal
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


def _blank(image: Path, size: int, name: str, log: host.Log) -> None:
    """Make `image` a raw image of `size` bytes holding one empty case-sensitive APFS volume, as a cryptex is."""
    with open(image, 'wb') as f:
        f.truncate(size)
    out = host.plist(
        'hdiutil',
        'attach',
        '-plist',
        '-imagekey',
        'diskimage-class=CRawDiskImage',
        '-nomount',
        '-noverify',
        '-noautofsck',
        str(image),
        log=log,
    )
    whole = out['system-entities'][0]['dev-entry']
    try:
        host.run('newfs_apfs', '-e', '-v', name, whole, log=log)
    finally:
        host.run('hdiutil', 'detach', whole, log=log, check=False)


def rebuild(source: str, translate: Callable[[Path], list[dict]], work_dir: Path, log: host.Log, what: str) -> Path:
    """Copy `source`'s volume into a new one in `work_dir` with its delta applied, and seal the new one.

    `translate(mount point)` makes the delta from the mounted source and returns its entries. A sealed volume cannot be
    sealed again (apfs_prepare_cryptex fails on Apple's own, untouched), so the copy is a fresh volume with room for the
    translated files, which are stored uncompressed.
    """
    image = Path(work_dir) / Path(source).name
    if image.exists():
        image.unlink()
    src_volume, src_whole = _attach(Path(source), log, writable=False)
    try:
        name = (host.disk_info(src_volume) or {}).get('VolumeName') or what
        with cards.mounted(src_volume, log, read_only=True) as src:
            entries = translate(src)
            size = os.path.getsize(source) + max(delta.added_bytes(src, entries), 0) + SLACK
            _blank(image, -(-size // (1 << 20)) * (1 << 20), name, log)
            volume, whole = _attach(image, log)
            try:
                with cards.mounted(volume, log, owners=True) as root:
                    host.run('ditto', '--preserveHFSCompression', str(src), str(root), log=log)
                    delta.apply(root, entries, log, what, consume=True)
                # The sealer mounts the container itself: it takes the volume's node, attached and not mounted.
                host.run(SEALER, '/dev/' + volume, log=log)
            finally:
                host.run('hdiutil', 'detach', whole, log=log, check=False)
    finally:
        host.run('hdiutil', 'detach', src_whole, log=log, check=False)
    log(f'{what}: sealed again as {image}')
    return image
