"""The kernel window staged in bl33.bin (native/loader/stage.h), proven on the host: the composed file as hekate puts it
at 0xAA000000 satisfies every boot.cmd memory check, and U-Boot's bytes are untouched apart from its SD-only
boot_targets. That the loader's own decoder (copy.c) reads the exact window back from the addresses it was built for
is //native/loader/test:bl33_window_test."""

import random
import re
import struct
import unittest

from pathlib import Path

# //tools/package's bl33, boot_script and kernel_window (lz4 from //tools:lz4 on PATH); the pinned bl33.bin and the
# boot.cmd template from runfiles (the test runs in the main repository's runfiles directory).
import bl33
from bl33 import bl33_patch
from boot_script import boot_script
from build_native_loader import (
    BL33_BASE,
    STAGE_HEADER,
    STAGE_SOURCE,
    compress_window,
    pack_window,
    stage_id,
    stage_into_bl33,
)

BL33 = Path('bazel/pinned/firmware/bl33.bin')
TEMPLATE = Path('boot/raw-v24-boot.cmd')


def window_and_stage():
    rng = random.Random(1104)
    window = bytearray(1536 * 4096)
    for page in rng.sample(range(1536), 500):
        at = page * 4096
        window[at : at + 4096] = rng.randbytes(64) * 64 if page % 3 else bytes([page & 255 or 1]) * 4096
    runs, _ = pack_window(bytes(window))
    blocks, stream = compress_window(bytes(window), runs)
    staged = stream + bytes(-len(stream) % 4096)
    return bytes(window), blocks, stream, staged


def itests(script):
    """Every `itest.l *ADDR == VALUE` in the script."""
    return [
        (int(a, 16), int(v, 0)) for a, v in re.findall(r'itest\.l \*(0x[0-9a-fA-F]+) == (0x[0-9a-fA-F]+|\d+)', script)
    ]


class Bl33StageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.window, cls.blocks, cls.stream, cls.staged = window_and_stage()
        cls.original = BL33.read_bytes()
        cls.bl33 = stage_into_bl33(bl33_patch(cls.original), cls.staged, len(cls.stream))
        cls.constants = {'STREAM_BYTES': len(cls.stream), 'STAGE_ID': stage_id(cls.staged)}
        cls.script = boot_script(TEMPLATE.read_text(), 0x1D040, cls.bl33, cls.constants, len(cls.staged))

    def word(self, image, address):
        return struct.unpack_from('<I', image, address - BL33_BASE)[0]

    def test_boot_script_memory_checks_hold_on_the_composed_file(self):
        inside = [(a, v) for a, v in itests(self.script) if BL33_BASE <= a < BL33_BASE + len(self.bl33)]
        # 6 U-Boot fingerprint words, 8 header words, 6 trailer words.
        self.assertEqual(len(inside), 20)
        for address, value in inside:
            self.assertEqual(self.word(self.bl33, address), value, hex(address))
        size = re.search(r'/bl33\.bin; then\n\s*if itest\.l \$\{filesize\} == (0x[0-9a-f]+);', self.script)
        self.assertEqual(int(size.group(1), 16), len(self.bl33))
        self.assertNotIn('kernel-window.bin', self.script)
        # A window that is not whole, or not this build's, fails a check.
        for at in (STAGE_HEADER + 0x18, STAGE_SOURCE + len(self.staged) + 0x14, STAGE_SOURCE + len(self.staged)):
            damaged = bytearray(self.bl33)
            damaged[at - BL33_BASE] ^= 1
            self.assertTrue(any(self.word(damaged, a) != v for a, v in inside), hex(at))
        truncated = self.bl33[:-4096]
        self.assertTrue(any(a + 4 > BL33_BASE + len(truncated) or self.word(truncated, a) != v for a, v in inside))

    def test_u_boot_is_untouched_but_never_scans_the_emmc(self):
        image = self.bl33
        start = self.original.index(bl33.ENV_START) + 1
        end = self.original.index(b'\0\0', start) + 2
        differing = [i for i in range(len(self.original)) if image[i] != self.original[i]]
        self.assertTrue(differing and all(start <= i < end for i in differing))  # only the default environment
        environment = image[start:end].split(b'\0')
        self.assertIn(b'boot_targets=mmc1', environment)
        self.assertNotIn(b'mmc1 mmc0', image)
        self.assertFalse(any(e.startswith(b'bootcmd_mmc0=') for e in environment))
        self.assertIn(b'bootcmd=setexpr.l ios_u_main *0x60005010; run distro_bootcmd', environment)
        self.assertTrue(
            any(e.startswith(b'mmc_boot=') and b'setexpr.l ios_u_sd *0x60005010;' in e for e in environment)
        )
        self.assertIn(b'distro_bootcmd=for target in ${boot_targets}', image)
        # Every other variable is as shipped, in order.
        original = self.original[start:end].split(b'\0')
        kept = [bl33.ENV_EDITS.get(e, e) for e in original if e not in bl33.ENV_DROPS]
        self.assertEqual([e for e in environment if e], [e for e in kept if e])
        _, bss_end = struct.unpack_from('<QQ', image, 0x18)
        self.assertLessEqual(BL33_BASE + bss_end, STAGE_HEADER)
        self.assertLessEqual(BL33_BASE + len(image), 0xB0000000)
        with self.assertRaises(SystemExit):
            bl33_patch(image)  # already patched: refuse rather than guess


if __name__ == '__main__':
    unittest.main()
