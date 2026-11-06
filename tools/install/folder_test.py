"""out/sd's images onto a card that holds the other version's, by the parts that differ (no device)."""

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.install import folder, host, manifest, sii_fixture

MIB = 1 << 20


def quiet(*parts: object) -> None:
    pass


def write(root: Path, relative: str, data: bytes = b'x') -> None:
    (root / relative).parent.mkdir(parents=True, exist_ok=True)
    (root / relative).write_bytes(data)


def body(seed: int, mib: int = 50) -> bytes:
    return bytes([seed]) * (mib * MIB)


class PrepareTest(unittest.TestCase):
    def test_keeps_images_and_drops_the_older_layout(self):
        out = Path(tempfile.mkdtemp()) / 'sd'
        folder.prepare(out)
        write(out, f'{folder.SII_DIR}/IOS27.SII.001')
        write(out, f'{folder.LEGACY}/part-00.bin')
        folder.prepare(out)
        self.assertTrue((out / folder.SII_DIR / 'IOS27.SII.001').is_file())
        self.assertFalse((out / folder.LEGACY).exists())

    def test_refuses_a_folder_it_did_not_make(self):
        out = Path(tempfile.mkdtemp())
        write(out, 'notes.txt')
        with self.assertRaisesRegex(host.HostError, 'not made by'):
            folder.prepare(out)

    def test_what_the_folder_may_hold(self):
        out = folder.prepare(Path(tempfile.mkdtemp()) / 'sd')
        source = out.parent / 'source'
        source.write_bytes(b'x')
        for allowed in (f'{folder.SII_DIR}/IOS17.SII.002', folder.PAYLOAD, 'bootloader/res/icon_ios_hue.bmp'):
            folder.add(out, allowed, source)
        for refused in ('switchroot/ios/bl33.bin', 'switchroot/ios/IOS17.SII', f'{folder.LEGACY}/manifest.bin'):
            with self.subTest(refused), self.assertRaises(host.HostError):
                folder.add(out, refused, source)


class MergeTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.out = folder.prepare(self.dir / 'sd')
        sii_fixture.make(self.out, 'IOS17', body(1), {'bl33.bin': b'kernel 17'})
        write(self.out, folder.PAYLOAD, b'payload')
        self.fat = self.dir / 'fat'
        self.untouched = {
            'switchroot/ios/bl33.bin': b'active kernel 27',
            'switchroot/ios/bootargs.txt': b'-v',
            'switchroot/ubuntu/boot.scr': b'linux',
        }
        for relative, data in self.untouched.items():
            write(self.fat, relative, data)
        sii_fixture.make(self.fat, 'IOS27', body(2, 10), {'bl33.bin': b'kernel 27'})
        write(self.fat, f'{folder.LEGACY}/manifest.bin')

    def merge(self) -> tuple[list[str], list[str]]:
        with mock.patch.object(folder.host, 'run'):
            return folder.merge(self.out, self.fat, ['IOS17'], quiet)

    def files(self, root: Path) -> dict[str, bytes]:
        return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob('*') if p.is_file()}

    def test_a_new_image_goes_on_whole_and_nothing_else_changes(self):
        theirs = self.files(self.fat / folder.SII_DIR)
        written, removed = self.merge()
        self.assertEqual(written, ['switchroot/ios/IOS17.SII.001', 'switchroot/ios/IOS17.SII.002', folder.PAYLOAD])
        self.assertIn(f'{folder.LEGACY}/manifest.bin', removed)
        files = self.files(self.fat)
        for relative, data in self.untouched.items():
            self.assertEqual(files[relative], data)
        for name, data in theirs.items():
            if name.startswith('IOS27'):
                self.assertEqual(files[f'{folder.SII_DIR}/{name}'], data)
        for part in ('001', '002'):
            mine = (self.out / folder.SII_DIR / f'IOS17.SII.{part}').read_bytes()
            self.assertEqual(files[f'{folder.SII_DIR}/IOS17.SII.{part}'], mine)
        self.assertEqual(self.merge(), ([], []))

    def test_new_boot_files_rewrite_only_the_head(self):
        self.merge()
        sii_fixture.make(self.out, 'IOS17', body(1), {'bl33.bin': b'kernel 17, again'})
        copies, stale = folder.copy_plan(self.out, self.fat, 'IOS17')
        self.assertEqual((copies, stale), ([('switchroot/ios/IOS17.SII.001', 0, manifest.BODY_OFFSET)], []))
        self.merge()
        mine = (self.out / folder.SII_DIR / 'IOS17.SII.001').read_bytes()
        self.assertEqual((self.fat / folder.SII_DIR / 'IOS17.SII.001').read_bytes(), mine)

    def test_a_changed_tail_copies_its_part_and_the_head(self):
        self.merge()
        sii_fixture.make(self.out, 'IOS17', body(1, 49) + bytes([9]) * MIB, {'bl33.bin': b'kernel 17'})
        copies, _ = folder.copy_plan(self.out, self.fat, 'IOS17')
        self.assertEqual(
            [(r, e) for r, _, e in copies],
            [('switchroot/ios/IOS17.SII.001', manifest.BODY_OFFSET), ('switchroot/ios/IOS17.SII.002', 42 * MIB)],
        )

    def test_a_shorter_image_removes_its_stale_parts(self):
        self.merge()
        for stale in (self.out / folder.SII_DIR).glob('IOS17.SII.*'):
            os.unlink(stale)
        sii_fixture.make(self.out, 'IOS17', body(1, 4), {'bl33.bin': b'kernel 17'})
        _, removed = self.merge()
        self.assertEqual(removed, ['switchroot/ios/IOS17.SII.002'])
        self.assertEqual(sorted(folder.images(self.fat)), ['IOS17', 'IOS27'])


class NameTest(unittest.TestCase):
    def test_one_image_per_ios_version(self):
        self.assertEqual(folder.image_name({'version': '17.7.11'}), 'IOS17')
        self.assertEqual(folder.image_name({'version': '27.2'}), 'IOS27')


if __name__ == '__main__':
    unittest.main()
