"""A run resumes only from a stage made of the same inputs, and a boot refresh only when Data's parts still match."""

import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from tools.install import folder, manifest, sii_fixture, stages

SPEC = {'volumes': {'system': {'uuid': 'S'}, 'data': {'uuid': 'D'}, 'preboot': {'uuid': 'P'}}}
PINS = {'build': '21H461', 'members': {'os': {'content_sha256': 'abc'}}}


def quiet(*parts: object):
    pass


class ScratchTest(unittest.TestCase):
    BUILDS = ('21H461', '24B119', '23F84', '22A3354')

    def test_legacy_build_keeps_the_unsuffixed_directory(self):
        self.assertEqual(stages.default_scratch('21H461').name, 'image')

    def test_each_build_has_its_own_directory(self):
        dirs = [stages.default_scratch(b) for b in self.BUILDS]
        self.assertEqual(len(set(dirs)), len(dirs))


class TranslationMemoTest(unittest.TestCase):
    def roles(self, **flags: object) -> stages.pretranslate.Roles | None:
        args = SimpleNamespace(scratch='/scratch', pretranslate={'os': {'driver': 'd', 'tool': 't'}}, **flags)
        return stages.Stages(args, SPEC, PINS, quiet).roles()

    def test_populate_translates_afresh_by_default(self):
        self.assertIsNone(self.roles().memo_root)

    def test_translation_memo_reads_the_scratchs_memo(self):
        self.assertEqual(self.roles(translation_memo=True).memo_root, Path('/scratch') / stages.TRANSLATIONS)


class StateTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.work = stages.Stages(SimpleNamespace(scratch=str(self.dir)), SPEC, PINS, quiet)

    def test_a_stage_counts_while_its_image_is_there(self):
        (self.dir / stages.COMPOSED).write_bytes(b'image')
        self.work.save(compose='c1', offset=1 << 20)
        self.assertEqual(self.work.state(), {'offset': 1 << 20, 'compose': 'c1'})
        self.work.save(populate='p1')
        self.assertNotIn('populate', self.work.state())
        (self.dir / stages.POPULATED).write_bytes(b'clone')
        self.assertEqual(self.work.state()['populate'], 'p1')
        (self.dir / stages.COMPOSED).unlink()
        self.assertNotIn('compose', self.work.state())


class StaleCryptexTest(unittest.TestCase):
    def test_a_previous_runs_rebuilt_cryptexes_are_stale_and_their_sources_are_not(self):
        scratch, ipsw = Path(tempfile.mkdtemp()), Path(tempfile.mkdtemp())
        for name in ('system.dmg', 'app.dmg'):
            (ipsw / name).write_bytes(b'apple')
        (scratch / 'system.dmg').write_bytes(b'rebuilt')
        args = SimpleNamespace(
            scratch=str(scratch), system_cryptex=str(ipsw / 'system.dmg'), app_cryptex=str(ipsw / 'app.dmg')
        )
        self.assertEqual(stages.Stages(args, SPEC, PINS, quiet).stale_cryptexes(), [scratch / 'system.dmg'])
        args.app_cryptex = str(scratch / 'system.dmg')
        args.system_cryptex = str(scratch / 'system.dmg')
        self.assertEqual(stages.Stages(args, SPEC, PINS, quiet).stale_cryptexes(), [])


class MinimumTest(unittest.TestCase):
    def test_room_to_run_and_ios_floor(self):
        layout = {'container_bytes': 1 << 40, 'minimum_bytes': 9 << 30}
        self.assertEqual(stages.minimum_bytes(layout), 16 << 30)
        layout['minimum_bytes'] = (20 << 30) + 5
        self.assertEqual(stages.minimum_bytes(layout), (24 << 30) + 4096)
        self.assertEqual(stages.minimum_bytes({'container_bytes': 8 << 30, 'minimum_bytes': 1 << 30}), 8 << 30)


class HeadTest(unittest.TestCase):
    def test_new_boot_files_change_the_head_only(self):
        out = folder.prepare(Path(tempfile.mkdtemp()) / 'sd')
        sii_fixture.make(out, 'IOS17', bytes([7]) * (90 << 20), {'bl33.bin': b'kernel 1'})
        parts = sorted((out / folder.SII_DIR).glob('IOS17.SII.*'))
        before = [p.read_bytes() for p in parts]
        package = out.parent / 'kernel'
        package.write_bytes(b'kernel 2, longer')
        m = stages.refresh_head(out, 'IOS17', [('bl33.bin', package)], 'iPadOS 17.7.11 (21H461)')
        after = [p.read_bytes() for p in parts]
        self.assertEqual(after[1:], before[1:])
        self.assertEqual(after[0][manifest.BODY_OFFSET :], before[0][manifest.BODY_OFFSET :])
        self.assertEqual(manifest.decode(after[0][: manifest.BOOT_OFFSET]), m)
        start = m.boot[0].offset
        self.assertEqual(after[0][start : start + 16], b'kernel 2, longer')

    def test_boot_files_are_the_packages_boot_entry(self):
        package = Path(tempfile.mkdtemp())
        for relative in ('switchroot/ios/bl33.bin', 'switchroot/ios/bl31.bin', 'bootloader/ini/ios.ini'):
            (package / relative).parent.mkdir(parents=True, exist_ok=True)
            (package / relative).write_bytes(b'x')
        self.assertEqual([n for n, _ in stages.boot_files(package)], ['bl31.bin', 'bl33.bin'])


class IpswVolumesTest(unittest.TestCase):
    def test_the_pins_name_system_and_the_cryptexes(self):
        spec = {
            'volumes': {'system': {'uuid': 'OLD'}, 'data': {'uuid': 'D'}, 'preboot': {'uuid': 'P'}},
            'cryptexes': [
                {'member': 'system_cryptex', 'path': '/private/preboot/Switch-Staging/a.dmg', 'volume_uuid': 'A'},
                {'member': 'app_cryptex', 'path': '/private/preboot/Switch-Staging/b.dmg', 'volume_uuid': 'B'},
            ],
        }
        pins = {
            'volumes': {
                'os_volume_uuid': 'd05a9d99-d227-4757-9f58-c32593d3bfa7',
                'system_cryptex_uuid': 'adb8fd34-da7e-4538-9aaf-4f05ebb09057',
                'app_cryptex_uuid': 'd8b9f68e-f191-4fcd-9f89-d88a322e9b4d',
                'system_cryptex_image': '141-10410-227.dmg',
                'app_cryptex_image': '141-08824-244.dmg',
            }
        }
        out = stages.ipsw_volumes(spec, pins)
        self.assertEqual(out['volumes']['system']['uuid'], 'D05A9D99-D227-4757-9F58-C32593D3BFA7')
        self.assertEqual(out['volumes']['data']['uuid'], 'D')
        self.assertEqual(out['cryptexes'][0]['path'], '/private/preboot/Switch-Staging/141-10410-227.dmg')
        self.assertEqual(out['cryptexes'][1]['volume_uuid'], 'D8B9F68E-F191-4FCD-9F89-D88A322E9B4D')
        self.assertEqual(spec['volumes']['system']['uuid'], 'OLD')
        self.assertIs(stages.ipsw_volumes(spec, {}), spec)


if __name__ == '__main__':
    unittest.main()
