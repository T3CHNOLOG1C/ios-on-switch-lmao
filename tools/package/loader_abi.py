"""What the native loader (native/loader) and the packaging agree on: addresses, the RAM ledger's exclusions, the ADT
properties the loader fills, and the staged kernel window's framing (native/loader/stage.h)."""

# The prelinked AMFI closure starts at VA ...0427c000; its 2 MiB offset is kept at PA b207c000.
PHYSICAL_BASE = 0xB207C000
WINDOW_SIZE = 768 << 20

# Kept out of XNU's RAM by the loader (the full-RAM profile's ledger).
NATIVE_EXCLUSIONS = [
    ('original-fdt', 0x80000000, 0x20000),
    ('working-fdt', 0x8D000000, 0x40000),
    ('boot-script', 0x8FE00000, 0x20000),
    ('adapter-stack-vectors', 0xA0000000, 0x20000),
    ('wrapped-adapter', 0xA0100000, 0x20040),
    ('staged-kernel-capacity', 0xA2000000, 0x6000000),
    # hekate loads all of bl33.bin here: U-Boot in the first MiB, then the staged kernel window (native/loader/stage.h).
    ('bl33-and-kernel-window', 0xAA000000, 0x6000000),
    ('ramoops', 0xB0000000, 0x200000),
    ('retained-firmware-display-carveouts', 0xF0000000, 0x10000000),
]

NATIVE_FULL_RAM_EXCLUSIONS = [r for r in NATIVE_EXCLUSIONS if r[0] != 'retained-firmware-display-carveouts'] + [
    ('framebuffer', 0xF5A00000, 0x400000),
    ('secure-low-tail', 0xFFD00000, 0x300000),
]

# boot_args Video: the native rotated text profile (base, display, row bytes, width, height, depth).
VIDEO = [0xF5A00000, 0, 2880, 720, 1280, 0x320]

# The cacheable-copy loader ships the window packed: only its nonzero extents (4 KiB granular; zero gaps shorter than
# 64 KiB stay in an extent), back to back. Over half the window is zeros (the VA holes between prelinked segments); the
# loader writes them itself during its cached copy. Each extent is also LZ4-compressed (lz4 -12, independent blocks of
# 4 MiB, ~2.4:1 on this kernel); the loader decodes it under the same cached regime (native/loader/copy.c lz4_block).
PACK_PAGE = 0x1000
PACK_MIN_GAP = 0x10000
LZ4_BLOCK = 4 << 20
LZ4_MAGIC = b'\x04\x22\x4d\x18'

# /chosen/ioswitch-boot-stamps: native/loader/stamps.h IOSW_STAMP_COUNT 64-bit words, zero until the loader fills it.
STAMPS_PROPERTY = 'ioswitch-boot-stamps'
STAMPS_BYTES = 26 * 8
STAMPS_SLOT = 0xA001EF00
# /chosen/random-seed (the pinned 64 bytes) and /chosen/ioswitch-seed-source (8 zero bytes): the loader mixes SE2's
# DRBG output into the first and reports in the second (native/loader/seed.h).
SEED_PROPERTY = 'random-seed'
SEED_BYTES = 64
SEED_SOURCE_PROPERTY = 'ioswitch-seed-source'
SEED_SOURCE_BYTES = 8

# The fixed same-boot SD reference page (native/storage/sd_reference.h): the loader's code, data and stacks end below.
SD_REFERENCE = 0xA001F000
SD_REFERENCE_BYTES = 0x1000

# The window rides in bl33.bin after U-Boot (native/loader/stage.h): hekate reads it with U-Boot at SDR104.
BL33_BASE = 0xAA000000
STAGE_HEADER = 0xAA100000
STAGE_SOURCE = 0xAA101000
STAGE_END = 0xB0000000
STAGE_WORDS = {'magic': 0x57534F49, 'head': 0x4E49574B, 'tail': 0x444E454B, 'version': 1}
