"""Boot-entry and overlay sync, and the log pull, on plain directories (no card)."""
import os
import plistlib
import tempfile
import unittest
from pathlib import Path

from tools.deploy import joycon_pairing
from tools.deploy import pull_logs
from tools.install import card as cards


class Log:
    def __init__(self):
        self.lines = []

    def __call__(self, *parts):
        self.lines.append(" ".join(map(str, parts)))

    quiet = __call__


def package(root, generation=0):
    root = Path(root)
    for name in ("loader.uboot.bin", "kernel-window.bin", "boot.scr"):
        path = root / "switchroot/ioswitch" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(name.encode() * (10 + (generation if name == "kernel-window.bin" else 0)))
    (root / "bootloader/ini").mkdir(parents=True, exist_ok=True)
    (root / "bootloader/ini/ioswitch.ini").write_text("[IOSwitch]\nl4t=1\n")
    overlay = root / "overlay/usr/sbin/BlueTool"
    overlay.parent.mkdir(parents=True, exist_ok=True)
    overlay.write_bytes(b"tool %d" % generation)
    overlay.chmod(0o555)


class SyncTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        package(self.root / "pkg0")
        package(self.root / "pkg1", generation=1)
        self.fat = self.root / "fat"
        for relative in ("bootloader/hekate_ipl.ini", "switchroot/ubuntu/boot.scr", "switchroot/ioswitch/old.bin",
                         "switchroot/ioswitch/._old.bin", "payload.bin"):
            (self.fat / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.fat / relative).write_bytes(b"x")

    def tearDown(self):
        self.tmp.cleanup()

    def snapshot(self):
        return {p.relative_to(self.fat).as_posix(): p.read_bytes() for p in self.fat.rglob("*") if p.is_file()}

    def test_boot_entry_only_touches_our_files_and_is_idempotent(self):
        log = Log()
        changed, removed = cards.sync_boot_entry(self.fat, self.root / "pkg0", log)
        self.assertEqual(len(changed), 4)
        self.assertEqual(removed, ["switchroot/ioswitch/old.bin"])
        files = self.snapshot()
        for untouched in ("bootloader/hekate_ipl.ini", "switchroot/ubuntu/boot.scr", "payload.bin"):
            self.assertEqual(files[untouched], b"x")
        self.assertNotIn("switchroot/ioswitch/._old.bin", files)
        self.assertEqual(cards.sync_boot_entry(self.fat, self.root / "pkg0", log), ([], []))
        changed, _ = cards.sync_boot_entry(self.fat, self.root / "pkg1", log)
        self.assertEqual(changed, ["switchroot/ioswitch/kernel-window.bin"])

    def test_overlay_replaced_only_when_different(self):
        data = self.root / "data"
        data.mkdir()
        log = Log()
        self.assertTrue(cards.sync_overlay(data, self.root / "pkg0", log))
        self.assertTrue(os.access(data / "ioswitch/overlay/usr/sbin/BlueTool", os.X_OK))
        self.assertFalse(cards.sync_overlay(data, self.root / "pkg0", log))
        self.assertTrue(cards.sync_overlay(data, self.root / "pkg1", log))

    def test_pull_logs_copies_what_exists(self):
        data = self.root / "data"
        (data / "db/diagnostics/Persist").mkdir(parents=True)
        (data / "db/diagnostics/Persist/0000.tracev3").write_bytes(b"trace")
        (data / "db/uuidtext/AB").mkdir(parents=True)
        (data / "log/com.apple.xpc.launchd").mkdir(parents=True)
        (data / "log/com.apple.xpc.launchd/launchd.log").write_text("launchd\n")
        (data / "mobile").mkdir()
        out = pull_logs.pull(data, self.root / "logs", Log(), harvest=False)
        self.assertEqual((out / "diagnostics/Persist/0000.tracev3").read_bytes(), b"trace")
        self.assertEqual((out / "launchd.log").read_text(), "launchd\n")
        self.assertTrue((out / "migration-state.txt").is_file())


# hekate nyx _joycon_info_dump_action's format (joycon_00 left, joycon_01 right; type 0 = none or not Horizon's).
DUMP = """[joycon_00]
type=1
mac=AA:BB:CC:00:11:22
host=98:B6:E9:12:34:56
ltk=00112233445566778899AABBCCDDEEFF

[joycon_01]
type=2
mac=AA:BB:CC:00:11:33
host=98:B6:E9:12:34:56
ltk=FFEEDDCCBBAA99887766554433221100

"""


class JoyConPairingTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.fat, self.data = self.root / "fat", self.root / "data"
        (self.fat / "switchroot").mkdir(parents=True)
        self.data.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def test_dump_is_staged_read_only_and_removed_with_it(self):
        ini = self.fat / joycon_pairing.INI
        ini.write_text(DUMP)
        before = ini.stat().st_mtime_ns
        log = Log()
        pairing = joycon_pairing.read(self.fat, log)
        self.assertEqual(pairing["host"], "98:b6:e9:12:34:56")
        self.assertEqual([c["Name"] for c in pairing["controllers"]], ["Joy-Con (L)", "Joy-Con (R)"])
        self.assertEqual(pairing["controllers"][0]["LinkKey"], bytes.fromhex("00112233445566778899AABBCCDDEEFF"))
        self.assertEqual(pairing["controllers"][1]["ProductID"], 0x2007)
        self.assertEqual(ini.stat().st_mtime_ns, before)
        self.assertEqual(sorted(p.name for p in self.fat.rglob("*")), ["joycon_mac.ini", "switchroot"])
        self.assertTrue(joycon_pairing.stage(self.data, pairing, log))
        staged = self.data / joycon_pairing.STAGED
        self.assertEqual((staged / "host-address").read_bytes(), bytes.fromhex("98B6E9123456"))
        self.assertEqual(oct((staged / "host-address").stat().st_mode & 0o777), "0o644")
        plist = plistlib.loads((staged / "joycon-pairing.plist").read_bytes())
        self.assertEqual(plist["Controllers"][1]["Address"], "aa:bb:cc:00:11:33")
        self.assertFalse(joycon_pairing.stage(self.data, pairing, log))
        ini.unlink()
        self.assertIsNone(joycon_pairing.read(self.fat, log))
        self.assertTrue(joycon_pairing.stage(self.data, None, log))
        self.assertEqual(list(staged.iterdir()), [])

    def test_unusable_entries_are_skipped(self):
        pc = DUMP.replace("type=2", "type=0")
        other = DUMP.replace("host=98:B6:E9:12:34:56\nltk=FFEE", "host=98:B6:E9:65:43:21\nltk=FFEE")
        for text, names in ((pc, ["Joy-Con (L)"]), (other, ["Joy-Con (L)"]), ("[joycon_00]\ntype=0\n", None)):
            pairing = joycon_pairing.parse(text)
            got = [c["Name"] for c in pairing["controllers"]] if pairing and pairing["controllers"] else None
            self.assertEqual(got, names)


if __name__ == "__main__":
    unittest.main()
