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
            self.assertIn("PASS 32 exact 4GiB map checks and 26 malformed input controls", output)

    def test_unpack_decodes_compressed_window_exactly(self):
        """loader_unpack (the loader's LZ4 decode under its cacheable regime) against the builder's compress_window, under
        ASan/UBSan: the exact window, then damaged streams and tables that must be refused without stray accesses."""
        import random
        import sys
        sys.path.insert(0, str(ROOT))
        from scripts.build_native_loader import compress_window, pack_window
        rng = random.Random(1104)
        pages = 2048  # 8 MiB: blocks of 4 MiB and a short tail, gaps, short periods and incompressible pages
        window = bytearray(pages * 4096)
        for page in rng.sample(range(pages), 700):
            kind, at = rng.randrange(5), page * 4096
            if kind == 0:
                window[at:at + 4096] = rng.randbytes(4096)              # stored-like noise
            elif kind == 1:
                window[at:at + 4096] = bytes([rng.randrange(1, 256)]) * 4096  # period 1
            elif kind == 2:
                pattern = rng.randbytes(rng.randrange(2, 16))
                window[at:at + 4096] = (pattern * 4096)[:4096]            # periods 2..15
            elif kind == 3:
                words = [b"IOSWITCH", b"__TEXT", b"__DATA", b"kernel", b"\0\0\0\0"]
                window[at:at + 4096] = b"".join(rng.choice(words) for _ in range(700))[:4096].ljust(4096, b"\0")
            else:
                window[at + rng.randrange(4096)] = rng.randrange(1, 256)
        window[0:8] = b"IOSWITCH"
        runs, _ = pack_window(bytes(window))
        blocks, stream = compress_window(bytes(window), runs)
        self.assertLess(len(stream), len(window))
        self.assertGreater(len(blocks), len(runs))
        table = ",".join(f"{{{a}u,{b}u,{c}u,{d}ull}}" for a, b, c, d in blocks)
        harness = f"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "copy.h"
static uint64_t blocks[][4] = {{{table}}};
enum {{ COUNT = sizeof blocks / sizeof blocks[0] }};
static unsigned calls;
static void progress(uint64_t done, uint64_t total, uint64_t started) {{ (void)done; (void)total; (void)started; ++calls; }}
static unsigned char *stream, *window, *out;
static int run(const unsigned char *source) {{
    memset(out, 0xa5, {len(window)});
    return loader_unpack((uint64_t)(uintptr_t)out, {len(window)}, (uint64_t)(uintptr_t)source, {len(stream)},
                         (const uint64_t (*)[4])blocks, COUNT, 0, 0);
}}
int main(void) {{
    FILE *f = fopen("stream.bin", "rb"), *g = fopen("window.bin", "rb");
    stream = malloc({len(stream)}); window = malloc({len(window)}); out = malloc({len(window)});
    unsigned char *copy = malloc({len(stream)});
    if (!f || !g || fread(stream, 1, {len(stream)}, f) != {len(stream)} || fread(window, 1, {len(window)}, g) != {len(window)}) return 2;
    memset(out, 0xa5, {len(window)});
    if (loader_unpack((uint64_t)(uintptr_t)out, {len(window)}, (uint64_t)(uintptr_t)stream, {len(stream)},
                      (const uint64_t (*)[4])blocks, COUNT, progress, 0)) return 3;
    if (memcmp(out, window, {len(window)}) || calls != COUNT + 1) return 4;
    /* Damaged streams: refused or (a flipped literal byte) decoded wrongly, but never outside the buffers (ASan). */
    unsigned refused = 0;
    srand(1104);
    for (unsigned trial = 0; trial < 400; ++trial) {{
        memcpy(copy, stream, {len(stream)});
        const unsigned at = (unsigned)rand() % {len(stream)};
        copy[at] ^= (unsigned char)(1u << (rand() % 8));
        if (trial % 4 == 0) memset(copy + at, 0xff, {len(stream)} - at > 64 ? 64 : {len(stream)} - at);
        refused += run(copy) != 0;
    }}
    if (!refused) return 5;
    /* Tables: overlap, misaligned, outside the window, a gap in the stream, a short stream, stored with a wrong size. */
    uint64_t saved[COUNT][4];
    memcpy(saved, blocks, sizeof saved);
    for (unsigned kind = 0; kind < 6; ++kind) {{
        memcpy(blocks, saved, sizeof saved);
        switch (kind) {{
            case 0: if (COUNT > 1) blocks[1][0] = blocks[0][0]; break;
            case 1: blocks[0][0] += 512; break;
            case 2: blocks[COUNT - 1][1] += {len(window)}; break;
            case 3: blocks[COUNT - 1][2] += 1; break;
            case 4: blocks[COUNT - 1][3] -= 1; break;
            case 5: blocks[0][3] |= LOADER_BLOCK_STORED; break;
        }}
        if (!run(stream)) return 10 + (int)kind;
    }}
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
            (directory / "stream.bin").write_bytes(stream)
            (directory / "window.bin").write_bytes(bytes(window))
            executable = directory / "unpack"
            subprocess.run(["clang", "-std=c11", "-DLOADER_COPY_HOST_TEST=1", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-I", str(directory), "-I", str(ROOT / "native/loader"),
                            str(directory / "copy.c"), str(directory / "harness.c"), "-o", str(executable)], check=True)
            output = subprocess.check_output([str(executable)], text=True, cwd=directory)
            self.assertIn("PASS unpack", output)


if __name__ == "__main__":
    unittest.main()
