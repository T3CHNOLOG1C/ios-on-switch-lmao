"""A composed partition image into install/: part files under FAT32's 4 GiB limit and manifest.bin."""

import hashlib
import os
from pathlib import Path

from tools.install import compose, extents, host, manifest

CHUNK = 8 << 20
MANIFEST = 'manifest.bin'


def part_name(index: int) -> str:
    return f'part-{index:02d}.bin'


def pack(
    image: Path,
    offset: int,
    partition_bytes: int,
    install_dir: Path,
    fat_prefix: str,
    label: str,
    log: host.Log,
    punch: bool = True,
) -> manifest.Manifest:
    """Pack the partition at `offset` in `image` into part files and a manifest; return the Manifest.

    Plans its extents, streams the copy extents into part files (punching the image behind, so the two never both
    exist whole), and writes the manifest.
    """
    install_dir = Path(install_dir)
    fd = os.open(image, os.O_RDWR)
    try:
        block, count, container, areas = extents.container(os.pread(fd, 4096, offset))
        if block * count != partition_bytes:
            raise extents.ExtentError(f'the container is {block * count} bytes, the partition {partition_bytes}')
        data = extents.data_ranges(fd, offset, partition_bytes, block)
        plan = extents.plan(data, areas, partition_bytes, block)
        sizes = manifest.split(plan)
        log(
            f'pack: {len(plan)} extents, {sum(sizes)} bytes to copy in {len(sizes)} parts, '
            f'{sum(n for _, s, n in plan if s == manifest.ZERO)} bytes written as zeros'
        )
        parts, hashes = _write_parts(fd, offset, plan, sizes, install_dir, fat_prefix, punch)
    finally:
        os.close(fd)
    m = manifest.Manifest(partition_bytes, block, container, label, parts, plan, hashes)
    (install_dir / MANIFEST).write_bytes(manifest.encode(m))
    log(f'pack: {install_dir / MANIFEST} written ({len(parts)} parts)')
    return m


class ChunkHashes:
    """SHA-256 of each HASH_CHUNK of a byte stream fed in any pieces."""

    def __init__(self) -> None:
        self.hashes, self.current, self.filled = [], hashlib.sha256(), 0

    def update(self, data: bytes) -> None:
        view = memoryview(data)
        while view:
            take = min(len(view), manifest.HASH_CHUNK - self.filled)
            self.current.update(view[:take])
            self.filled += take
            view = view[take:]
            if self.filled == manifest.HASH_CHUNK:
                self.hashes.append(self.current.digest())
                self.current, self.filled = hashlib.sha256(), 0

    def finish(self) -> list[bytes]:
        if self.filled:
            self.hashes.append(self.current.digest())
        return self.hashes


def _write_parts(
    fd: int,
    offset: int,
    plan: list[tuple[int, int, int]],
    sizes: list[int],
    install_dir: Path,
    fat_prefix: str,
    punch: bool,
) -> tuple[list[manifest.Part], list[bytes]]:
    parts, index, out, left = [], -1, None, 0
    hashes = ChunkHashes()

    def close() -> None:
        if out is not None:
            out.close()
            parts.append(manifest.Part(f'{fat_prefix}/{part_name(index)}', sizes[index]))

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
                    raise extents.ExtentError(f'short read of the image at {at + done:#x}')
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
