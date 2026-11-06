"""A composed partition image into install/: part files under FAT32's 4 GiB limit and manifest.bin."""

import hashlib
import os
from pathlib import Path

from tools.install import compose, extents, manifest

CHUNK = 8 << 20
MANIFEST = 'manifest.bin'


def part_name(index):
    return 'part-{:02d}.bin'.format(index)


def pack(image, offset, partition_bytes, install_dir, fat_prefix, label, log, punch=True):
    """Plan the extents of the partition at `offset` in `image`, stream the copy extents into part files (punching
    the image behind, so the two never both exist whole), and write the manifest. Returns the Manifest."""
    install_dir = Path(install_dir)
    fd = os.open(image, os.O_RDWR)
    try:
        block, count, container, areas = extents.container(os.pread(fd, 4096, offset))
        if block * count != partition_bytes:
            raise extents.ExtentError(
                'the container is {} bytes, the partition {}'.format(block * count, partition_bytes)
            )
        data = extents.data_ranges(fd, offset, partition_bytes, block)
        plan = extents.plan(data, areas, partition_bytes, block)
        sizes = manifest.split(plan)
        log(
            'pack: {} extents, {} bytes to copy in {} parts, {} bytes written as zeros'.format(
                len(plan), sum(sizes), len(sizes), sum(n for _, s, n in plan if s == manifest.ZERO)
            )
        )
        parts, hashes = _write_parts(fd, offset, plan, sizes, install_dir, fat_prefix, punch)
    finally:
        os.close(fd)
    m = manifest.Manifest(partition_bytes, block, container, label, parts, plan, hashes)
    (install_dir / MANIFEST).write_bytes(manifest.encode(m))
    log('pack: {} written ({} parts)'.format(install_dir / MANIFEST, len(parts)))
    return m


class ChunkHashes:
    """SHA-256 of each HASH_CHUNK of a byte stream fed in any pieces."""

    def __init__(self):
        self.hashes, self.current, self.filled = [], hashlib.sha256(), 0

    def update(self, data):
        view = memoryview(data)
        while view:
            take = min(len(view), manifest.HASH_CHUNK - self.filled)
            self.current.update(view[:take])
            self.filled += take
            view = view[take:]
            if self.filled == manifest.HASH_CHUNK:
                self.hashes.append(self.current.digest())
                self.current, self.filled = hashlib.sha256(), 0

    def finish(self):
        if self.filled:
            self.hashes.append(self.current.digest())
        return self.hashes


def _write_parts(fd, offset, plan, sizes, install_dir, fat_prefix, punch):
    parts, index, out, left = [], -1, None, 0
    hashes = ChunkHashes()

    def close():
        if out is not None:
            out.close()
            parts.append(manifest.Part('{}/{}'.format(fat_prefix, part_name(index)), sizes[index]))

    try:
        for at, source, n in plan:
            if source == manifest.ZERO:
                continue
            done = 0
            while done < n:
                if not left:
                    close()
                    index += 1
                    out, left = open(install_dir / part_name(index), 'wb'), sizes[index]
                take = min(CHUNK, n - done, left)
                chunk = os.pread(fd, take, offset + at + done)
                if len(chunk) != take:
                    raise extents.ExtentError('short read of the image at {:#x}'.format(at + done))
                out.write(chunk)
                hashes.update(chunk)
                if punch:
                    compose.punch(fd, offset + at + done, take)
                done += take
                left -= take
        close()
        out = None
    finally:
        if out is not None:
            out.close()
    return parts, hashes.finish()
