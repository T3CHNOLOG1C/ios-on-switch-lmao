"""The container fit against macOS itself: a container grown to the nominal size, fitted to partitions of other sizes,
must attach in the kernel at that size and pass fsck_apfs, and a fit below the minimum must change nothing.

Manual and local: hdiutil and diskutil on sparse images (about 1 TiB each, a few MB on disk), no root.
"""

import os
import plistlib
import shutil
import subprocess
import tempfile
import unittest
import uuid
from pathlib import Path

from python.runfiles import runfiles

from tools.install import gpt

BLOCK = 4096
NOMINAL = 1 << 40
FIRST = 2048
GIB = 1 << 30


def run(*args: str) -> str:
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def attach(image: Path) -> str:
    out = plistlib.loads(
        subprocess.run(
            ['/usr/bin/hdiutil', 'attach', '-plist', '-nomount', '-imagekey', 'diskimage-class=CRawDiskImage', str(image)],
            capture_output=True,
            check=True,
        ).stdout
    )
    return min(e['dev-entry'] for e in out['system-entities'])


def container_size(disk: str) -> int | None:
    listing = plistlib.loads(subprocess.run(['/usr/sbin/diskutil', 'apfs', 'list', '-plist'], capture_output=True).stdout)
    for c in listing.get('Containers', []):
        if any(s.get('DeviceIdentifier', '').startswith(disk.removeprefix('/dev/') + 's') for s in c['PhysicalStores']):
            return c.get('CapacityCeiling')
    return None


class FitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool = runfiles.Create().Rlocation('_main/boot/installer/apfs_tool')
        cls.dir = Path(tempfile.mkdtemp())
        cls.nominal = cls.dir / 'nominal.img'
        with open(cls.nominal, 'wb') as f:
            f.truncate(NOMINAL + 2 * GIB)
        disk = attach(cls.nominal)
        try:
            run('/usr/sbin/diskutil', 'partitionDisk', disk, 'GPT', 'APFS', 'fit', '6G', 'free', 'free', 'R')
            store = disk + 's2'
            run('/usr/sbin/diskutil', 'apfs', 'resizeContainer', store, f'{NOMINAL}B')
        finally:
            run('/usr/bin/hdiutil', 'detach', disk)
        fd = os.open(cls.nominal, os.O_RDONLY)
        cls.offset = gpt.read(fd, os.fstat(fd).st_size).used[-1].first * 512
        os.close(fd)
        size, minimum = map(int, run(cls.tool, str(cls.nominal), str(cls.offset)).split())
        cls.size, cls.minimum = size, minimum

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir)

    def fitted(self, blocks: int) -> Path:
        """A partition of exactly `blocks` holding the nominal container fitted to it."""
        copy = self.dir / 'copy.img'
        subprocess.run(['/bin/cp', '-c', str(self.nominal), str(copy)], check=True)
        run(self.tool, str(copy), str(self.offset), str(blocks))
        out = self.dir / f'{blocks}.img'
        sectors = FIRST + blocks * 8 + 2048
        with open(out, 'wb') as f:
            f.truncate(sectors * 512)
        src, dst = os.open(copy, os.O_RDONLY), os.open(out, os.O_RDWR)
        for lba, data in gpt.build(sectors, [(gpt.APFS, uuid.uuid4(), FIRST, FIRST + blocks * 8 - 1, 'iOS')]):
            os.pwrite(dst, data, lba * 512)
        at = 0
        while at < blocks * BLOCK:
            try:
                data = os.lseek(src, self.offset + at, os.SEEK_DATA) - self.offset
            except OSError:
                break
            if data >= blocks * BLOCK:
                break
            hole = min(os.lseek(src, self.offset + data, os.SEEK_HOLE) - self.offset, blocks * BLOCK)
            os.pwrite(dst, os.pread(src, hole - data, self.offset + data), FIRST * 512 + data)
            at = hole
        os.close(src)
        os.close(dst)
        copy.unlink()
        return out

    def check(self, blocks: int) -> None:
        image = self.fitted(blocks)
        disk = attach(image)
        try:
            self.assertEqual(container_size(disk), blocks * BLOCK)
            store = disk.removeprefix('/dev/') + 's1'
            subprocess.run(['/usr/sbin/diskutil', 'unmountDisk', store], capture_output=True)
            fsck = subprocess.run(['/sbin/fsck_apfs', '-n', '/dev/r' + store], capture_output=True, text=True)
            self.assertIn('appears to be OK', fsck.stdout + fsck.stderr)
        finally:
            run('/usr/bin/hdiutil', 'detach', disk)
            image.unlink()

    def test_the_nominal_size(self):
        self.assertEqual(self.size * BLOCK, NOMINAL)
        self.assertLess(self.minimum * BLOCK, 16 * GIB)

    def test_fits_on_both_sides_of_the_pool_queue_limit(self):
        for size in (16 * GIB, 100 * GIB + 12 * BLOCK, 500 * GIB, NOMINAL - 7 * 32768 * BLOCK):
            with self.subTest(size=size):
                self.check(size // BLOCK)

    def test_below_the_minimum_nothing_is_written(self):
        result = subprocess.run(
            [self.tool, str(self.nominal), str(self.offset), str(self.minimum - 1)], capture_output=True
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(run(self.tool, str(self.nominal), str(self.offset)).split(), [str(self.size), str(self.minimum)])


if __name__ == '__main__':
    unittest.main()
