"""Small .SII images for the tests: a body of a few chunks behind the real head, split at a test-sized FAT limit."""

import hashlib
import uuid
from pathlib import Path

from tools.install import folder, manifest, pack

LIMIT = 80 << 20


def make(root: Path, name: str, body: bytes, boot: dict[str, bytes], limit: int = LIMIT) -> manifest.Manifest:
    """NAME.SII.* under `root`/switchroot/ios holding `body` (a block multiple) and the boot files `boot`."""
    directory = Path(root) / folder.SII_DIR
    directory.mkdir(parents=True, exist_ok=True)
    sources = []
    for file_name, data in boot.items():
        source = Path(root) / f'.boot-{name}-{file_name}'
        source.write_bytes(data)
        sources.append((file_name, source))
    sizes = manifest.split(manifest.BODY_OFFSET + len(body), limit)
    writer = pack.Writer(directory, name, sizes)
    writer.write(body)
    writer.close()
    chunk = manifest.HASH_CHUNK
    hashes = [hashlib.sha256(body[i : i + chunk]).digest() for i in range(0, len(body), chunk)]
    parts = [manifest.Part(f'{folder.SII_DIR}/{pack.part_name(name, i)}', n) for i, n in enumerate(sizes)]
    m = manifest.Manifest(1 << 40, 16 << 30, 4096, uuid.UUID(int=1), f'{name} test', parts, [(0, 0, len(body))], hashes)
    m.boot = pack.boot_records(sources)
    pack.write_head(directory, m, sources)
    for _, source in sources:
        source.unlink()
    return m
