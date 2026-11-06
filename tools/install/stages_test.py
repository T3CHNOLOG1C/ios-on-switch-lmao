"""A run resumes only from a stage made of the same inputs, and a boot refresh only when Data's parts still match."""

import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from tools.install import folder, host, stages

SPEC = {'volumes': {'system': {'uuid': 'S'}, 'data': {'uuid': 'D'}, 'preboot': {'uuid': 'P'}}}
PINS = {'build': '21H461', 'members': {'os': {'content_sha256': 'abc'}}}


def quiet(*parts):
    pass


class StateTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.args = SimpleNamespace(scratch=str(self.dir))
        self.work = stages.Stages(self.args, SPEC, PINS, quiet)
        (self.dir / stages.IMAGE).write_bytes(b'image')
        self.work.save('compose', 32 << 30, 1 << 20)

    def test_resumes_the_same_inputs(self):
        self.assertEqual(self.work.state(32 << 30), ('compose', 1 << 20))

    def test_starts_again_for_other_inputs(self):
        other = stages.Stages(self.args, SPEC, dict(PINS, build='24B119'), quiet)
        self.assertEqual(other.state(32 << 30), (None, None))
        self.assertEqual(self.work.state(64 << 30), (None, None))

    def test_refuses_a_card_of_another_size(self):
        with self.assertRaisesRegex(host.HostError, '32768 MiB'):
            self.work.state(64 << 30, refuse_other_size=True)

    def test_no_image_no_state(self):
        (self.dir / stages.IMAGE).unlink()
        self.assertEqual(self.work.state(32 << 30), (None, None))


class RefreshTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.package = self.dir / 'package'
        for relative, data in {
            'switchroot/ios/bl33.bin': b'kernel 1',
            'bootloader/ini/ios.ini': b'[iOS]\n',
            'overlay/usr/bin/tool': b'tool',
        }.items():
            (self.package / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.package / relative).write_bytes(data)
        self.gpu = self.dir / 'gpu'
        self.gpu.mkdir()
        (self.gpu / 'TegraGM20BMetal.trustcache').write_bytes(b'trust')
        self.payload = self.dir / 'install-ios.bin'
        self.payload.write_bytes(b'payload')
        self.args = SimpleNamespace(
            boot_package=str(self.package), gpu_base=str(self.gpu), install_payload=str(self.payload)
        )
        self.out = folder.prepare(self.dir / 'out')
        (self.out / folder.INSTALL / 'part-00.bin').write_bytes(b'part')
        stages.add_boot(self.out, self.args, 'bootloader/payloads/install-ios.bin')
        record = {'label': 'x', 'partition_bytes': 1, 'boot': stages.boot_record(self.args)}
        (self.out / folder.MARKER).write_text(json.dumps(record))

    def test_a_kernel_change_refreshes_the_boot_entry_only(self):
        (self.package / 'switchroot/ios/bl33.bin').write_bytes(b'kernel 2')
        stages.refresh_boot(self.out, self.args, 'bootloader/payloads/install-ios.bin', quiet)
        self.assertEqual((self.out / 'switchroot/ios/bl33.bin').read_bytes(), b'kernel 2')
        self.assertEqual((self.out / folder.INSTALL / 'part-00.bin').read_bytes(), b'part')

    def test_refuses_when_the_overlay_changed(self):
        (self.package / 'overlay/usr/bin/tool').write_bytes(b'tool 2')
        with self.assertRaisesRegex(host.HostError, 'overlay'):
            stages.refresh_boot(self.out, self.args, 'bootloader/payloads/install-ios.bin', quiet)

    def test_refuses_when_the_gpu_trust_cache_changed(self):
        (self.gpu / 'TegraGM20BMetal.trustcache').write_bytes(b'trust 2')
        with self.assertRaisesRegex(host.HostError, 'gpu_trustcache'):
            stages.refresh_boot(self.out, self.args, 'bootloader/payloads/install-ios.bin', quiet)


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
