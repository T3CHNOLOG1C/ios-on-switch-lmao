"""One kind's image folder, made and merged onto a card holding the other kind's, on plain directories (no card)."""

import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.install import folder, host

OURS = '21H461'
OTHER = '24B119'


def quiet(*parts: object) -> None:
    pass


def write(root: Path, relative: str, data: bytes = b'x') -> None:
    (root / relative).parent.mkdir(parents=True, exist_ok=True)
    (root / relative).write_bytes(data)


def tree(root: Path) -> dict[str, bytes]:
    return {p.relative_to(root).as_posix(): p.read_bytes() for p in root.rglob('*') if p.is_file()}


class PrepareTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.out = self.dir / 'sd'

    def test_keeps_the_other_kinds_image_and_nothing_else(self):
        folder.prepare(self.out, OTHER)
        write(self.out, f'{folder.kind_dir(OTHER)}/part-00.bin')
        folder.record(self.out, OTHER, {'label': 'iPadOS 27.2 (24B119)'})
        write(self.out, folder.PAYLOAD)
        write(self.out, 'pretranslated/trust.json')
        folder.prepare(self.out, OURS)
        write(self.out, f'{folder.kind_dir(OURS)}/part-00.bin', b'ours')
        folder.prepare(self.out, OURS)
        self.assertEqual(tree(self.out).keys() - {folder.MARKER}, {f'{folder.kind_dir(OTHER)}/part-00.bin'})
        self.assertTrue((self.out / folder.kind_dir(OURS)).is_dir())
        self.assertEqual(list(folder.records(self.out)), [OTHER])

    def test_a_folder_from_before_one_folder_per_kind_starts_afresh(self):
        write(self.out, folder.MARKER, b'{"label": "x", "boot": {}}')
        write(self.out, f'{folder.INSTALL}/part-00.bin')
        write(self.out, 'switchroot/ios/bl33.bin')
        folder.prepare(self.out, OURS)
        self.assertEqual(set(tree(self.out)), {folder.MARKER})

    def test_refuses_a_folder_it_did_not_make(self):
        write(self.out, 'notes.txt')
        with self.assertRaisesRegex(host.HostError, 'not made by'):
            folder.prepare(self.out, OURS)

    def test_refuses_a_build_that_is_no_folder_name(self):
        with self.assertRaises(host.HostError):
            folder.kind_dir('../24B119')


class MergeTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.out = folder.prepare(self.dir / 'sd', OURS)
        mine = folder.kind_dir(OURS)
        for relative, data in {
            f'{mine}/manifest.bin': b'manifest 2',
            f'{mine}/part-00.bin': b'part 0 again',
            f'{mine}/bl33.bin': b'kernel 17',
            folder.PAYLOAD: b'payload',
            'bootloader/ini/ios.ini': b'[iOS]\n',
        }.items():
            write(self.out, relative, data)
        self.fat = self.dir / 'fat'
        theirs = folder.kind_dir(OTHER)
        self.untouched = {
            'switchroot/ios/bl31.bin': b'active bl31',
            'switchroot/ios/bl33.bin': b'active kernel 27',
            'switchroot/ios/bootargs.txt': b'-v',
            f'{theirs}/manifest.bin': b'manifest 27',
            f'{theirs}/part-00.bin': b'part 27',
            f'{theirs}/bl33.bin': b'kernel 27',
            'switchroot/ubuntu/boot.scr': b'linux',
            'bootloader/hekate_ipl.ini': b'[config]\n',
        }
        for relative, data in self.untouched.items():
            write(self.fat, relative, data)
        for relative in (f'{mine}/part-00.bin', f'{mine}/part-01.bin', f'{mine}/._bl33.bin'):
            write(self.fat, relative, b'old')
        for relative in (f'{folder.INSTALL}/manifest.bin', f'{folder.INSTALL}/part-00.bin'):
            write(self.fat, relative, b'older layout')

    def merge(self, keep: tuple[str, ...] | list[str] = ()) -> tuple[list[str], list[str]]:
        with mock.patch.object(folder.host, 'run'):
            return folder.merge(self.out, self.fat, quiet, keep)

    def test_replaces_only_its_kinds_image(self):
        changed, removed = self.merge()
        mine = folder.kind_dir(OURS)
        files = tree(self.fat)
        for relative, data in self.untouched.items():
            self.assertEqual(files[relative], data, relative)
        self.assertEqual(files[f'{mine}/part-00.bin'], b'part 0 again')
        self.assertEqual(
            sorted(removed),
            [
                f'{mine}/._bl33.bin',
                f'{mine}/part-01.bin',
                f'{folder.INSTALL}/manifest.bin',
                f'{folder.INSTALL}/part-00.bin',
            ],
        )
        self.assertEqual(len(changed), 5)
        self.assertEqual(self.merge()[0], [])

    def test_keeps_the_parts_packed_straight_onto_the_card(self):
        (self.out / folder.kind_dir(OURS) / 'part-00.bin').unlink()
        _, removed = self.merge(keep=[f'{folder.kind_dir(OURS)}/part-01.bin'])
        self.assertNotIn(f'{folder.kind_dir(OURS)}/part-01.bin', removed)
        self.assertIn(f'{folder.kind_dir(OURS)}/part-00.bin', removed)

    def test_what_the_folder_may_hold(self):
        mine, source = folder.kind_dir(OURS), self.dir / 'source'
        source.write_bytes(b'x')
        for allowed in (f'{mine}/bl31.bin', folder.PAYLOAD, 'bootloader/res/icon_ios_hue.bmp'):
            folder.add(self.out, allowed, source)
        for refused in ('switchroot/ios/bl33.bin', f'{folder.INSTALL}/manifest.bin', f'{mine}/sub/x', 'boot.ini'):
            with self.subTest(refused), self.assertRaises(host.HostError):
                folder.add(self.out, refused, source)


class ListingTest(unittest.TestCase):
    def test_lists_the_kinds_folder_and_never_the_active_entry(self):
        package = Path(tempfile.mkdtemp())
        for relative in ('switchroot/ios/bl31.bin', 'switchroot/ios/bl33.bin', 'bootloader/ini/ios.ini'):
            write(package, relative)
        mine = folder.kind_dir(OURS)
        self.assertEqual(
            folder.listing(package, OURS),
            [
                'bootloader/ini/ios.ini',
                folder.PAYLOAD,
                f'{mine}/bl31.bin',
                f'{mine}/bl33.bin',
                f'{mine}/manifest.bin',
                f'{mine}/part-NN.bin',
            ],
        )


if __name__ == '__main__':
    unittest.main()
