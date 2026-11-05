"""Boot-entry and overlay sync, and the log pull, on plain directories (no card)."""
import os
import plistlib
import tempfile
import unittest
from pathlib import Path

from tools.deploy import joycon_pairing
from tools.deploy import poster_snapshots
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

    def test_pull_logs_copies_and_decodes_a_saved_panic(self):
        data = self.root / "data"
        (data / "ioswitch").mkdir(parents=True)
        (data / "ioswitch/panic-2026-11-05-120000.log").write_text(
            "IOSwitch panic record 1: ...\npanic: userspace watchdog timeout: no successful checkins from "
            "com.apple.backboardd (2 induced crashes) in 60 seconds\n")
        (data / "ioswitch/panic-2026-11-05-120000.kcdata").write_bytes(b"kcdata")
        # A stand-in for XNU's kcdata.py -s: the report a panic stackshot decodes to.
        report = {"binaryImages": [["590d839a-82d2-30f1-a17d-0cc053420f73", 6442450944, "S"],
                                   ["74f305ba-7e28-4e3f-6e7d-bd42b4ee7da1", 0, "U"]],
                  "processByPid": {"53": {"procname": "backboardd", "waitInfo": ["thread 900: semaphore port 0x1"],
                                          "threadById": {"900": {"name": "main", "state": ["TH_WAIT", "TH_UNINT"],
                                                                 "kernelFrames": [[1, 16]],
                                                                 "userFrames": [[0, 4096], [0, 8192]]}}},
                                   "27": {"procname": "SpringBoard", "threadById": {}}}}
        tool = self.root / "kcdata.py"
        tool.write_text("import json, sys\nopen(sys.argv[2], 'w').write('{}\\n' + json.dumps(%r))\n" % report)
        log = Log()
        out = pull_logs.pull(data, self.root / "logs", log, harvest=False, kcdata=tool)
        self.assertEqual((out / "panic-2026-11-05-120000.kcdata").read_bytes(), b"kcdata")
        self.assertTrue((out / "panic-2026-11-05-120000.stackshot.ips").is_file())
        summary = (out / "panic-2026-11-05-120000.threads.txt").read_text()
        self.assertIn("backboardd[53]: 1 threads", summary)
        self.assertIn("wait: thread 900: semaphore port 0x1", summary)
        self.assertIn("user: S:590d839a+0x1000 S:590d839a+0x2000", summary)
        self.assertIn("kernel: U:74f305ba+0x10", summary)
        self.assertNotIn("SpringBoard", summary)

    def test_panic_process_names(self):
        self.assertEqual(pull_logs.panic_process("panic: userspace watchdog timeout: no successful checkins from "
                                                 "com.apple.SpringBoard (0 induced crashes)"), "springboard")
        self.assertEqual(pull_logs.panic_process("panic: watchdog timeout: no checkins from watchdogd in 91 seconds"),
                         "watchdogd")
        self.assertEqual(pull_logs.panic_process("panic:  watchdogd[184] exited -- exit reason namespace 2"),
                         "watchdogd")
        self.assertIsNone(pull_logs.panic_process("panic: Kernel data abort."))


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


class PosterSnapshotsTest(unittest.TestCase):
    TILE = "SNAPSHOT_GALLERY-dC(b[{{0, 0}, {720, 1280}}]-s[1.000000]-o[4]-ui[2]-ax[0]-s[0])-cP(ab).atx"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.data = Path(self.tmp.name)
        apps = self.data / poster_snapshots.APPLICATIONS
        for uuid, identifier in (("A-SETTINGS", "com.apple.Preferences"), ("B-POSTERBOARD", "com.apple.PosterBoard")):
            (apps / uuid).mkdir(parents=True)
            with open(apps / uuid / poster_snapshots.METADATA, "wb") as handle:
                plistlib.dump({"MCMMetadataIdentifier": identifier}, handle)
        self.store = apps / "B-POSTERBOARD" / poster_snapshots.STORE / "61/Extensions"
        self.files = {}
        for bundle, kind, uuid, name in (
                ("com.apple.PridePoster.PridePosterExtension", "descriptors", "20E10D08", self.TILE),
                ("com.apple.PridePoster.PridePosterExtension", "descriptors", "20E10D08", "descriptor.plist"),
                ("com.apple.WatchFacesWallpaperSupport.KaleidoscopePoster", "staticdescriptors", "A063BC49", self.TILE),
                ("com.apple.WallpaperKit.CollectionsPoster", "configurations", "2AB99B7D",
                 "SNAPSHOT_GALLERY_WITH_COMPLICATIONS-x.atx"),
                ("com.apple.WallpaperKit.CollectionsPoster", "descriptors", "81552EBA", self.TILE),
                ("com.apple.EmojiPoster.EmojiPosterExtension", "descriptors", "B763AC6A", self.TILE)):
            path = self.store / bundle / kind / uuid / "versions/0" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"atx")
            self.files[(bundle, kind, name)] = path

    def tearDown(self):
        self.tmp.cleanup()

    def test_only_the_named_extensions_descriptor_tiles_go(self):
        bundles = ["com.apple.PridePoster.PridePosterExtension", "com.apple.WatchFacesWallpaperSupport.KaleidoscopePoster",
                   "com.apple.WallpaperKit.CollectionsPoster"]
        log = Log()
        self.assertEqual(poster_snapshots.reset(self.data, bundles, log, dry_run=True), 3)
        self.assertTrue(all(path.exists() for path in self.files.values()))
        self.assertEqual(poster_snapshots.reset(self.data, bundles, log), 3)
        gone = {key for key, path in self.files.items() if not path.exists()}
        self.assertEqual(gone, {("com.apple.PridePoster.PridePosterExtension", "descriptors", self.TILE),
                                ("com.apple.WatchFacesWallpaperSupport.KaleidoscopePoster", "staticdescriptors",
                                 self.TILE),
                                ("com.apple.WallpaperKit.CollectionsPoster", "descriptors", self.TILE)})
        self.assertIn("gallery snapshots: removed 1 of com.apple.PridePoster.PridePosterExtension", log.lines)
        self.assertEqual(poster_snapshots.reset(self.data, bundles, log), 0)

    def test_no_posterboard_container(self):
        log = Log()
        self.assertEqual(poster_snapshots.reset(self.root_without_posterboard(), ["x"], log), 0)
        self.assertIn("gallery snapshots: no PosterBoard container on Data", log.lines)

    def root_without_posterboard(self):
        root = self.data / "empty"
        root.mkdir()
        return root


if __name__ == "__main__":
    unittest.main()
