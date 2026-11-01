#!/usr/bin/env python3
"""Copy the packaged boot entry onto the Switch's SD card and eject the card.

Usage: deploy.py <boot-dir> <sd-mount>

Unmounts the iPad APFS volumes macOS mounted from the card, copies every file of <boot-dir>
to the same path on the card with `cp -X` (no extended attributes, so no AppleDouble files),
checks each copy with `cmp`, deletes any `._` file next to what it wrote, syncs and ejects."""
import os
import plistlib
import subprocess
import sys
from pathlib import Path

DISKUTIL = '/usr/sbin/diskutil'


def plist(*command):
    return plistlib.loads(subprocess.run(command, check=True, capture_output=True).stdout)


def copy_entry(boot, sd):
    """Copy and compare every file; return the paths written and the directories created."""
    written, created = [], []
    for source in sorted(p for p in boot.rglob('*') if p.is_file()):
        target = sd / source.relative_to(boot)
        missing = [d for d in reversed(target.parents) if d.is_relative_to(sd) and not d.exists()]
        target.parent.mkdir(parents=True, exist_ok=True)
        created += missing
        subprocess.run(['/bin/cp', '-X', str(source), str(target)], check=True)
        subprocess.run(['/usr/bin/cmp', str(source), str(target)], check=True)
        written.append(target)
    for path in written + created:
        double = path.with_name('._' + path.name)
        if double.is_file():
            double.unlink()
    return written, created


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    boot, sd = Path(sys.argv[1]), Path(sys.argv[2])
    if not any(boot.rglob('*')):
        raise SystemExit(f'{boot} is empty; build the package target first')
    if not os.path.ismount(sd):
        raise SystemExit(f'{sd} is not mounted')
    if not (sd / 'bootloader').is_dir():
        raise SystemExit(f'{sd} has no bootloader/ directory; is it the Hekate card?')
    info = plist(DISKUTIL, 'info', '-plist', str(sd))
    disk = info['ParentWholeDisk']
    if not (info.get('Ejectable') or info.get('RemovableMedia') or info.get('Removable')):
        raise SystemExit(f'{sd} ({disk}) is not removable media')

    # macOS mounts the iPad volumes of the card's APFS container; unmount them before writing.
    for container in plist(DISKUTIL, 'apfs', 'list', '-plist').get('Containers', []):
        stores = [store['DeviceIdentifier'] for store in container.get('PhysicalStores', [])]
        if any(store.startswith(disk + 's') for store in stores):
            subprocess.run([DISKUTIL, 'unmountDisk', container['ContainerReference']], check=True)

    written, _ = copy_entry(boot, sd)
    subprocess.run(['/bin/sync'], check=True)
    subprocess.run([DISKUTIL, 'eject', '/dev/' + disk], check=True)
    if os.path.ismount(sd):
        raise SystemExit(f'{sd} is still mounted after the eject')
    print(f'{len(written)} files copied to {sd} and verified; {disk} ejected')


if __name__ == '__main__':
    main()
