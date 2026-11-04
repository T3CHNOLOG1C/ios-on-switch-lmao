"""The card guards on the real partition table and on broken variants of it (no device, no diskutil)."""
import base64
import json
import os
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

from python.runfiles import runfiles

from tools.install import card as cards
from tools.install import gpt

SPEC = {"partition_bytes": 32 << 30, "partition_sectors": (32 << 30) // 512}


def table(mutate=None):
    layout = json.loads(Path(runfiles.Create().Rlocation("_main/install/testdata/switch-card-1tb.json")).read_text())
    sectors = {r["lba"]: bytearray(base64.b64decode(r["base64"])) for r in layout["sectors"]}
    disk_sectors = layout["disk_bytes"] // 512
    if mutate:
        entries = sectors[2]
        mutate(entries, sectors[0])
        crc = zlib.crc32(bytes(entries[:10 * 128]))
        sectors[disk_sectors - 33][:] = entries
        for lba in (1, disk_sectors - 1):
            h = sectors[lba]
            h[88:92] = crc.to_bytes(4, "little")
            h[16:20] = bytes(4)
            h[16:20] = zlib.crc32(bytes(h[:92])).to_bytes(4, "little")
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "card.img")
        with open(path, "wb") as f:
            f.truncate(layout["disk_bytes"])
            for lba, data in sectors.items():
                f.seek(lba * 512)
                f.write(data)
        fd = os.open(path, os.O_RDONLY)
        try:
            t = gpt.read(fd, layout["disk_bytes"])
        finally:
            os.close(fd)
    card = cards.Card("disk99", {}, layout["disk_bytes"], "", t)
    return card


def retype(index, kind):
    def mutate(entries, mbr):
        entries[index * 128:index * 128 + 16] = kind.bytes_le
    return mutate


class CardTest(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.object(cards.host, "disk_info", return_value={"FilesystemType": "msdos"})
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_real_card_targets_userdata(self):
        card = table()
        target = cards.check_layout(card, SPEC)
        self.assertEqual((target.index, target.name), (8, "userdata"))

    def test_already_installed_card(self):
        card = table(retype(8, gpt.APFS))
        self.assertEqual(cards.check_layout(card, SPEC).index, 8)

    def test_refuses_wrong_size(self):
        def shrink(entries, mbr):
            last = int.from_bytes(entries[8 * 128 + 40:8 * 128 + 48], "little") - 1
            entries[8 * 128 + 40:8 * 128 + 48] = last.to_bytes(8, "little")
        with self.assertRaisesRegex(cards.Refused, "requires exactly"):
            cards.check_layout(table(shrink), SPEC)

    def test_refuses_second_apfs_partition(self):
        with self.assertRaisesRegex(cards.Refused, "other APFS"):
            cards.check_layout(table(retype(1, gpt.APFS)), SPEC)

    def test_refuses_without_hos_data(self):
        def no_fat(entries, mbr):
            entries[0:16] = gpt.LINUX_DATA.bytes_le
        with self.assertRaisesRegex(cards.Refused, "hos_data"):
            cards.check_layout(table(no_fat), SPEC)

    def test_refuses_without_hybrid_mbr(self):
        def protective(entries, mbr):
            pass
        card = table(protective)
        card.table.mbr_partitions = [p for p in card.table.mbr_partitions if p.kind == 0xEE]
        with self.assertRaisesRegex(cards.Refused, "hybrid MBR"):
            cards.check_layout(card, SPEC)

    def test_refuses_emummc_or_unknown_target(self):
        def rename(entries, mbr):
            entries[8 * 128 + 56:8 * 128 + 128] = "other".encode("utf-16-le").ljust(72, b"\0")
            entries[8 * 128 + 16:8 * 128 + 32] = bytes(15) + b"\1"
        with self.assertRaisesRegex(cards.Refused, "exactly one userdata"):
            cards.check_layout(table(rename), SPEC)

    def test_resolve_refuses_partitions_and_paths(self):
        for argument in ("disk4s9", "/Volumes/SWITCH SD", "/dev/disk4s1", "rdisk4s2"):
            with self.assertRaises(cards.Refused):
                cards.resolve(argument)


if __name__ == "__main__":
    unittest.main()
