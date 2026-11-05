"""The LZ4 window vectors copy_unpack_test and bl33_window_test decode, made at build time by the builder itself.

The windows are generated as the old Python tests generated them (random.Random(1104)), then compressed by
tools/package/build_native_loader.py's own pack_window and compress_window (lz4 -12, //tools:lz4) and, for the bl33
case, composed into the pinned bl33.bin by bl33.py's bl33_patch and stage_into_bl33. Written as raw files:
<name>_window.bin, <name>_stream.bin and <name>_blocks.bin (the block table, little-endian u64 x 4 per block),
unpack_meta.bin (little-endian u64: pack_window's run count), and bl33_image.bin with bl33_meta.bin (little-endian
u64: the stream's offset in bl33_image.bin, its length). The tests check what the vectors must be.

    make_window_vectors --bl33 bazel/pinned/firmware/bl33.bin --out <directory>
"""

import argparse
import random
import struct
from pathlib import Path

from bl33 import bl33_patch
from build_native_loader import BL33_BASE, STAGE_SOURCE, compress_window, pack_window, stage_into_bl33


def unpack_window() -> bytes:
    """8 MiB: blocks of 4 MiB and a short tail, gaps, short periods and incompressible pages."""
    rng = random.Random(1104)
    pages = 2048
    window = bytearray(pages * 4096)
    for page in rng.sample(range(pages), 700):
        kind, at = rng.randrange(5), page * 4096
        if kind == 0:
            window[at : at + 4096] = rng.randbytes(4096)  # stored-like noise
        elif kind == 1:
            window[at : at + 4096] = bytes([rng.randrange(1, 256)]) * 4096  # period 1
        elif kind == 2:
            pattern = rng.randbytes(rng.randrange(2, 16))
            window[at : at + 4096] = (pattern * 4096)[:4096]  # periods 2..15
        elif kind == 3:
            words = [b'IOSWITCH', b'__TEXT', b'__DATA', b'kernel', b'\0\0\0\0']
            window[at : at + 4096] = b''.join(rng.choice(words) for _ in range(700))[:4096].ljust(4096, b'\0')
        else:
            window[at + rng.randrange(4096)] = rng.randrange(1, 256)
    window[0:8] = b'IOSWITCH'
    return bytes(window)


def bl33_window() -> bytes:
    """6 MiB of repeated 64-byte patterns and constant pages, a third of the pages left zero."""
    rng = random.Random(1104)
    window = bytearray(1536 * 4096)
    for page in rng.sample(range(1536), 500):
        at = page * 4096
        window[at : at + 4096] = rng.randbytes(64) * 64 if page % 3 else bytes([page & 255 or 1]) * 4096
    return bytes(window)


def compress(window: bytes, lz4: str) -> tuple[list, list, bytes]:
    """Compress the window as the builder does: its nonzero runs, the LZ4 block table, the blocks back to back."""
    runs, _ = pack_window(window)
    blocks, stream = compress_window(window, runs, lz4)
    return runs, blocks, stream


def table(blocks: list) -> bytes:
    """Pack the block table as the tests read it: little-endian u64 x 4 per block."""
    return b''.join(struct.pack('<4Q', *block) for block in blocks)


def main() -> None:
    """Write every vector file into --out."""
    parser = argparse.ArgumentParser(description=__doc__.split('\n', 1)[0])
    parser.add_argument('--bl33', type=Path, required=True)
    parser.add_argument('--lz4', required=True, help='the lz4 CLI (//tools:lz4)')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out = args.out

    window = unpack_window()
    runs, blocks, stream = compress(window, args.lz4)
    (out / 'unpack_meta.bin').write_bytes(struct.pack('<Q', len(runs)))
    (out / 'unpack_window.bin').write_bytes(window)
    (out / 'unpack_stream.bin').write_bytes(stream)
    (out / 'unpack_blocks.bin').write_bytes(table(blocks))

    window = bl33_window()
    _, blocks, stream = compress(window, args.lz4)
    staged = stream + bytes(-len(stream) % 4096)
    image = stage_into_bl33(bl33_patch(args.bl33.read_bytes()), staged, len(stream))
    (out / 'bl33_window.bin').write_bytes(window)
    (out / 'bl33_blocks.bin').write_bytes(table(blocks))
    (out / 'bl33_image.bin').write_bytes(image)
    (out / 'bl33_meta.bin').write_bytes(struct.pack('<2Q', STAGE_SOURCE - BL33_BASE, len(stream)))


if __name__ == '__main__':
    main()
