import contextlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "hekate_entry.py"
SPEC = importlib.util.spec_from_file_location("hekate_entry", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
hekate_entry = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(hekate_entry)


WORKING = """[config]
autoboot=1
bootwait=3

[My Linux]
l4t=1
boot_prefixes=/switchroot/custom-linux/
id=MYLINUX
rootdev=mmcblk0p4
bootargs_extra=quiet console=tty0 custom=value
;usb3_enable=1
uart_port=0

[Stock]
stock=1
"""


class HekateEntryTests(unittest.TestCase):
    def test_preserves_installed_options_and_excludes_other_sections(self):
        entry = hekate_entry.render_entry(WORKING, "My Linux")
        self.assertEqual(entry, """[IOSwitch (Linux)]
l4t=1
boot_prefixes=/switchroot/custom-linux/
id=MYLINUX
rootdev=mmcblk0p4
bootargs_extra=quiet console=tty0 custom=value
;usb3_enable=1
uart_port=0
""")

    def test_preserves_option_text_duplicates_and_equals_in_values(self):
        source = WORKING.replace("uart_port=0", "custom=a=b\ncustom=c\nuart_port=0  ")
        entry = hekate_entry.render_entry(source, "My Linux")
        self.assertIn("custom=a=b\ncustom=c\nuart_port=0  \n", entry)

    def test_removes_only_blank_and_hash_comment_lines(self):
        source = WORKING.replace("id=MYLINUX", "\n# explanatory comment\n;id=OLD\nid=MYLINUX")
        entry = hekate_entry.render_entry(source, "My Linux")
        self.assertNotIn("\n\n", entry)
        self.assertNotIn("# explanatory", entry)
        self.assertIn(";id=OLD\nid=MYLINUX", entry)

    def test_caption_ends_entry(self):
        source = WORKING.replace("\n[Stock]", "\n{Other entries}\ncaption_option=ignored\n[Stock]")
        entry = hekate_entry.render_entry(source, "My Linux")
        self.assertNotIn("caption_option", entry)

    def test_accepts_rootdev_without_label(self):
        entry = hekate_entry.render_entry(WORKING.replace("id=MYLINUX\n", ""), "My Linux")
        self.assertIn("rootdev=mmcblk0p4", entry)

    def test_rejects_unsafe_or_incomplete_entries(self):
        cases = {
            "payload": WORKING.replace("l4t=1", "l4t=1\npayload=bootloader/payloads/tool.bin"),
            "disabled L4T": WORKING.replace("l4t=1", ";l4t=1"),
            "non L4T": WORKING.replace("l4t=1", "l4t=0"),
            "duplicate L4T": WORKING.replace("l4t=1", "l4t=1\nl4t=1"),
            "missing boot files": WORKING.replace("boot_prefixes=/switchroot/custom-linux/", "boot_prefixes="),
            "missing root": WORKING.replace("id=MYLINUX\nrootdev=mmcblk0p4\n", ""),
            "duplicate section": WORKING + "\n[My Linux]\nl4t=1\n",
            "malformed header": WORKING + "\n[Malformed\n",
        }
        for reason, source in cases.items():
            with self.subTest(reason=reason), self.assertRaises(ValueError):
                hekate_entry.render_entry(source, "My Linux")

    def test_rejects_config_missing_sections_and_invalid_renames(self):
        for section, name in [("config", "IOSwitch"), ("Missing", "IOSwitch"), ("My Linux", "config"), ("My Linux", "bad\n[entry]")]:
            with self.subTest(section=section, name=name), self.assertRaises(ValueError):
                hekate_entry.render_entry(WORKING, section, name)

    def test_cli_exclusive_creation_preserves_source_and_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "existing.ini"
            output = Path(directory) / "new.ini"
            source.write_bytes(WORKING.replace("\n", "\r\n").encode())
            original = source.read_bytes()
            args = ["--source", str(source), "--section", "My Linux", "--output", str(output)]
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(hekate_entry.main(args), 0)
                created = output.read_bytes()
                self.assertEqual(hekate_entry.main(args), 2)
                self.assertEqual(output.read_bytes(), created)
                self.assertEqual(source.read_bytes(), original)
                self.assertEqual(hekate_entry.main(args[:-1] + [str(source)]), 2)
                self.assertEqual(source.read_bytes(), original)

    def test_validation_failure_creates_no_file(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "existing.ini"
            output = Path(directory) / "new.ini"
            source.write_text(WORKING, encoding="utf-8")
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(hekate_entry.main(["--source", str(source), "--section", "Stock", "--output", str(output)]), 2)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
