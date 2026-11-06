"""The iOS APFS container, composed in a scratch disk image on the Mac (never on the card).

The IPSW's OS container (System volume 04769D26...) is copied into a raw image, grown to the target partition's size,
given the Data and Preboot volumes, and those get the UUIDs the kernel was built with (diskutil cannot choose UUIDs;
apfs.py rewrites them offline). fsck_apfs checks the result.
"""

import os
import subprocess
import uuid

from tools.install import apfs, gpt, host

COPY_CHUNK = 8 << 20
F_PUNCHHOLE = 99  # <sys/fcntl.h>: deallocate a range of a file (APFS)
START_SECTOR = 2048


def punch(fd, offset, length):
    """Give a copied range of a scratch image back to the disk, so the scratch and its copy never both exist whole."""
    import fcntl
    import struct

    try:
        fcntl.fcntl(fd, F_PUNCHHOLE, struct.pack('<IIqq', 0, 0, offset, length))
    except OSError:
        pass


def ours(spec, found):
    have = {(str(u).upper(), role) for u, role, _ in (found or [])}
    return all((v['uuid'], v['role']) in have for v in spec['volumes'].values())


def attach_raw(image, log, mount=False):
    args = ['attach', '-plist'] + ([] if mount else ['-nomount'])
    out = host.plist('hdiutil', *args, '-imagekey', 'diskimage-class=CRawDiskImage', str(image), log=log)
    disk = host.whole_disk_of(out['system-entities'])
    if not disk:
        raise host.HostError('hdiutil attach gave no disk for {}'.format(image))
    return disk


def fsck(store, what, log):
    result = subprocess.run([host.TOOLS['fsck_apfs'], '-n', '/dev/r' + store], capture_output=True, text=True)
    log.quiet(result.stdout + result.stderr)
    tail = (result.stdout + result.stderr).strip().splitlines()[-1:] or ['?']
    if result.returncode or 'appears to be OK' not in tail[0]:
        raise host.HostError('fsck_apfs -n {} ({}): {}'.format(store, what, tail[0]))
    log('fsck_apfs {} ({}): {}'.format(store, what, tail[0]))


def _attach_os(os_dmg, log):
    out = host.plist(
        'hdiutil', 'attach', '-plist', '-readonly', '-nomount', '-noverify', '-noautofsck', os_dmg, log=log
    )
    apfs_part = [
        e['dev-entry']
        for e in out['system-entities']
        if str(gpt.APFS).upper() in (e.get('content-hint', '').upper(), e.get('unmapped-content-hint', '').upper())
    ]
    if len(apfs_part) != 1:
        for d in (e.get('dev-entry', '') for e in out['system-entities']):
            if d.startswith('/dev/disk') and 's' not in d[len('/dev/disk') :]:
                subprocess.run([host.TOOLS['hdiutil'], 'detach', d], capture_output=True)
        raise host.HostError('{}: expected one APFS partition, got {}'.format(os_dmg, out))
    return apfs_part[0][: apfs_part[0].rindex('s')], apfs_part[0]


def os_container_bytes(os_dmg, log):
    whole, source = _attach_os(os_dmg, log)
    try:
        return int(host.disk_info(source)['TotalSize'])
    finally:
        host.run('hdiutil', 'detach', whole, log=log)


