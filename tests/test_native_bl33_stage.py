"""The kernel window staged in bl33.bin (native/loader/stage.h), proven on the host: the composed file as hekate puts it
at 0xAA000000 satisfies every boot.cmd memory check, U-Boot's bytes are untouched apart from its SD-only boot_targets,
and the loader's own decoder (copy.c) reads the exact window back from the addresses it was built for."""
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/package'))
import bl33 as pkg  # noqa: E402
from boot_script import boot_script  # noqa: E402
from build_native_loader import (BL33_BASE, STAGE_HEADER, STAGE_SOURCE, compress_window,  # noqa: E402
                                         pack_window, stage_id, stage_into_bl33)


def window_and_stage():
    rng = random.Random(1104)
    window = bytearray(1536 * 4096)
    for page in rng.sample(range(1536), 500):
        at = page * 4096
        window[at:at + 4096] = rng.randbytes(64) * 64 if page % 3 else bytes([page & 255 or 1]) * 4096
    runs, _ = pack_window(bytes(window))
    blocks, stream = compress_window(bytes(window), runs)
    staged = stream + bytes(-len(stream) % 4096)
    return bytes(window), blocks, stream, staged


def itests(script):
    """Every `itest.l *ADDR == VALUE` in the script."""
    return [(int(a, 16), int(v, 0)) for a, v in re.findall(r'itest\.l \*(0x[0-9a-fA-F]+) == (0x[0-9a-fA-F]+|\d+)',
                                                             script)]


class Bl33StageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.window, cls.blocks, cls.stream, cls.staged = window_and_stage()
        cls.original = (ROOT / 'bazel/pinned/firmware/bl33.bin').read_bytes()
        cls.bl33 = stage_into_bl33(pkg.bl33_patch(cls.original), cls.staged, len(cls.stream))
        cls.constants = {'STREAM_BYTES': len(cls.stream), 'STAGE_ID': stage_id(cls.staged)}
        cls.script = boot_script((ROOT / 'boot/raw-v24-boot.cmd').read_text(), 0x1d040, cls.bl33, cls.constants, len(cls.staged))

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
        start = self.original.index(pkg.ENV_START) + 1
        end = self.original.index(b'\0\0', start) + 2
        differing = [i for i in range(len(self.original)) if image[i] != self.original[i]]
        self.assertTrue(differing and all(start <= i < end for i in differing))  # only the default environment
        environment = image[start:end].split(b'\0')
        self.assertIn(b'boot_targets=mmc1', environment)
        self.assertNotIn(b'mmc1 mmc0', image)
        self.assertFalse(any(e.startswith(b'bootcmd_mmc0=') for e in environment))
        self.assertIn(b'bootcmd=setexpr.l ios_u_main *0x60005010; run distro_bootcmd', environment)
        self.assertTrue(any(e.startswith(b'mmc_boot=') and b'setexpr.l ios_u_sd *0x60005010;' in e for e in environment))
        self.assertIn(b'distro_bootcmd=for target in ${boot_targets}', image)
        # Every other variable is as shipped, in order.
        original = self.original[start:end].split(b'\0')
        kept = [pkg.ENV_EDITS.get(e, e) for e in original if e not in pkg.ENV_DROPS]
        self.assertEqual([e for e in environment if e], [e for e in kept if e])
        _, bss_end = struct.unpack_from('<QQ', image, 0x18)
        self.assertLessEqual(BL33_BASE + bss_end, STAGE_HEADER)
        self.assertLessEqual(BL33_BASE + len(image), 0xB0000000)
        with self.assertRaises(SystemExit):
            pkg.bl33_patch(image)  # already patched: refuse rather than guess

    def test_loader_decodes_the_window_from_bl33(self):
        table = ",".join(f"{{{a}u,{b}u,{c}u,{d}ull}}" for a, b, c, d in self.blocks)
        harness = f"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "copy.h"
static const uint64_t blocks[][4] = {{{table}}};
int main(void) {{
    FILE *f = fopen("bl33.bin", "rb"), *g = fopen("window.bin", "rb");
    unsigned char *image = malloc({len(self.bl33)}), *window = malloc({len(self.window)}), *out = malloc({len(self.window)});
    if (!f || !g || fread(image, 1, {len(self.bl33)}, f) != {len(self.bl33)} ||
        fread(window, 1, {len(self.window)}, g) != {len(self.window)}) return 2;
    memset(out, 0xa5, {len(self.window)});
    /* The loader's source is 0x{STAGE_SOURCE:x}, i.e. this many bytes into bl33.bin as hekate loads it. */
    const unsigned char *source = image + 0x{STAGE_SOURCE - BL33_BASE:x};
    if (loader_unpack((uint64_t)(uintptr_t)out, {len(self.window)}, (uint64_t)(uintptr_t)source, {len(self.stream)},
                      blocks, {len(self.blocks)}, 0, 0)) return 3;
    if (memcmp(out, window, {len(self.window)})) return 4;
    puts("PASS bl33 window");
    return 0;
}}
"""
        with tempfile.TemporaryDirectory(prefix="ioswitch-bl33-") as directory:
            directory = Path(directory)
            (directory / "layout.h").write_text("/* host */\n")
            source = (ROOT / "native/loader/copy.c").read_text()
            storage = ("uint64_t loader_copy_tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES]\n"
                       "    __attribute__((section(\".copy_tables\"), aligned(4096))) = {{0}};\n")
            (directory / "copy.c").write_text(source.replace(storage, ""))
            (directory / "harness.c").write_text(harness)
            (directory / "bl33.bin").write_bytes(self.bl33)
            (directory / "window.bin").write_bytes(self.window)
            executable = directory / "decode"
            subprocess.run(["clang", "-std=c11", "-DLOADER_COPY_HOST_TEST=1", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-I", str(directory), "-I", str(ROOT / "native/loader"),
                            str(directory / "copy.c"), str(directory / "harness.c"), "-o", str(executable)], check=True)
            self.assertIn("PASS bl33 window", subprocess.check_output([str(executable)], text=True, cwd=directory))


if __name__ == "__main__":
    unittest.main()
