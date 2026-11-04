"""Boot-entry and overlay sync, and the log pull, on plain directories (no card)."""
import os
import tempfile
import unittest
from pathlib import Path

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


if __name__ == "__main__":
    unittest.main()
