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


if __name__ == "__main__":
    unittest.main()
