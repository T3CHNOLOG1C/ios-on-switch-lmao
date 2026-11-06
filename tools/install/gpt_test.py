"""GPT reading and the type/name edit, on the user's card's real partition table (no device)."""

import base64
import json
import os
import tempfile
import unittest
from pathlib import Path

from python.runfiles import runfiles

from tools.install import gpt

LAYOUT = '_main/install/testdata/switch-card-1tb.json'


def card_image(directory):
    layout = json.loads(Path(runfiles.Create().Rlocation(LAYOUT)).read_text())
    path = Path(directory) / 'card.img'
    with open(path, 'wb') as f:
        f.truncate(layout['disk_bytes'])
        for record in layout['sectors']:
            f.seek(record['lba'] * 512)
            f.write(base64.b64decode(record['base64']))
    return path, layout['disk_bytes']


class GptTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.path, self.size = card_image(self.tmp.name)
        self.fd = os.open(self.path, os.O_RDWR)

    def tearDown(self):
        os.close(self.fd)
        self.tmp.cleanup()

    def test_reads_the_switch_card(self):
        table = gpt.read(self.fd, self.size)
        names = [e.name for e in table.used]
        self.assertEqual(
            names, ['hos_data', 'l4t', 'boot', 'recovery', 'dtb', 'misc', 'cache', 'super', 'userdata', 'emummc']
        )
        self.assertTrue(table.hybrid())
        userdata = table.entries[8]
        self.assertEqual(userdata.type, gpt.LINUX_DATA)
        self.assertEqual(userdata.sectors, 67108864)
        self.assertEqual(table.entries[9].type, gpt.EMUMMC)

    def test_edit_changes_only_the_entry_and_headers(self):
        table = gpt.read(self.fd, self.size)
        before = {
            lba: os.pread(self.fd, 512, lba * 512)
            for lba in [
                0,
                1,
                2,
                3,
                4,
                table.disk_sectors - 33,
                table.disk_sectors - 32,
                table.disk_sectors - 31,
                table.disk_sectors - 1,
            ]
        }
        writes = gpt.edit(table, 8, gpt.APFS, 'iOS')
        self.assertNotIn(0, [lba for lba, _ in writes])
        gpt.apply(self.fd, writes)
        after = gpt.read(self.fd, self.size)
        self.assertEqual(after.entries[8].type, gpt.APFS)
        self.assertEqual(after.entries[8].name, 'iOS')
        for i, entry in enumerate(after.entries):
            if i != 8:
                self.assertEqual(entry, table.entries[i])
        self.assertEqual(os.pread(self.fd, 512, 0), before[0], 'the hybrid MBR must not change')
        # entry 8 lives in the third sector of each array (128-byte entries, 4 per sector): sectors 2 and 3 untouched
        self.assertEqual(os.pread(self.fd, 512, 2 * 512), before[2])
        self.assertEqual(os.pread(self.fd, 512, 3 * 512), before[3])
        self.assertNotEqual(os.pread(self.fd, 512, 4 * 512), before[4])

    def test_corrupt_backup_is_refused(self):
        table = gpt.read(self.fd, self.size)
        os.pwrite(self.fd, b'\xff' * 16, table.backup_entries_lba * 512)
        with self.assertRaises(gpt.GptError):
            gpt.read(self.fd, self.size)

    def test_refuses_lba0_writes(self):
        with self.assertRaises(gpt.GptError):
            gpt.apply(self.fd, [(0, bytes(512))])


if __name__ == '__main__':
    unittest.main()
