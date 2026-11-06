"""U-Boot legacy images (the 64-byte uImage header): the loader (loader.uboot.bin) and the boot script."""

import struct
import zlib

# Where U-Boot's bootm puts the loader and jumps (the loader's link address, boot/loader/link.ld).
LOAD = 0xA0000000


def legacy_image(data: bytes, *, os_type: int, kind: int, name: str, load: int = 0, entry: int = 0) -> bytes:
    """`data` behind a legacy header (architecture 22, AArch64; uncompressed), checked by decoding it again."""
    title = name.encode('ascii')
    if len(title) > 32:
        raise ValueError('legacy image name is too long')
    header = struct.pack(
        '>7I4B32s',
        0x27051956,
        0,
        0,
        len(data),
        load,
        entry,
        zlib.crc32(data),
        os_type,
        22,
        kind,
        0,
        title.ljust(32, b'\0'),
    )
    header = header[:4] + struct.pack('>I', zlib.crc32(header)) + header[8:]
    image = header + data
    decoded = struct.unpack('>7I4B32s', image[:64])
    zeroed = image[:4] + bytes(4) + image[8:64]
    if (
        decoded[0] != 0x27051956
        or decoded[1] != zlib.crc32(zeroed)
        or decoded[3:7] != (len(data), load, entry, zlib.crc32(data))
        or decoded[7:11] != (os_type, 22, kind, 0)
    ):
        raise ValueError('legacy wrapper validation failed')
    return image
