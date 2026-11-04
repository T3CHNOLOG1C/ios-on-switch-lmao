"""Focused descriptor checks only; no privileged instruction/model/device execution."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeLoaderCopyTests(unittest.TestCase):
    def test_exact_mapping_permissions_and_refusals(self):
        with tempfile.TemporaryDirectory(prefix="ioswitch-copy-tables-") as directory:
            directory = Path(directory)
            (directory / "layout.h").write_text("/* Host table builder uses explicit checked bounds. */\n")
            # Darwin cannot parse the ELF-only image section annotation. The
            # host supplies guarded aligned storage; omit only that unused
            # global declaration, preserving both functions byte-for-byte.
            source = (ROOT / "native/loader/copy.c").read_text()
            storage = ("uint64_t loader_copy_tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES]\n"
                       "    __attribute__((section(\".copy_tables\"), aligned(4096))) = {{0}};\n")
            self.assertEqual(source.count(storage), 1)
            (directory / "copy.c").write_text(source.replace(storage, ""))
            executable = directory / "tables"
            subprocess.run([
                "clang", "-std=c11", "-D_POSIX_C_SOURCE=200112L", "-DLOADER_COPY_HOST_TEST=1",
                "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-O1", "-g",
                "-I", str(directory), "-I", str(ROOT / "native/loader"),
                str(directory / "copy.c"), str(ROOT / "tests/native_loader_copy_harness.c"),
                "-o", str(executable)
            ], check=True)
            output = subprocess.check_output([str(executable)], text=True)
            self.assertIn("PASS 32 exact 4GiB map checks and 21 malformed input controls", output)

    def test_unpack_expands_packed_window_exactly(self):
        """loader_unpack (the loader's copy under its cacheable regime) against the builder's pack_window."""
        import random
        import sys
        sys.path.insert(0, str(ROOT))
        from scripts.build_native_loader import pack_window
        rng = random.Random(1104)
        window = bytearray(64 * 4096)
        for page in rng.sample(range(64), 23):
            window[page * 4096 + rng.randrange(4096)] = rng.randrange(1, 256)
        window[0:8] = b"IOSWITCH"
        runs, packed = pack_window(bytes(window))
        self.assertLess(len(packed), len(window))
        table = ",".join(f"{{{start}u,{size}u}}" for start, size in runs)
        harness = f"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "copy.h"
static const uint64_t runs[][2] = {{{table}}};
static unsigned calls;
static void progress(uint64_t done, uint64_t total, uint64_t started) {{ (void)done; (void)total; (void)started; ++calls; }}
int main(void) {{
    FILE *f = fopen("packed.bin", "rb"), *g = fopen("window.bin", "rb");
    static unsigned char packed[{len(packed)}], window[{len(window)}], out[{len(window)}];
    if (!f || !g || fread(packed, 1, sizeof packed, f) != sizeof packed || fread(window, 1, sizeof window, g) != sizeof window) return 2;
    memset(out, 0xa5, sizeof out);
    if (loader_unpack((uint64_t)(uintptr_t)out, (uint64_t)(uintptr_t)packed, sizeof out, runs, {len(runs)}, progress, 0)) return 3;
    if (memcmp(out, window, sizeof out) || calls != {len(runs)} + 1) return 4;
    const uint64_t overlap[][2] = {{{{0, 8192}}, {{4096, 4096}}}}, unaligned[][2] = {{{{512, 4096}}}}, outside[][2] = {{{{0, sizeof out + 4096}}}};
    if (!loader_unpack((uint64_t)(uintptr_t)out, (uint64_t)(uintptr_t)packed, sizeof out, overlap, 2, 0, 0)) return 5;
    if (!loader_unpack((uint64_t)(uintptr_t)out, (uint64_t)(uintptr_t)packed, sizeof out, unaligned, 1, 0, 0)) return 6;
    if (!loader_unpack((uint64_t)(uintptr_t)out, (uint64_t)(uintptr_t)packed, sizeof out, outside, 1, 0, 0)) return 7;
    puts("PASS unpack");
    return 0;
}}
"""
        with tempfile.TemporaryDirectory(prefix="ioswitch-unpack-") as directory:
            directory = Path(directory)
            (directory / "layout.h").write_text("/* host */\n")
            source = (ROOT / "native/loader/copy.c").read_text()
            storage = ("uint64_t loader_copy_tables[LOADER_COPY_TABLE_PAGES][LOADER_COPY_TABLE_ENTRIES]\n"
                       "    __attribute__((section(\".copy_tables\"), aligned(4096))) = {{0}};\n")
            (directory / "copy.c").write_text(source.replace(storage, ""))
            (directory / "harness.c").write_text(harness)
            (directory / "packed.bin").write_bytes(packed)
            (directory / "window.bin").write_bytes(bytes(window))
            executable = directory / "unpack"
            subprocess.run(["clang", "-std=c11", "-DLOADER_COPY_HOST_TEST=1", "-Wall", "-Wextra", "-Werror", "-O1",
                            "-I", str(directory), "-I", str(ROOT / "native/loader"),
                            str(directory / "copy.c"), str(directory / "harness.c"), "-o", str(executable)], check=True)
            output = subprocess.check_output([str(executable)], text=True, cwd=directory)
            self.assertIn("PASS unpack", output)


if __name__ == "__main__":
    unittest.main()