def _copy_os(os_dmg, image, partition_sectors, spec, log):
    disk_sectors = START_SECTOR + partition_sectors + 2048
    whole, source = _attach_os(os_dmg, log)
    try:
        os_bytes = int(host.disk_info(source)['TotalSize'])
        raw_source = '/dev/r' + source[len('/dev/') :]
        fd = os.open(raw_source, os.O_RDONLY)
        try:
            found = apfs.Container(fd).volumes()
        finally:
            os.close(fd)
        if (spec['volumes']['system']['uuid'], 1) not in {(str(u).upper(), r) for u, r, _ in found}:
            raise host.HostError(
                'the OS image has no System volume {} ({})'.format(spec['volumes']['system']['uuid'], found)
            )
        with open(image, 'wb') as out:
            out.truncate(disk_sectors * gpt.SECTOR)
        ifd = os.open(image, os.O_RDWR)
        sfd = os.open(raw_source, os.O_RDONLY)
        try:
            first = (gpt.APFS, uuid.uuid4(), START_SECTOR, START_SECTOR + os_bytes // gpt.SECTOR - 1, 'compose')
            for lba, data in gpt.build(disk_sectors, [first]):
                os.pwrite(ifd, data, lba * gpt.SECTOR)
            copied = 0
            while copied < os_bytes:
                chunk = os.pread(sfd, min(COPY_CHUNK, os_bytes - copied), copied)
                if not chunk:
                    raise host.HostError('short read of the OS image at {}'.format(copied))
                if chunk.strip(b'\0'):
                    os.pwrite(ifd, chunk, START_SECTOR * gpt.SECTOR + copied)
                copied += len(chunk)
            os.fsync(ifd)
        finally:
            os.close(sfd)
            os.close(ifd)
        log('compose: OS container copied ({} bytes)'.format(os_bytes))
        return os_bytes
    finally:
        host.run('hdiutil', 'detach', whole, log=log)


def _grow(image, partition_bytes, spec, log):
    disk = attach_raw(image, log)
    try:
        store = disk + 's1'
        host.run('diskutil', 'apfs', 'resizeContainer', store, '{}B'.format(partition_bytes), log=log)
        container = host.container_on(store)
        if not container:
            raise host.HostError('the scratch container did not attach')
        for key, role in (('data', 'D'), ('preboot', 'B')):
            host.run(
                'diskutil',
                'apfs',
                'addVolume',
                container['ContainerReference'],
                'Case-sensitive APFS',
                spec['volumes'][key]['name'],
                '-role',
                role,
                '-nomount',
                log=log,
            )
        log('compose: grown to {} bytes, Data and Preboot added'.format(partition_bytes))
    finally:
        host.run('hdiutil', 'detach', disk, log=log)


def _set_uuids(image, partition_bytes, spec, log):
    fd = os.open(image, os.O_RDWR)
    try:
        part = gpt.read(fd, os.fstat(fd).st_size).used[0]
        if part.bytes != partition_bytes:
            raise host.HostError(
                'resizeContainer left the scratch partition at {} bytes, not {}'.format(part.bytes, partition_bytes)
            )
        container = apfs.Container(fd, part.first * gpt.SECTOR)
        found = container.volumes()
        mapping = {}
        for key in ('data', 'preboot'):
            want = spec['volumes'][key]
            match = [u for u, role, name in found if role == want['role'] and name == want['name']]
            if len(match) != 1:
                raise host.HostError('scratch container: no single {} volume ({})'.format(key, found))
            mapping[match[0]] = uuid.UUID(want['uuid'])
        patched = container.set_volume_uuids(mapping)
        log(
            'compose: volume UUIDs set offline in {} superblock copies: {}'.format(
                len(patched), ', '.join('{}->{}'.format(o, n) for _, o, n in patched)
            )
        )
        if not ours(spec, container.volumes()):
            raise host.HostError('UUID rewrite did not take: {}'.format(container.volumes()))
        nx_blocks = int.from_bytes(container.superblocks()[0][40:48], 'little')
        if nx_blocks * container.block_size != partition_bytes:
            raise host.HostError(
                'container is {} bytes, partition {}'.format(nx_blocks * container.block_size, partition_bytes)
            )
        return part.first * gpt.SECTOR
    finally:
        os.close(fd)


def compose(image, os_dmg, partition_bytes, spec, log):
    """Write the container into `image` (a new raw disk image whose one partition is exactly `partition_bytes`).
    Returns (the partition's offset in the image, the OS container's size)."""
    if image.exists():
        image.unlink()
    os_bytes = _copy_os(os_dmg, image, partition_bytes // gpt.SECTOR, spec, log)
    _grow(image, partition_bytes, spec, log)
    offset = _set_uuids(image, partition_bytes, spec, log)
    disk = attach_raw(image, log)
    try:
        fsck(disk + 's1', 'scratch', log)
    finally:
        host.run('hdiutil', 'detach', disk, log=log)
    return offset, os_bytes
